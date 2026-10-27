#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "WavSink.h"
#include "AudioFormatStr.h"
#include "Log.h"
#include "RingBuffer.h"
#include "WavFile.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <limits>
#include <stdexcept>
#ifdef WA_WAV_SINK_TESTING
#include "WavSinkTestHooks.h"
#endif

namespace wa {

namespace {
class FileOutput : public WavSinkOutput {
public:
    Result open(const std::wstring& path, const AudioFormat& fmt) override {
        return writer_.open(path, fmt);
    }
    Result setBuffer(size_t bytes) override {
        return writer_.setStdioBuffer(bytes);
    }
    Result write(const void* data, size_t bytes) override { return writer_.write(data, bytes); }
    Result close() override { return writer_.close(); }
private:
    WavWriter writer_;
};
constexpr size_t kFlushBytes = 256u * 1024u;
constexpr size_t kRingMin    = 1u << 20;
constexpr size_t kRingMax    = 16u << 20;
constexpr uint64_t kReady = 1;
constexpr uint64_t kClosed = 2;
constexpr uint64_t kActive = 4;
constexpr uint64_t kGenerationStep = 8;
static_assert(std::atomic<uint64_t>::is_always_lock_free, "WAV admission must be lock-free");
static_assert(alignof(WavSink) >= 8, "WAV admission must be eight-byte aligned");
static_assert(alignof(std::atomic<uint64_t>) <= 8, "WAV gate alignment is insufficient");

size_t ringBytesFor(const AudioFormat& fmt) {
    const uint32_t bps = fmt.avgBytesPerSec();
    size_t n = static_cast<size_t>(bps) * 4u;
    if (n < kRingMin) n = kRingMin;
    if (n > kRingMax) n = kRingMax;
    return n;
}

std::wstring fileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return path;
    return path.substr(slash + 1);
}

std::wstring wideAscii(const char* s) {
    std::wstring w;
    if (!s) return w;
    for (; *s; ++s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
    return w;
}

std::wstring joinPath(const std::wstring& folder, const std::wstring& name) {
    if (folder.empty()) return name;
    const wchar_t last = folder.back();
    if (last == L'\\' || last == L'/') return folder + name;
    return folder + L'\\' + name;
}

std::wstring typeToken(const AudioFormat& fmt) {
    std::wstring t = std::to_wstring(fmt.bitsPerSample);
    if (fmt.isFloat) t += L'f';
    return t;
}

std::wstring autoStem(const std::wstring& prefix, const AudioFormat& fmt, const SYSTEMTIME& st) {
    return prefix + L'_' + std::to_wstring(fmt.sampleRate) + L'_' +
           std::to_wstring(fmt.channels) + L"ch_" + typeToken(fmt) + L'_' +
           std::to_wstring(st.wYear) +
           (st.wMonth < 10 ? L"0" : L"") + std::to_wstring(st.wMonth) +
           (st.wDay < 10 ? L"0" : L"") + std::to_wstring(st.wDay) + L'_' +
           (st.wHour < 10 ? L"0" : L"") + std::to_wstring(st.wHour) +
           (st.wMinute < 10 ? L"0" : L"") + std::to_wstring(st.wMinute) +
           (st.wSecond < 10 ? L"0" : L"") + std::to_wstring(st.wSecond);
}

std::wstring uniqueWavPath(const std::wstring& folder, const std::wstring& stem) {
    std::wstring path = joinPath(folder, stem + L".wav");
    int n = 2;
    while (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        path = joinPath(folder, stem + L'_' + std::to_wstring(n) + L".wav");
        if (++n > 10000) break;
    }
    return path;
}
} // namespace

WavSink::WavSink(WavSinkOutputFactory outputFactory)
    : outputFactory_(std::move(outputFactory)) {}
WavSink::~WavSink() { stop(); }

Result WavSink::start(const std::wstring& folder, const std::wstring& prefix,
                      const AudioFormat& fmt, uint64_t source,
                      const std::atomic<uint64_t>* sourceVersion) {
    std::lock_guard<std::mutex> control(controlMtx_);
    if (folder.empty() || prefix.empty())
        return Result::Fail(-1, "WavSink: folder and prefix required");
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return begin(uniqueWavPath(folder, autoStem(prefix, fmt, st)), fmt, source, sourceVersion);
}

Result WavSink::start(const std::wstring& folder, const char* prefix,
                      const AudioFormat& fmt, uint64_t source,
                      const std::atomic<uint64_t>* sourceVersion) {
    return start(folder, wideAscii(prefix), fmt, source, sourceVersion);
}

Result WavSink::startExact(const std::wstring& path, const AudioFormat& fmt, uint64_t source,
                          const std::atomic<uint64_t>* sourceVersion) {
    std::lock_guard<std::mutex> control(controlMtx_);
    return begin(path, fmt, source, sourceVersion);
}

Result WavSink::begin(const std::wstring& path, const AudioFormat& fmt, uint64_t source,
                      const std::atomic<uint64_t>* sourceVersion) {
    if ((gate_.load(std::memory_order_seq_cst) & (kReady | kClosed)) == kReady)
        return Result::Fail(-1, "WavSink: already running");
    if (path.empty()) return Result::Fail(-1, "WavSink: empty path");
    if (source == 0) return Result::Fail(-1, "WavSink: source required");
    if (generation_ == (std::numeric_limits<uint64_t>::max)() / kGenerationStep)
        return Result::Fail(-1, "WavSink: generation exhausted");

    stopLocked();
#ifdef WA_WAV_SINK_TESTING
    wavSinkTest::checkpoint(wavSinkTest::Point::BeforePreparing, this);
#endif
    try {
        fmt_ = fmt;
        sourceVersion_ = sourceVersion;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            path_ = path;
            fileName_ = fileNameOf(path);
            message_.clear();
            prepared_ = false;
            preparedReady_ = false;
            failure_.store(Failure::None, std::memory_order_seq_cst);
            // Poll sees path, source and generation as one round. Source end
            // can race this reservation; the writer checks the live version.
            source_.store(source, std::memory_order_seq_cst);
            gate_.store(++generation_ * kGenerationStep, std::memory_order_seq_cst);
        }
#ifdef WA_WAV_SINK_TESTING
        wavSinkTest::prepare(wavSinkTest::Preparation::Ring);
#endif
        ring_ = std::make_unique<RingBuffer>(ringBytesFor(fmt));
#ifdef WA_WAV_SINK_TESTING
        wavSinkTest::prepare(wavSinkTest::Preparation::Thread);
#endif
        writer_ = std::thread(&WavSink::writerLoop, this);
    } catch (const std::exception& e) {
        gate_.fetch_or(kClosed, std::memory_order_seq_cst);
        ring_.reset();
        failure_.store(Failure::Output, std::memory_order_seq_cst);
        std::lock_guard<std::mutex> lk(mtx_);
        message_ = e.what();
        completedGeneration_ = generation_;
        completedPhase_ = WavDumpPhase::Failure;
        prepared_ = true;
        readyCv_.notify_all();
        return Result::Fail(-1, message_);
    }
    bool ready = false;
    {
        std::unique_lock<std::mutex> lk(mtx_);
        readyCv_.wait(lk, [this] { return prepared_; });
        ready = preparedReady_;
    }
    if (ready) {
        WA_LOG(wa::log::Level::Info, "WavSink", "start",
               wa::narrowAscii(path) + " fmt=" + wa::formatAudio(fmt), "ok");
        return Result::Ok();
    }
    const Result result = stopLocked();
    return result ? Result::Fail(-1, "WavSink: start cancelled") : result;
}

size_t WavSink::push(const void* data, size_t bytes, uint64_t source) {
    if (!data || bytes == 0) return 0;
    uint64_t observed = gate_.load(std::memory_order_seq_cst);
#ifdef WA_WAV_SINK_TESTING
    wavSinkTest::checkpoint(wavSinkTest::Point::AfterSnapshot, this);
#endif
    if ((observed & (kReady | kClosed | kActive)) != kReady) return 0;
    if (!gate_.compare_exchange_strong(observed, observed | kActive,
                                      std::memory_order_seq_cst)) return 0;
#ifdef WA_WAV_SINK_TESTING
    wavSinkTest::checkpoint(wavSinkTest::Point::AfterAdmission, this);
#endif
    if (source_.load(std::memory_order_seq_cst) != source) {
        gate_.fetch_and(~kActive, std::memory_order_seq_cst);
        return 0;
    }
    const size_t n = ring_->write(data, bytes);
    if (n < bytes) {
        Failure expected = Failure::None;
        failure_.compare_exchange_strong(expected, Failure::Overflow, std::memory_order_seq_cst);
        gate_.fetch_or(kClosed, std::memory_order_seq_cst);
    }
    gate_.fetch_and(~kActive, std::memory_order_seq_cst);
    return n;
}

void WavSink::requestStop(uint64_t source) {
    uint64_t observed = gate_.load(std::memory_order_seq_cst);
    const uint64_t generation = observed / kGenerationStep;
    for (;;) {
        if ((observed & kClosed) != 0 || source_.load(std::memory_order_seq_cst) != source)
            return;
#ifdef WA_WAV_SINK_TESTING
        wavSinkTest::checkpoint(wavSinkTest::Point::BeforeStopAdmission, this);
#endif
        if (gate_.compare_exchange_strong(observed, observed | kClosed,
                                         std::memory_order_seq_cst)) return;
        if (observed / kGenerationStep != generation) return;
    }
}

bool WavSink::hasFailed() const {
    return failure_.load(std::memory_order_seq_cst) != Failure::None;
}

Result WavSink::stop() {
    std::lock_guard<std::mutex> control(controlMtx_);
    return stopLocked();
}

Result WavSink::stopLocked() {
    gate_.fetch_or(kClosed, std::memory_order_seq_cst);
    joinWriter();
    ring_.reset();
    const bool failed = hasFailed();
    if (failed) {
        WA_LOG(wa::log::Level::Info, "WavSink", "stop",
               wa::narrowAscii(path_), "error");
        return Result::Fail(-1, message_.empty() ? "WavSink: failed" : message_);
    }
    if (!path_.empty())
        WA_LOG(wa::log::Level::Info, "WavSink", "stop", wa::narrowAscii(path_), "ok");
    return Result::Ok();
}

bool WavSink::isRunning() const {
    return (gate_.load(std::memory_order_seq_cst) & (kReady | kClosed)) == kReady;
}

WavSinkStatus WavSink::poll() const {
    WavSinkStatus s;
    std::lock_guard<std::mutex> lk(mtx_);
    const uint64_t gate = gate_.load(std::memory_order_seq_cst);
    s.generation = gate / kGenerationStep;
    s.source = source_.load(std::memory_order_seq_cst);
    if (s.generation == completedGeneration_) s.phase = completedPhase_;
    else if ((gate & kClosed) != 0) s.phase = WavDumpPhase::Finalizing;
    else if ((gate & kReady) != 0) s.phase = WavDumpPhase::Accepting;
    else s.phase = WavDumpPhase::Preparing;
    s.state = s.phase == WavDumpPhase::Accepting ? WavSinkState::Running
            : s.phase == WavDumpPhase::Failure ? WavSinkState::Error : WavSinkState::Idle;
    s.path = path_;
    s.fileName = fileName_;
    s.message = message_;
    return s;
}

void WavSink::joinWriter() {
    if (writer_.joinable()) writer_.join();
}

void WavSink::writerLoop() {
    wa::log::setThreadName("wav");
    std::unique_ptr<WavSinkOutput> writer;
    bool accepted = false;
    auto notifyPrepared = [this](bool ready = false) {
        std::lock_guard<std::mutex> lk(mtx_);
        preparedReady_ = preparedReady_ || ready;
        prepared_ = true;
        readyCv_.notify_all();
    };
    auto recordFailure = [this](const Result& r) {
        Failure expected = Failure::None;
        const bool first = failure_.compare_exchange_strong(expected, Failure::Output,
                                                            std::memory_order_seq_cst);
        gate_.fetch_or(kClosed, std::memory_order_seq_cst);
        if (first) {
            std::lock_guard<std::mutex> lk(mtx_);
            message_ = r.message;
        }
        WA_LOG(wa::log::Level::Err, "WavSink", "output", wa::narrowAscii(path_), r.message);
    };
    try {
        writer = outputFactory_ ? outputFactory_() : std::make_unique<FileOutput>();
        if (!writer) throw std::runtime_error("WavSink: output factory returned null");
        Result wr = writer->open(path_, fmt_);
        if (wr) wr = writer->setBuffer(kFlushBytes);
        if (!wr) {
            recordFailure(wr);
        } else {
            // Allocate scratch before publishing Ready or waking the starter.
#ifdef WA_WAV_SINK_TESTING
            wavSinkTest::prepare(wavSinkTest::Preparation::Scratch);
#endif
            std::vector<uint8_t> chunk(kFlushBytes);
#ifdef WA_WAV_SINK_TESTING
            wavSinkTest::checkpoint(wavSinkTest::Point::BeforeReady, this);
#endif
            // An end request can precede reservation of Preparing. In that case
            // it saw the old Closed gate; the source version still rejects it.
            if (sourceVersion_ && sourceVersion_->load(std::memory_order_seq_cst) !=
                                      source_.load(std::memory_order_seq_cst))
                requestStop(source_.load(std::memory_order_seq_cst));
            uint64_t preparing = generation_ * kGenerationStep;
            if (gate_.compare_exchange_strong(preparing, preparing | kReady,
                                              std::memory_order_seq_cst)) {
#ifdef WA_WAV_SINK_TESTING
                wavSinkTest::checkpoint(wavSinkTest::Point::AfterReady, this);
#endif
                const bool current = !sourceVersion_ ||
                    sourceVersion_->load(std::memory_order_seq_cst) ==
                        source_.load(std::memory_order_seq_cst);
                if (!current) requestStop(source_.load(std::memory_order_seq_cst));
                else {
                    accepted = true;
                    notifyPrepared(true);
                }
                for (;;) {
                    const uint64_t gate = gate_.load(std::memory_order_seq_cst);
                    const bool drain = (gate & kClosed) != 0;
#ifdef WA_WAV_SINK_TESTING
                    if (drain) wavSinkTest::drainObserved(this, (gate & kActive) != 0);
#endif
                    if (drain && (gate & kActive) != 0) {
                        Sleep(5);
                        continue;
                    }
                    // Re-read after Closed/no-Active: the last producer has published.
                    const size_t avail = ring_->availableRead();
                    if ((!drain && avail < kFlushBytes) || (avail == 0 && !drain)) {
                        Sleep(5);
                        continue;
                    }
                    if (avail == 0) break;
                    const size_t got = ring_->read(chunk.data(), (std::min)(avail, kFlushBytes));
                    const Result wrChunk = writer->write(chunk.data(), got);
                    if (!wrChunk) {
                        recordFailure(wrChunk);
                        break;
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        recordFailure(Result::Fail(-1, e.what()));
    } catch (...) {
        recordFailure(Result::Fail(-1, "WavSink: output preparation or write failed"));
    }
    gate_.fetch_or(kClosed, std::memory_order_seq_cst);
    // I/O failure may discard queued bytes, but never ends an in-flight access.
    while ((gate_.load(std::memory_order_seq_cst) & kActive) != 0) Sleep(5);
    if (writer) {
        try {
            const Result closed = writer->close();
            if (!closed) recordFailure(closed);
        } catch (const std::exception& e) {
            recordFailure(Result::Fail(-1, e.what()));
        } catch (...) {
            recordFailure(Result::Fail(-1, "WavSink: output close failed"));
        }
    }
    const bool failed = hasFailed();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (failed && message_.empty()) message_ = "WavSink: ring overflow";
        completedGeneration_ = generation_;
        completedPhase_ = failed ? WavDumpPhase::Failure
                                : accepted ? WavDumpPhase::Success : WavDumpPhase::Cancelled;
    }
    notifyPrepared();
}

} // namespace wa
