#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "AudioFormatDef.h"
#include "Engine.h"        // BackendKind
#include "IAudioBackend.h" // IAudioBackend, DataFlow, DeviceId
#include "Result.h"
#include "StreamParams.h"

namespace wa {

class RingBuffer;
class ScopeBuffer;
class DelayFifo;

enum class StreamState { Idle, Running, Error };

// Numeric reasons surfaced in MonitorStatus::errorCode (kept lock-free; the GUI
// maps these to static text instead of carrying a std::string through poll()).
enum class MonitorError : uint32_t {
    None = 0,
    Factory,
    CaptureOpen,
    CaptureStart,
    RenderOpen,
    RenderStart,
    RateMismatch,
    PumpLaunch,
    InvalidDelay,
    LoopbackFeedback,
};

struct MonitorStatus {
    StreamState overall     = StreamState::Idle;
    StreamState capState    = StreamState::Idle;
    StreamState renderState = StreamState::Idle;
    StreamState silentRenderState = StreamState::Idle;
    uint32_t    sampleRate  = 0;
    uint32_t    captureChannels = 0;
    uint32_t    delayMs     = 0;
    float       fifoFillMs  = 0.f;   // EMA-smoothed DelayFifo occupancy in ms
    uint32_t    renderBufMs = 0;     // render device buffering (shown separately)
    uint64_t    driftFixes  = 0;
    uint64_t    capXruns    = 0;
    uint64_t    renderXruns = 0;     // = renderRing overruns (not counted during prefill)
    uint64_t    capWrittenFrames = 0;
    uint64_t    renderWrittenFrames = 0;
    uint64_t    loopbackIdleSilenceFrames = 0;
    uint64_t    loopbackSilentPacketFrames = 0;
    float       capLevel    = 0.f;
    float       renderLevel = 0.f;
    uint32_t    errorCode   = 0;     // MonitorError
};

// Dual-stream delayed monitor pass-through: capture -> frame-domain DelayFifo
// (drift-controlled) -> render, with two scope taps for visualization. A single
// "pump" thread does all conversion/downmix/framing/drift; the capture/render
// backends only move bytes. A BackendFactory seam makes the pump unit-testable
// with a fake backend (no WASAPI hardware).
class MonitorEngine {
public:
    // `source` is non-null only for capture backend creation and is valid only
    // during the factory call; render backend creation passes nullptr.
    using BackendFactory =
        std::function<std::unique_ptr<IAudioBackend>(DataFlow, const CaptureSource*,
                                                     const AudioFormat*)>;
    using SilentRenderFactory =
        std::function<std::unique_ptr<IAudioBackend>(const AudioFormat*)>;

    explicit MonitorEngine(BackendFactory factory = {},
                           SilentRenderFactory silentFactory = {});
    ~MonitorEngine();

    MonitorEngine(const MonitorEngine&)            = delete;
    MonitorEngine& operator=(const MonitorEngine&) = delete;

    Result start(BackendKind kind, const CaptureSource& capSource, const DeviceId& renderId,
                 uint32_t delayMs, bool playbackEnabled = true,
                 const StreamParams& capParams = {}, const StreamParams& renderParams = {},
                 const AudioFormat* capFormat = nullptr,
                 const LoopbackOptions& loopbackOptions = {});
    Result start(BackendKind kind, const DeviceId& capId, const DeviceId& renderId,
                 uint32_t delayMs, bool playbackEnabled = true,
                 const StreamParams& capParams = {}, const StreamParams& renderParams = {},
                 const AudioFormat* capFormat = nullptr) {
        return start(kind, CaptureSource{CaptureSourceKind::Endpoint, capId}, renderId,
                     delayMs, playbackEnabled, capParams, renderParams, capFormat, {});
    }
    void   stop();
    MonitorStatus poll();
    void   setPlaybackEnabled(bool enabled);                        // 运行期实时开关（GUI 线程）
    void   setRenderParams(const StreamParams& p);                  // 运行中可调;下次 engage 取快照生效

    bool snapshotCapture(size_t n, float* out, uint64_t& endIdxOut);
    bool snapshotCaptureChannel(uint16_t channel, size_t n, float* out, uint64_t& endIdxOut);
    bool snapshotCaptureChannelAt(uint16_t channel, uint64_t endIdx, size_t n, float* out);
    bool snapshotRender (size_t n, float* out, uint64_t& endIdxOut);

    uint64_t capWritten()    const;
    uint64_t renderWritten() const;

private:
    void   pumpLoop();
    void   teardown();   // stop+join pump, stop+free backends/rings/scopes/fifo
    Result rollback(StreamState finalState, MonitorError err, long code, std::string msg);
    Result guardLoopbackFeedback();
    std::unique_ptr<IAudioBackend> makeBackend(DataFlow flow, BackendKind kind,
                                               const CaptureSource* source,
                                               const AudioFormat* requested);
    std::unique_ptr<IAudioBackend> makeSilentRenderBackend(const AudioFormat* requested);
    Result startSilentRenderIfNeeded();
    void   stopSilentRender();
    Result engageRender();      // pump 前(start) 或 pump 线程调用；开渲染+校验+建 FIFO/刮擦；失败原子(全关渲染)
    void   disengageRender();   // 停+关渲染、释放设备、reset renderRing_/delayFifo_；不动 renderScope_

    BackendFactory factory_; // empty => build real WASAPI streams
    SilentRenderFactory silentFactory_;

    // Run-const session parameters (set in start(), consumed in engageRender()).
    BackendKind kind_{BackendKind::WasapiShared};
    CaptureSource capSource_{};
    DeviceId    renderId_{};
    uint32_t    delayMs_ = 0;
    LoopbackOptions loopbackOptions_{};
    std::atomic<bool> wantPlayback_{false};
    StreamParams capParams_{};      // start 时消费(采集参数改动需 Stop/Start)
    StreamParams renderParams_{};   // paramsMtx_ 保护;engageRender 取快照
    std::mutex   paramsMtx_;
    AudioFormat  capRequestedFormat_{};
    bool         hasCapFormat_ = false;

    std::unique_ptr<IAudioBackend> capBackend_;
    std::unique_ptr<IAudioBackend> renderBackend_;
    std::unique_ptr<IAudioBackend> silentRenderBackend_;
    std::unique_ptr<RingBuffer>    captureRing_;
    std::unique_ptr<RingBuffer>    renderRing_;
    std::unique_ptr<ScopeBuffer>   captureScope_;
    std::unique_ptr<ScopeBuffer>   renderScope_;
    std::unique_ptr<DelayFifo>     delayFifo_;

    std::thread       pump_;
    std::atomic<bool> running_{false};
    std::atomic<bool> prefilled_{false};

    // Run-constant config (published to the pump via thread creation).
    AudioFormat capFmt_{};
    AudioFormat renderFmt_{};
    uint32_t    capFrameBytes_    = 0;
    uint32_t    renderFrameBytes_ = 0;
    uint16_t    capCh_            = 0;
    uint16_t    renderCh_         = 0;
    size_t      prefillFrames_    = 0;
    size_t      maxChunkFrames_   = 0;
    void*       capDataReadyEvent_ = nullptr;

    // Lock-free status (written by the pump, read by poll() on the GUI thread).
    std::atomic<StreamState> overall_{StreamState::Idle};
    std::atomic<StreamState> capState_{StreamState::Idle};
    std::atomic<StreamState> renderState_{StreamState::Idle};
    std::atomic<StreamState> silentRenderState_{StreamState::Idle};
    std::atomic<uint32_t>    sampleRate_{0};
    std::atomic<uint32_t>    captureChannels_{0};
    std::atomic<uint32_t>    delayMsAtomic_{0};
    std::atomic<uint32_t>    renderBufMs_{0};
    std::atomic<float>       fifoFillMs_{0.f};
    std::atomic<uint64_t>    driftFixes_{0};
    std::atomic<uint64_t>    capXruns_{0};
    std::atomic<uint64_t>    renderXruns_{0};
    std::atomic<uint64_t>    renderDropped_{0};
    std::atomic<float>       capLevel_{0.f};
    std::atomic<float>       renderLevel_{0.f};
    std::atomic<uint32_t>    errorCode_{0};

    // Preallocated pump scratch (no per-iteration heap allocation in the loop).
    std::vector<uint8_t> capScratch_;
    std::vector<float>   capFloat_;
    std::vector<float>   capMono_;
    std::vector<float>   popBuf_;
    std::vector<float>   renderAdapt_;
    std::vector<float>   renderMono_;
    std::vector<uint8_t> renderBytes_;
};

} // namespace wa
