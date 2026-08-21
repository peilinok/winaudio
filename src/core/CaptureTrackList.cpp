#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CaptureTrackList.h"
#include "ApplicationLoopbackCapture.h"
#include "AudioFormatStr.h"
#include "Log.h"
#include "RingBuffer.h"
#include "SampleConvert.h"
#include "ScopeBuffer.h"
#include "WasapiStream.h"
#include "WavSink.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <thread>

namespace wa {

namespace {
constexpr size_t kRingBytes = 1u << 20;

void computeLevels(const uint8_t* data, size_t bytes, const AudioFormat& fmt,
                   float& l, float& r) {
    l = r = 0.f;
    if (fmt.channels == 0) return;
    const uint16_t ch = fmt.channels;
    if (fmt.isFloat && fmt.bitsPerSample == 32) {
        const float* s = reinterpret_cast<const float*>(data);
        size_t n = bytes / 4;
        for (size_t i = 0; i + ch <= n; i += ch) {
            l = std::max(l, std::fabs(s[i]));
            r = std::max(r, std::fabs(s[i + (ch > 1 ? 1 : 0)]));
        }
    } else if (!fmt.isFloat && fmt.bitsPerSample == 16) {
        const int16_t* s = reinterpret_cast<const int16_t*>(data);
        size_t n = bytes / 2;
        for (size_t i = 0; i + ch <= n; i += ch) {
            l = std::max(l, std::fabs(s[i] / 32768.f));
            r = std::max(r, std::fabs(s[i + (ch > 1 ? 1 : 0)] / 32768.f));
        }
    }
}

bool isLoopbackKind(CaptureSourceKind kind) {
    return kind == CaptureSourceKind::SystemLoopback
        || kind == CaptureSourceKind::ApplicationLoopback;
}

const char* sourceName(CaptureSourceKind kind) {
    switch (kind) {
    case CaptureSourceKind::SystemLoopback:      return "system-loopback";
    case CaptureSourceKind::ApplicationLoopback: return "application-loopback";
    default:                                     return "endpoint";
    }
}

const wchar_t* dumpPrefixKind(CaptureSourceKind kind) {
    switch (kind) {
    case CaptureSourceKind::SystemLoopback:      return L"loopback";
    case CaptureSourceKind::ApplicationLoopback: return L"app-loopback";
    default:                                     return L"capture";
    }
}

std::wstring sanitizeDumpToken(std::wstring s) {
    for (wchar_t& c : s) {
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*')
            c = L'_';
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.'))
        s.pop_back();
    return s.empty() ? L"unknown" : s;
}

std::wstring processDumpName(uint32_t pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"unknown";
    wchar_t path[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &n) && n > 0) {
        const std::wstring p(path, n);
        const size_t slash = p.find_last_of(L"\\/");
        name = (slash == std::wstring::npos) ? p : p.substr(slash + 1);
    }
    CloseHandle(h);
    return sanitizeDumpToken(std::move(name));
}

std::wstring dumpPrefixFor(const CaptureSource& src) {
    std::wstring p = dumpPrefixKind(src.kind);
    if (src.kind == CaptureSourceKind::ApplicationLoopback)
        p += L'_' + processDumpName(src.processId) + L'_' + std::to_wstring(src.processId);
    return p;
}
} // namespace

struct CaptureTrackList::Member {
    TrackId     id = 0;
    CaptureSource source{};
    std::wstring wavPath;
    std::unique_ptr<IAudioBackend> backend;
    std::unique_ptr<IAudioBackend> silent;
    std::unique_ptr<RingBuffer>    ring;
    std::unique_ptr<ScopeBuffer>   tap;
    WavSink sink;
    bool wavFatal = false;
    std::thread pump;
    std::atomic<bool> running{false};
    std::mutex mtx;
    CaptureTrackStatus status{};

    void stopPumpAndBackends() {
        running.store(false, std::memory_order_relaxed);
        if (pump.joinable()) pump.join();
        else sink.stop();
        if (silent) {
            silent->stop();
            silent->close();
            silent.reset();
        }
        if (backend) {
            backend->stop();
            backend->close();
            backend.reset();
        }
        ring.reset();
        tap.reset();
    }
};

CaptureTrackList::CaptureTrackList(BackendFactory factory, SilentRenderFactory silentFactory)
    : factory_(std::move(factory)), silentFactory_(std::move(silentFactory)) {}

CaptureTrackList::~CaptureTrackList() { destroyAll(); }

static std::unique_ptr<IAudioBackend> makeDefaultCaptureBackend(BackendKind kind,
                                                         const CaptureSource& source,
                                                         const AudioFormat* requested) {
    const WasapiMode mode = (kind == BackendKind::WasapiExclusive) ? WasapiMode::Exclusive
                                                                   : WasapiMode::Shared;
    if (source.kind == CaptureSourceKind::SystemLoopback)
        return std::make_unique<WasapiSystemLoopbackCaptureStream>(mode, requested);
    if (source.kind == CaptureSourceKind::ApplicationLoopback)
        return std::make_unique<ApplicationLoopbackCaptureStream>(
            mode, source.processId, requested, source.processLoopbackMode);
    return std::make_unique<WasapiCaptureStream>(mode, requested);
}

Result CaptureTrackList::create(const CaptureTrackCreate& spec, TrackId* outId) {
    if (isLoopbackKind(spec.source.kind) && spec.kind == BackendKind::WasapiExclusive) {
        WA_LOG(wa::log::Level::Err, "CaptureTrackList", "create",
               "source=" + std::string(sourceName(spec.source.kind)),
               "exclusive rejected");
        return Result::Fail(-1, "WASAPI loopback requires Shared mode");
    }

    auto m = std::make_unique<Member>();
    m->source = spec.source;
    m->wavPath = spec.wavPath;
    m->status.source = spec.source;
    m->status.state = StreamState::Idle;

    WA_LOG(wa::log::Level::Info, "CaptureTrackList", "create",
           std::string("source=") + sourceName(spec.source.kind)
               + " id=" + (spec.source.deviceId.empty()
                               ? std::string("(default)")
                               : wa::narrowAscii(spec.source.deviceId))
               + (spec.source.kind == CaptureSourceKind::ApplicationLoopback
                      ? " pid=" + std::to_string(spec.source.processId)
                      : std::string())
               + (spec.requested ? " fmt=" + wa::formatAudio(*spec.requested)
                                 : std::string()),
           "");

    try {
        m->ring = std::make_unique<RingBuffer>(kRingBytes);
        if (factory_)
            m->backend = factory_(spec.source, spec.requested);
        else
            m->backend = makeDefaultCaptureBackend(spec.kind, spec.source, spec.requested);
        if (!m->backend) {
            WA_LOG(wa::log::Level::Err, "CaptureTrackList", "create", "", "factory null");
            return Result::Fail(-1, "CaptureTrackList: capture factory returned null");
        }

        Result r = m->backend->open(spec.source.deviceId, AudioFormat{}, m->ring.get(),
                                    spec.streamParams);
        if (!r) {
            WA_LOG(wa::log::Level::Err, "CaptureTrackList", "create", "open", r.message);
            return r;
        }
        r = m->backend->start();
        if (!r) {
            WA_LOG(wa::log::Level::Err, "CaptureTrackList", "create", "start", r.message);
            m->backend->close();
            return r;
        }

        StreamState silentSt = StreamState::Idle;
        if (spec.source.kind == CaptureSourceKind::SystemLoopback
            && spec.loopbackOptions.silentRender) {
            if (silentFactory_)
                m->silent = silentFactory_(nullptr);
            else if (!factory_)
                m->silent = std::make_unique<WasapiSilentRenderStream>(WasapiMode::Shared,
                                                                       nullptr);
            if (m->silent) {
                WA_LOG(wa::log::Level::Info, "CaptureTrackList", "silentRender.start",
                       "dev=" + (spec.source.deviceId.empty()
                                     ? std::string("(default)")
                                     : wa::narrowAscii(spec.source.deviceId)),
                       "requested");
                Result sr = m->silent->open(spec.source.deviceId, AudioFormat{}, nullptr, {});
                if (sr)
                    sr = m->silent->start();
                if (!sr) {
                    WA_LOG(wa::log::Level::Warn, "CaptureTrackList", "silentRender", "",
                           sr.message);
                    m->silent->stop();
                    m->silent->close();
                    m->silent.reset();
                    silentSt = StreamState::Error;
                } else {
                    WA_LOG(wa::log::Level::Info, "CaptureTrackList", "silentRender.started",
                           "", "ok");
                    silentSt = StreamState::Running;
                }
            }
        }

        {
            const AudioFormat fmt = m->backend->stats().actualFormat;
            const uint32_t sr = fmt.sampleRate ? fmt.sampleRate : 48000u;
            const uint16_t ch = fmt.channels ? fmt.channels : static_cast<uint16_t>(1);
            const size_t scopeCap = std::max<size_t>(static_cast<size_t>(sr) * 2u, 1048576u);
            m->tap = std::make_unique<ScopeBuffer>(scopeCap, ch);
            if (!spec.wavPath.empty()) {
                m->wavFatal = true;
                Result wr = m->sink.startExact(spec.wavPath, fmt);
                if (!wr) {
                    WA_LOG(wa::log::Level::Err, "CaptureTrackList", "wav.open", "", wr.message);
                    std::lock_guard<std::mutex> lk(m->mtx);
                    m->status.state = StreamState::Error;
                    m->status.message = wr.message.empty() ? "cannot open output wav"
                                                           : wr.message;
                    m->wavFatal = false;
                }
            }
        }
        const bool wavOpenFailed = m->status.state == StreamState::Error;
        m->running.store(!wavOpenFailed, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(m->mtx);
            if (!wavOpenFailed)
                m->status.state = StreamState::Running;
            m->status.actualFormat = m->backend->stats().actualFormat;
            m->status.silentRenderState = silentSt;
        }
        if (!wavOpenFailed)
        m->pump = std::thread([mem = m.get()] {
            wa::log::setThreadName("ctl-cap");
            AudioFormat fmt = mem->backend->stats().actualFormat;
            const uint32_t frameBytes = fmt.blockAlign();
            const uint16_t ch = fmt.channels ? fmt.channels : static_cast<uint16_t>(1);
            std::vector<uint8_t> buf(16384);
            std::vector<float> interleaved(4096u * ch, 0.f);
            while (mem->running.load(std::memory_order_relaxed)) {
                size_t got = mem->ring->read(buf.data(), buf.size());
                if (got == 0 || frameBytes == 0) {
                    Sleep(5);
                    continue;
                }
                const size_t frames = got / frameBytes;
                got = frames * frameBytes;
                if (frames == 0) continue;
                if (mem->sink.isRunning()) {
                    const size_t wn = mem->sink.push(buf.data(), got);
                    if (wn != got && mem->wavFatal) {
                        WA_LOG(wa::log::Level::Err, "CaptureTrackList", "wav.write", "",
                               "short write");
                        std::lock_guard<std::mutex> lk(mem->mtx);
                        mem->status.state = StreamState::Error;
                        mem->status.message = "wav write failed";
                    }
                }
                if (mem->tap && frames > 0) {
                    size_t done = 0;
                    while (done < frames) {
                        const size_t chunk = (frames - done > 4096u) ? 4096u : (frames - done);
                        pcmToFloat(buf.data() + done * frameBytes, chunk, fmt, interleaved.data());
                        mem->tap->pushInterleaved(interleaved.data(), chunk);
                        done += chunk;
                    }
                }
                float l = 0.f, r = 0.f;
                computeLevels(buf.data(), got, fmt, l, r);
                std::lock_guard<std::mutex> lk(mem->mtx);
                mem->status.levelL = l;
                mem->status.levelR = r;
                mem->status.actualFormat = fmt;
                mem->status.overruns = mem->ring->overruns();
                mem->status.writtenFrames = mem->tap ? mem->tap->totalWritten() : 0;
            }
            mem->sink.stop();
        });
    } catch (const std::exception& e) {
        m->stopPumpAndBackends();
        WA_LOG(wa::log::Level::Err, "CaptureTrackList", "create", "", e.what());
        return Result::Fail(-1, e.what());
    }

    std::lock_guard<std::mutex> lk(mtx_);
    m->id = nextId_++;
    m->status.id = m->id;
    if (outId) *outId = m->id;
    WA_LOG(wa::log::Level::Info, "CaptureTrackList", "create",
           "id=" + std::to_string(m->id)
               + " fmt=" + wa::formatAudio(m->status.actualFormat),
           "ok");
    members_.push_back(std::move(m));
    return Result::Ok();
}

void CaptureTrackList::destroy(TrackId id) {
    std::unique_ptr<Member> gone;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = std::find_if(members_.begin(), members_.end(),
                               [id](const std::unique_ptr<Member>& m) { return m->id == id; });
        if (it == members_.end()) return;
        gone = std::move(*it);
        members_.erase(it);
    }
    WA_LOG(wa::log::Level::Info, "CaptureTrackList", "destroy",
           "id=" + std::to_string(id), "");
    gone->stopPumpAndBackends();
}

void CaptureTrackList::destroyAll() {
    std::vector<std::unique_ptr<Member>> gone;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        gone.swap(members_);
    }
    WA_LOG(wa::log::Level::Info, "CaptureTrackList", "destroyAll",
           "n=" + std::to_string(gone.size()), "");
    for (auto& m : gone) m->stopPumpAndBackends();
}

CaptureTrackList::Member* CaptureTrackList::findUnlocked(TrackId id) {
    return const_cast<Member*>(
        static_cast<const CaptureTrackList*>(this)->findUnlocked(id));
}

const CaptureTrackList::Member* CaptureTrackList::findUnlocked(TrackId id) const {
    for (const auto& m : members_) {
        if (m->id == id) return m.get();
    }
    return nullptr;
}

Result CaptureTrackList::startDump(TrackId id, const std::wstring& folder) {
    Member* m = nullptr;
    AudioFormat fmt{};
    CaptureSource source{};
    {
        std::lock_guard<std::mutex> lk(mtx_);
        m = findUnlocked(id);
        if (!m) return Result::Fail(-1, "CaptureTrackList: unknown track");
        std::lock_guard<std::mutex> mlk(m->mtx);
        if (m->status.state != StreamState::Running)
            return Result::Fail(-1, "CaptureTrackList: track not running");
        if (m->sink.isRunning())
            return Result::Fail(-1, "CaptureTrackList: dump already running");
        fmt = m->status.actualFormat;
        source = m->source;
    }
    const std::wstring prefix = dumpPrefixFor(source);
    Result r = m->sink.start(folder, prefix, fmt);
    if (!r) {
        WA_LOG(wa::log::Level::Err, "CaptureTrackList", "dump.start",
               "id=" + std::to_string(id), r.message);
        return r;
    }
    m->wavFatal = false;
    WA_LOG(wa::log::Level::Info, "CaptureTrackList", "dump.start",
           "id=" + std::to_string(id) + " prefix=" + wa::narrowAscii(prefix), "ok");
    return Result::Ok();
}

Result CaptureTrackList::stopDump(TrackId id) {
    Member* m = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        m = findUnlocked(id);
        if (!m) return Result::Fail(-1, "CaptureTrackList: unknown track");
    }
    const bool wasRunning = m->sink.isRunning();
    Result r = m->sink.stop();
    WA_LOG(r ? wa::log::Level::Info : wa::log::Level::Err, "CaptureTrackList", "dump.stop",
           "id=" + std::to_string(id), r ? "ok" : r.message);
    if (!wasRunning && r)
        return Result::Fail(-1, "CaptureTrackList: dump not running");
    return r;
}

uint64_t CaptureTrackList::written(TrackId id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Member* m = findUnlocked(id);
    return (m && m->tap) ? m->tap->totalWritten() : 0;
}

uint16_t CaptureTrackList::tapChannels(TrackId id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Member* m = findUnlocked(id);
    return (m && m->tap) ? m->tap->channels() : 0;
}

bool CaptureTrackList::snapshotLatest(TrackId id, size_t n, float* out, uint64_t& endIdxOut) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Member* m = findUnlocked(id);
    return (m && m->tap) ? m->tap->snapshotLatest(n, out, endIdxOut) : false;
}

bool CaptureTrackList::snapshotEndingAt(TrackId id, uint64_t endIdx, size_t n, float* out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Member* m = findUnlocked(id);
    return (m && m->tap) ? m->tap->snapshotEndingAt(endIdx, n, out) : false;
}

bool CaptureTrackList::snapshotChannelEndingAt(TrackId id, uint16_t channel, uint64_t endIdx,
                                               size_t n, float* out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Member* m = findUnlocked(id);
    return (m && m->tap) ? m->tap->snapshotChannelEndingAt(channel, endIdx, n, out) : false;
}

std::vector<CaptureTrackStatus> CaptureTrackList::poll() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<CaptureTrackStatus> out;
    out.reserve(members_.size());
    for (const auto& m : members_) {
        std::lock_guard<std::mutex> mlk(m->mtx);
        CaptureTrackStatus s = m->status;
        const WavSinkStatus ds = m->sink.poll();
        s.dumping = ds.state == WavSinkState::Running;
        s.dumpError = ds.state == WavSinkState::Error;
        s.dumpPath = ds.path;
        s.dumpFileName = ds.fileName;
        s.dumpMessage = ds.message;
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace wa
