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

namespace wa {

namespace {
constexpr size_t kFlushBytes = 256u * 1024u;
constexpr size_t kRingMin    = 1u << 20;
constexpr size_t kRingMax    = 16u << 20;

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

WavSink::WavSink() = default;
WavSink::~WavSink() { stop(); }

Result WavSink::start(const std::wstring& folder, const std::wstring& prefix,
                      const AudioFormat& fmt) {
    if (folder.empty() || prefix.empty())
        return Result::Fail(-1, "WavSink: folder and prefix required");
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return begin(uniqueWavPath(folder, autoStem(prefix, fmt, st)), fmt);
}

Result WavSink::start(const std::wstring& folder, const char* prefix, const AudioFormat& fmt) {
    return start(folder, wideAscii(prefix), fmt);
}

Result WavSink::startExact(const std::wstring& path, const AudioFormat& fmt) {
    return begin(path, fmt);
}

Result WavSink::begin(const std::wstring& path, const AudioFormat& fmt) {
    if (state_.load(std::memory_order_acquire) == WavSinkState::Running)
        return Result::Fail(-1, "WavSink: already running");
    if (path.empty())
        return Result::Fail(-1, "WavSink: empty path");

    stop();

    fmt_ = fmt;
    overflow_.store(false, std::memory_order_relaxed);
    writeError_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        path_ = path;
        fileName_ = fileNameOf(path);
        message_.clear();
    }
    ring_ = std::make_unique<RingBuffer>(ringBytesFor(fmt));
    running_.store(true, std::memory_order_release);
    state_.store(WavSinkState::Idle, std::memory_order_release);
    writer_ = std::thread(&WavSink::writerLoop, this);

    for (int i = 0; i < 2000; ++i) {
        const WavSinkState st = state_.load(std::memory_order_acquire);
        if (st == WavSinkState::Running) {
            WA_LOG(wa::log::Level::Info, "WavSink", "start",
                   wa::narrowAscii(path) + " fmt=" + wa::formatAudio(fmt), "ok");
            return Result::Ok();
        }
        if (st == WavSinkState::Error) {
            joinWriter();
            std::lock_guard<std::mutex> lk(mtx_);
            return Result::Fail(-1, message_.empty() ? "WavSink: open failed" : message_);
        }
        Sleep(1);
    }
    running_.store(false, std::memory_order_release);
    joinWriter();
    state_.store(WavSinkState::Idle, std::memory_order_release);
    return Result::Fail(-1, "WavSink: timed out waiting for writer");
}

size_t WavSink::push(const void* data, size_t bytes) {
    if (!data || bytes == 0) return 0;
    if (state_.load(std::memory_order_acquire) != WavSinkState::Running) return 0;
    if (!running_.load(std::memory_order_acquire) || !ring_) return 0;
    const size_t n = ring_->write(data, bytes);
    if (n < bytes) {
        overflow_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_release);
    }
    return n;
}

Result WavSink::stop() {
    running_.store(false, std::memory_order_release);
    joinWriter();
    ring_.reset();
    const bool failed = overflow_.load(std::memory_order_relaxed)
                        || writeError_.load(std::memory_order_relaxed)
                        || state_.load(std::memory_order_relaxed) == WavSinkState::Error;
    if (failed) {
        state_.store(WavSinkState::Error, std::memory_order_release);
        WA_LOG(wa::log::Level::Info, "WavSink", "stop",
               wa::narrowAscii(path_), "error");
        return Result::Fail(-1, message_.empty() ? "WavSink: failed" : message_);
    }
    state_.store(WavSinkState::Idle, std::memory_order_release);
    if (!path_.empty())
        WA_LOG(wa::log::Level::Info, "WavSink", "stop", wa::narrowAscii(path_), "ok");
    return Result::Ok();
}

bool WavSink::isRunning() const {
    return state_.load(std::memory_order_acquire) == WavSinkState::Running;
}

WavSinkStatus WavSink::poll() const {
    WavSinkStatus s;
    s.state = state_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lk(mtx_);
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
    WavWriter writer;
    Result wr = writer.open(path_, fmt_);
    if (!wr) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            message_ = wr.message.empty() ? "WavSink: cannot open file" : wr.message;
        }
        WA_LOG(wa::log::Level::Err, "WavSink", "open", wa::narrowAscii(path_), message_);
        running_.store(false, std::memory_order_release);
        state_.store(WavSinkState::Error, std::memory_order_release);
        return;
    }
    writer.setStdioBuffer(kFlushBytes);
    state_.store(WavSinkState::Running, std::memory_order_release);

    std::vector<uint8_t> chunk(kFlushBytes);
    bool failed = false;
    for (;;) {
        const bool stop = !running_.load(std::memory_order_acquire);
        const bool overflow = overflow_.load(std::memory_order_acquire);
        const size_t avail = ring_ ? ring_->availableRead() : 0;
        const bool drain = stop || overflow;
        if (!drain && avail < kFlushBytes) {
            Sleep(5);
            continue;
        }
        if (avail == 0) {
            if (drain) break;
            Sleep(5);
            continue;
        }
        const size_t want = (std::min)(avail, kFlushBytes);
        const size_t got = ring_->read(chunk.data(), want);
        if (got == 0) {
            if (drain) break;
            continue;
        }
        if (writer.write(chunk.data(), got) != got) {
            WA_LOG(wa::log::Level::Err, "WavSink", "write", wa::narrowAscii(path_),
                   "short write");
            writeError_.store(true, std::memory_order_release);
            failed = true;
            break;
        }
        if (overflow && (!ring_ || ring_->availableRead() == 0)) break;
    }
    writer.close();
    if (failed || overflow_.load(std::memory_order_relaxed)) {
        const bool ovf = overflow_.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (message_.empty())
                message_ = ovf ? "WavSink: ring overflow" : "WavSink: write failed";
        }
        if (ovf)
            WA_LOG(wa::log::Level::Err, "WavSink", "write", wa::narrowAscii(path_), "overflow");
        state_.store(WavSinkState::Error, std::memory_order_release);
    }
}

} // namespace wa
