#include "AudioFormatStr.h"
#include "ComUtil.h"
#include "Log.h"
#include "MonitorEngine.h"
#include "RingBuffer.h"
#include "WasapiStream.h"
#include "WavFile.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

bool require(bool condition, const char* operation) {
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", operation);
    return condition;
}

template <typename Predicate>
bool waitFor(Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (Clock::now() < deadline);
    return predicate();
}

struct Producer {
    std::atomic<bool> running{true};
    std::thread thread;
    Producer(wa::RingBuffer& ring, const wa::AudioFormat& format) {
        thread = std::thread([this, &ring, format] {
            std::vector<float> block(static_cast<size_t>(format.sampleRate / 100) * format.channels);
            for (size_t i = 0; i < block.size(); ++i)
                block[i] = (i % 4 < 2) ? .001f : -.001f;
            auto next = Clock::now();
            while (running.load()) {
                ring.write(block.data(), block.size() * sizeof(float));
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
            }
        });
    }
    ~Producer() { running.store(false); if (thread.joinable()) thread.join(); }
};

bool finished(const wa::WavSinkStatus& status) {
    return status.phase == wa::WavDumpPhase::Success || status.phase == wa::WavDumpPhase::Failure ||
           status.phase == wa::WavDumpPhase::Cancelled;
}

bool verify(const char* stage, const wa::WavSinkStatus& status, const wa::AudioFormat& format) {
    if (!require(status.phase == wa::WavDumpPhase::Success, "completed file has Success phase")) {
        std::fprintf(stderr, "file result: %s\n", status.message.c_str());
        return false;
    }
    wa::WavReader reader;
    if (!require(static_cast<bool>(reader.open(status.path)), "open completed WAV")) return false;
    if (!require(reader.format() == format, "WAV negotiated format")) return false;
    if (!require(reader.format().channelMask == wa::toWaveFormatExtensible(format).dwChannelMask,
                 "WAV channel mask")) return false;
    std::vector<uint8_t> block(65536);
    size_t bytes = 0, nonzero = 0;
    for (;;) {
        const size_t got = reader.read(block.data(), block.size());
        if (!got) break;
        bytes += got;
        nonzero += static_cast<size_t>(std::count_if(block.begin(), block.begin() + got,
            [](uint8_t value) { return value != 0; }));
    }
    if (!require(bytes > 0 && nonzero > 0 && bytes % format.blockAlign() == 0 && reader.eof(),
                 "nonempty complete PCM frames and EOF")) return false;
    std::printf("stage=%s generation=%llu source=%llu rate=%u channels=%u bits=%u float=%d "
                "bytes=%zu nonzero_bytes=%zu file=%s\n", stage,
        static_cast<unsigned long long>(status.generation), static_cast<unsigned long long>(status.source),
        format.sampleRate, format.channels, format.bitsPerSample, format.isFloat ? 1 : 0,
        bytes, nonzero, wa::narrowAscii(status.path).c_str());
    return require(static_cast<bool>(reader.close()), "close WAV reader");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: WavMonitorDeviceSmoke <source-render-endpoint> <other-render-endpoint> <existing-folder>\n");
        return 2;
    }
    if (!require(std::wstring(argv[1]) != argv[2], "distinct loopback and playback endpoints")) return 3;
    wa::log::init();
    struct Shutdown { ~Shutdown() { wa::log::shutdown(); } } logging;
    wa::log::addFileSink(wa::narrowAscii(std::wstring(argv[3]) + L"\\runtime.txt"));
    wa::ComInitGuard com;
    if (!require(com.ok(), "COM initialize")) return 4;
    wa::RingBuffer ring(1u << 20);
    wa::WasapiRenderStream source(wa::WasapiMode::Shared, nullptr);
    if (!require(static_cast<bool>(source.open(argv[1], {}, &ring, {})), "open source render") ||
        !require(static_cast<bool>(source.start()), "start source render")) return 5;
    const auto format = source.stats().actualFormat;
    if (!require(format.isFloat && format.bitsPerSample == 32, "Float32 source mix")) return 6;
    Producer producer(ring, format);
    wa::MonitorEngine monitor;
    wa::CaptureSource capture;
    capture.kind = wa::CaptureSourceKind::SystemLoopback;
    capture.deviceId = argv[1];
    wa::LoopbackOptions options;
    options.silentRender = false;
    if (!require(static_cast<bool>(monitor.start(wa::BackendKind::WasapiShared, capture, argv[2],
                                               50, true, {}, {}, nullptr, options)), "start Monitor")) return 7;
    if (!require(waitFor([&] { return monitor.renderWritten() >= format.sampleRate / 5; }),
                 "real capture and delayed render PCM")) return 8;
    if (!require(static_cast<bool>(monitor.startDumpCapture(argv[3])), "start Capture Dump") ||
        !require(static_cast<bool>(monitor.startDumpRender(argv[3])), "start Render Dump")) return 9;
    const auto first = monitor.poll();
    const auto beforeFirst = monitor.renderWritten();
    if (!require(waitFor([&] { return monitor.renderWritten() >= beforeFirst + format.sampleRate / 5; }),
                 "both Dumps receive real PCM")) return 10;
    const auto beforeOff = Clock::now();
    monitor.setPlaybackEnabled(false);
    const auto requestNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - beforeOff).count();
    if (!require(waitFor([&] { return monitor.poll().renderState == wa::StreamState::Idle; }),
                 "playback becomes Idle")) return 11;
    const auto offUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - beforeOff).count();
    const auto captureBefore = monitor.capWritten();
    if (!require(waitFor([&] { return monitor.capWritten() >= captureBefore + format.sampleRate / 5; }),
                 "capture Scope continues with playback off") ||
        !require(monitor.poll().capDumping, "Capture Dump remains accepting") ||
        !require(waitFor([&] { return finished(monitor.poll().renderDump); }), "independent Render Dump completion")) return 12;
    auto completed = monitor.poll();
    if (!require(completed.capDump.generation == first.capDump.generation &&
                 completed.capDump.source == first.capDump.source, "Capture Dump identity survives playback off") ||
        !verify("playback-off-render", completed.renderDump, format)) return 13;
    std::printf("playback_off_request_ns=%lld render_idle_us=%lld capture_frames_during_off=%llu\n",
        static_cast<long long>(requestNs), static_cast<long long>(offUs),
        static_cast<unsigned long long>(monitor.capWritten() - captureBefore));

    monitor.setPlaybackEnabled(true);
    if (!require(waitFor([&] { return monitor.poll().renderState == wa::StreamState::Running; }),
                 "render re-engages") ||
        !require(!monitor.poll().renderDumping, "re-engage does not reopen a Dump") ||
        !require(static_cast<bool>(monitor.startDumpRender(argv[3])), "explicit Render Dump restart")) return 14;
    const auto restarted = monitor.poll();
    if (!require(restarted.renderDump.generation != first.renderDump.generation &&
                 restarted.renderDump.source != first.renderDump.source &&
                 restarted.renderDump.path != first.renderDump.path, "new render source and file identities")) return 15;
    const auto beforeRestart = monitor.renderWritten();
    if (!require(waitFor([&] { return monitor.renderWritten() >= beforeRestart + format.sampleRate / 5; }),
                 "restarted Render Dump PCM") ||
        !require(static_cast<bool>(monitor.stopDumpRender()), "explicit Render Dump stop") ||
        !verify("explicit-stop-render", monitor.poll().renderDump, format) ||
        !require(static_cast<bool>(monitor.stopDumpCapture()), "explicit Capture Dump stop") ||
        !verify("explicit-stop-capture", monitor.poll().capDump, format)) return 16;
    if (!require(static_cast<bool>(monitor.startDumpCapture(argv[3])), "Capture Dump restart") ||
        !require(static_cast<bool>(monitor.startDumpRender(argv[3])), "Render Dump restart before Monitor stop")) return 17;
    const auto beforeStop = monitor.renderWritten();
    if (!require(waitFor([&] { return monitor.renderWritten() >= beforeStop + format.sampleRate / 5; }),
                 "both Dumps active before Monitor stop")) return 18;
    monitor.stop();
    completed = monitor.poll();
    if (!verify("monitor-stop-capture", completed.capDump, format) ||
        !verify("monitor-stop-render", completed.renderDump, format)) return 19;
    if (!require(static_cast<bool>(monitor.start(wa::BackendKind::WasapiShared, capture, argv[2],
                                               50, true, {}, {}, nullptr, options)), "restart Monitor session") ||
        !require(static_cast<bool>(monitor.startDumpCapture(argv[3])), "next session Capture Dump") ||
        !require(static_cast<bool>(monitor.startDumpRender(argv[3])), "next session Render Dump")) return 20;
    const auto session = monitor.poll();
    if (!require(session.capDump.source != first.capDump.source &&
                 session.renderDump.source != restarted.renderDump.source, "next session sources advance")) return 21;
    if (!require(waitFor([&] { return monitor.renderWritten() >= format.sampleRate / 5; }),
                 "next session real PCM")) return 22;
    monitor.stop();
    completed = monitor.poll();
    if (!verify("next-session-capture", completed.capDump, format) ||
        !verify("next-session-render", completed.renderDump, format)) return 23;
    std::printf("Monitor device smoke passed: 7 completed WAV files, playback toggle, Dump restart, session restart, clean stop.\n");
    return 0;
}
