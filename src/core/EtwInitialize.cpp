#include "EtwInitialize.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <objbase.h>

#include "AudioFormatStr.h"
#include "ComUtil.h"
#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace wa {
namespace {

std::string lowerCopy(std::string s) {
    for (char& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool containsI(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        if (_strnicmp(hay.c_str() + i, needle.c_str(), needle.size()) == 0)
            return true;
    }
    return false;
}

bool deviceIdsAlign(const std::string& eventId, const std::string& sessionId) {
    if (eventId.empty() || sessionId.empty()) return false;
    return _stricmp(eventId.c_str(), sessionId.c_str()) == 0;
}

std::optional<int32_t> parseI32(const std::string& s) {
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    const long v = strtol(s.c_str(), &end, 0);
    if (end == s.c_str() || *end != '\0') return std::nullopt;
    return static_cast<int32_t>(v);
}

std::optional<bool> parseBool(const std::string& s) {
    const std::string l = lowerCopy(s);
    if (l == "1" || l == "true" || l == "yes") return true;
    if (l == "0" || l == "false" || l == "no") return false;
    if (const auto n = parseI32(s)) return *n != 0;
    return std::nullopt;
}

const char* categoryNameFromValue(int v) {
    switch (v) {
        case 0:  return "Other";
        case 1:  return "ForegroundOnlyMedia";
        case 2:  return "BackgroundCapableMedia";
        case 3:  return "Communications";
        case 4:  return "Alerts";
        case 5:  return "SoundEffects";
        case 6:  return "GameEffects";
        case 7:  return "GameMedia";
        case 8:  return "GameChat";
        case 9:  return "Speech";
        case 10: return "Movie";
        case 11: return "Media";
        case 12: return "FarFieldSpeech";
        case 13: return "UniformSpeech";
        case 14: return "VoiceTyping";
        default: return nullptr;
    }
}

std::string mapCategory(const std::string& raw) {
    if (const auto n = parseI32(raw)) {
        if (const char* name = categoryNameFromValue(*n)) return name;
        return std::to_string(*n);
    }
    return raw;
}

int64_t fileTimeToUnixMs(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart / 10000ULL) - 11644473600000LL;
}

void mergeFields(EtwInitializeHint& hint, const EtwEventFields& e) {
    hint.present = true;
    if (e.category) hint.category = e.category;
    if (e.raw) hint.raw = e.raw;
    if (e.matchFormat) hint.matchFormat = e.matchFormat;
    if (e.exclusive) hint.exclusive = e.exclusive;
    if (e.hresult) hint.hresult = e.hresult;
}

Result win32Result(ULONG err, const char* where) {
    if (err == ERROR_SUCCESS) return Result::Ok();
    return HrToResult(HRESULT_FROM_WIN32(err), where);
}

const GUID kMicrosoftWindowsAudio = {
    0xAE4BD3BE, 0xF36F, 0x45B6, {0x8D, 0x21, 0xBD, 0xD6, 0xFB, 0x83, 0x28, 0x53}};
const GUID kMicrosoftWindowsAudioClient = {
    0x6E7B1892, 0x5288, 0x5FE5, {0x8F, 0x34, 0xE3, 0xB0, 0xDC, 0x67, 0x1F, 0xD2}};

bool interestingAudioEvent(USHORT id) {
    return id == 24 || id == 25 || id == 123 || id == 125 || id == 127;
}

std::string wideToUtf8(const wchar_t* w, int nchars = -1) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, nchars, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w, nchars, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::string guidString(const GUID& g) {
    wchar_t w[64] = {};
    const int n = StringFromGUID2(g, w, 64);
    if (n <= 1) return {};
    return wa::narrowAscii(std::wstring(w, static_cast<size_t>(n - 1)));
}

std::string tdhPropertyString(PEVENT_RECORD rec, PTRACE_EVENT_INFO info, USHORT index) {
    const EVENT_PROPERTY_INFO& epi = info->EventPropertyInfoArray[index];
    if ((epi.Flags & PropertyParamCount) != 0 || (epi.Flags & PropertyStruct) != 0)
        return {};
    const wchar_t* name = reinterpret_cast<const wchar_t*>(
        reinterpret_cast<const BYTE*>(info) + epi.NameOffset);
    PROPERTY_DATA_DESCRIPTOR desc{};
    desc.PropertyName = reinterpret_cast<ULONGLONG>(name);
    desc.ArrayIndex = ULONG_MAX;
    ULONG sz = 0;
    ULONG st = TdhGetPropertySize(rec, 0, nullptr, 1, &desc, &sz);
    if (st != ERROR_SUCCESS || sz == 0) return {};
    std::vector<BYTE> buf(sz);
    st = TdhGetProperty(rec, 0, nullptr, 1, &desc, sz, buf.data());
    if (st != ERROR_SUCCESS) return {};

    const USHORT inType = epi.nonStructType.InType;
    switch (inType) {
        case TDH_INTYPE_INT8:
            return std::to_string(static_cast<int>(static_cast<int8_t>(buf[0])));
        case TDH_INTYPE_UINT8:
            return std::to_string(static_cast<unsigned>(buf[0]));
        case TDH_INTYPE_INT16:
            return std::to_string(*reinterpret_cast<int16_t*>(buf.data()));
        case TDH_INTYPE_UINT16:
            return std::to_string(*reinterpret_cast<uint16_t*>(buf.data()));
        case TDH_INTYPE_INT32:
            return std::to_string(*reinterpret_cast<int32_t*>(buf.data()));
        case TDH_INTYPE_UINT32:
            return std::to_string(*reinterpret_cast<uint32_t*>(buf.data()));
        case TDH_INTYPE_INT64:
            return std::to_string(*reinterpret_cast<int64_t*>(buf.data()));
        case TDH_INTYPE_UINT64:
            return std::to_string(*reinterpret_cast<uint64_t*>(buf.data()));
        case TDH_INTYPE_FLOAT:
            return std::to_string(*reinterpret_cast<float*>(buf.data()));
        case TDH_INTYPE_BOOLEAN:
            return (*reinterpret_cast<uint32_t*>(buf.data()) != 0) ? "1" : "0";
        case TDH_INTYPE_UNICODESTRING:
            return wideToUtf8(reinterpret_cast<const wchar_t*>(buf.data()));
        case TDH_INTYPE_ANSISTRING:
            return reinterpret_cast<const char*>(buf.data());
        case TDH_INTYPE_GUID:
            if (sz >= sizeof(GUID))
                return guidString(*reinterpret_cast<const GUID*>(buf.data()));
            return {};
        default:
            break;
    }
    return {};
}

bool decodeRecord(PEVENT_RECORD rec, EtwEventFields& out) {
    ULONG bufSize = 0;
    ULONG st = TdhGetEventInformation(rec, 0, nullptr, nullptr, &bufSize);
    if (st != ERROR_INSUFFICIENT_BUFFER) return false;
    std::vector<BYTE> raw(bufSize);
    auto* info = reinterpret_cast<PTRACE_EVENT_INFO>(raw.data());
    st = TdhGetEventInformation(rec, 0, nullptr, info, &bufSize);
    if (st != ERROR_SUCCESS) {
        WA_LOG(wa::log::Level::Warn, "EtwInit", "TdhGetEventInformation",
               "id=" + std::to_string(rec->EventHeader.EventDescriptor.Id),
               wa::log::hrName(HRESULT_FROM_WIN32(st)));
        return false;
    }

    const bool audioProv = InlineIsEqualGUID(rec->EventHeader.ProviderId, kMicrosoftWindowsAudio);
    const bool clientProv =
        InlineIsEqualGUID(rec->EventHeader.ProviderId, kMicrosoftWindowsAudioClient);
    if (!audioProv && !clientProv) return false;
    if (audioProv && !interestingAudioEvent(rec->EventHeader.EventDescriptor.Id))
        return false;

    std::wstring eventName;
    if (info->EventNameOffset) {
        eventName = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const BYTE*>(info) + info->EventNameOffset);
    }
    if (clientProv && !eventName.empty() &&
        _wcsicmp(eventName.c_str(), L"AudioClientInitialize") != 0) {
        return false;
    }

    std::vector<std::pair<std::string, std::string>> props;
    props.reserve(info->TopLevelPropertyCount);
    for (USHORT i = 0; i < info->TopLevelPropertyCount; ++i) {
        const EVENT_PROPERTY_INFO& epi = info->EventPropertyInfoArray[i];
        const wchar_t* wname = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const BYTE*>(info) + epi.NameOffset);
        const std::string name = wideToUtf8(wname);
        const std::string value = tdhPropertyString(rec, info, i);
        if (!name.empty() && !value.empty())
            props.emplace_back(name, value);
    }

    FILETIME ft{};
    ft.dwLowDateTime = rec->EventHeader.TimeStamp.LowPart;
    ft.dwHighDateTime = rec->EventHeader.TimeStamp.HighPart;
    out = etwFieldsFromProperties(rec->EventHeader.ProcessId, fileTimeToUnixMs(ft), props);
    return out.processId != 0 || out.category || out.raw || out.matchFormat || out.exclusive ||
           out.hresult;
}

struct SessionProps {
    EVENT_TRACE_PROPERTIES props;
    wchar_t loggerName[128];
};

void fillSessionProps(SessionProps& p, const wchar_t* name) {
    ZeroMemory(&p, sizeof(p));
    p.props.Wnode.BufferSize = sizeof(SessionProps);
    p.props.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p.props.Wnode.ClientContext = 2;  // system time (FILETIME)
    p.props.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p.props.LoggerNameOffset = static_cast<ULONG>(offsetof(SessionProps, loggerName));
    p.props.BufferSize = 64;
    p.props.MinimumBuffers = 2;
    p.props.MaximumBuffers = 8;
    p.props.FlushTimer = 1;
    wcsncpy_s(p.loggerName, name, _TRUNCATE);
}

}  // namespace

const char* etwWatchStatusText(EtwWatchStatus status) {
    switch (status) {
        case EtwWatchStatus::Listening:   return "ETW listening";
        case EtwWatchStatus::Unavailable: return "ETW unavailable";
        case EtwWatchStatus::Stopped:     return "ETW stopped";
    }
    return "ETW unavailable";
}

int64_t etwNowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return fileTimeToUnixMs(ft);
}

EtwEventFields etwFieldsFromProperties(
    uint32_t headerPid,
    int64_t timeMs,
    const std::vector<std::pair<std::string, std::string>>& props) {
    EtwEventFields f;
    f.processId = headerPid;
    f.timeMs = timeMs;
    for (const auto& kv : props) {
        const std::string key = lowerCopy(kv.first);
        const std::string& val = kv.second;
        if (key == "pid" || key == "processid") {
            if (const auto n = parseI32(val)) {
                if (*n > 0) f.processId = static_cast<uint32_t>(*n);
            }
        } else if (key == "deviceid" || key == "endpointid" || key == "endpoint" ||
                   key == "device" || key == "endpointidstring") {
            if (f.deviceId.empty()) f.deviceId = val;
        } else if (key == "category" || key == "audiocategory") {
            f.category = mapCategory(val);
        } else if (key == "raw") {
            f.raw = parseBool(val);
        } else if (key == "matchformat" || key == "match_format") {
            f.matchFormat = parseBool(val);
        } else if (key == "hresult" || key == "hr" || key == "result") {
            f.hresult = parseI32(val);
        } else if (key == "sharemode" || key == "exclusive") {
            const std::string l = lowerCopy(val);
            if (containsI(l, "excl")) {
                f.exclusive = true;
            } else if (const auto n = parseI32(val)) {
                f.exclusive = (*n != 0);
            } else {
                const auto b = parseBool(val);
                if (b) f.exclusive = b;
            }
        }
    }
    return f;
}

EtwInitializeHint matchEtwInitialize(
    const LiveSessionView& session,
    const std::vector<EtwEventFields>& events,
    int64_t nowMs,
    int64_t windowMs) {
    EtwInitializeHint hint;
    if (session.processId == 0 || windowMs < 0) return hint;

    std::vector<const EtwEventFields*> hits;
    hits.reserve(events.size());
    for (const auto& e : events) {
        if (e.timeMs > nowMs + 1000) continue;
        if (nowMs - e.timeMs > windowMs) continue;
        const bool pidHit = e.processId != 0 && e.processId == session.processId;
        const bool deviceParsed = !e.deviceId.empty();
        const bool deviceHit = deviceParsed && deviceIdsAlign(e.deviceId, session.deviceId);
        // PlaybackManager / AudioClientInitialize: PID (+ device when parsed).
        // Performance 123/125/127 often run in the audio engine: no app PID,
        // match by parsed endpoint id + time so RAW is not dropped.
        if (pidHit) {
            if (deviceParsed && !deviceHit) continue;
        } else if (!deviceHit) {
            continue;
        }
        hits.push_back(&e);
    }
    if (hits.empty()) return hint;

    std::sort(hits.begin(), hits.end(),
              [](const EtwEventFields* a, const EtwEventFields* b) {
                  return a->timeMs < b->timeMs;
              });
    for (const auto* e : hits)
        mergeFields(hint, *e);
    return hint;
}

struct EtwInitializeWatch::Impl {
    mutable std::mutex mu;
    std::vector<EtwEventFields> events;
    std::atomic<EtwWatchStatus> status{EtwWatchStatus::Stopped};
    TRACEHANDLE session = 0;
    TRACEHANDLE consumer = INVALID_PROCESSTRACE_HANDLE;
    std::thread worker;
    wchar_t name[128] = {};

    void push(EtwEventFields e) {
        const int64_t now = etwNowMs();
        std::lock_guard<std::mutex> lock(mu);
        events.push_back(std::move(e));
        constexpr int64_t kRetainMs = 30000;
        constexpr size_t kMaxEvents = 256;
        const auto isOld = [&](const EtwEventFields& x) {
            return now - x.timeMs > kRetainMs;
        };
        events.erase(std::remove_if(events.begin(), events.end(), isOld), events.end());
        if (events.size() > kMaxEvents)
            events.erase(events.begin(),
                         events.begin() + static_cast<std::ptrdiff_t>(events.size() - kMaxEvents));
    }

    static VOID WINAPI onRecord(PEVENT_RECORD rec) {
        if (!rec || !rec->UserContext) return;
        auto* self = static_cast<Impl*>(rec->UserContext);
        EtwEventFields f;
        if (!decodeRecord(rec, f)) return;
        self->push(std::move(f));
    }

    Result enableProviders() {
        ULONG err = EnableTraceEx2(session, &kMicrosoftWindowsAudio, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                   TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
        WA_LOG(wa::log::Level::Debug, "EtwInit", "EnableTraceEx2(Microsoft-Windows-Audio)",
               "", wa::log::hrName(HRESULT_FROM_WIN32(err)));
        if (err != ERROR_SUCCESS) return win32Result(err, "EtwInit: EnableTraceEx2(Audio)");

        err = EnableTraceEx2(session, &kMicrosoftWindowsAudioClient, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                             TRACE_LEVEL_VERBOSE, 0, 0, 0, nullptr);
        WA_LOG(wa::log::Level::Debug, "EtwInit", "EnableTraceEx2(Microsoft.Windows.Audio.Client)",
               "", wa::log::hrName(HRESULT_FROM_WIN32(err)));
        if (err != ERROR_SUCCESS)
            return win32Result(err, "EtwInit: EnableTraceEx2(Audio.Client)");
        return Result::Ok();
    }

    void disableProviders() {
        if (session == 0) return;
        ULONG err = EnableTraceEx2(session, &kMicrosoftWindowsAudio,
                                   EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
        WA_LOG(wa::log::Level::Debug, "EtwInit", "EnableTraceEx2(disable Audio)", "",
               wa::log::hrName(HRESULT_FROM_WIN32(err)));
        err = EnableTraceEx2(session, &kMicrosoftWindowsAudioClient,
                             EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
        WA_LOG(wa::log::Level::Debug, "EtwInit", "EnableTraceEx2(disable Audio.Client)", "",
               wa::log::hrName(HRESULT_FROM_WIN32(err)));
    }

    void stopSession() {
        disableProviders();
        if (consumer != INVALID_PROCESSTRACE_HANDLE) {
            const ULONG err = CloseTrace(consumer);
            WA_LOG(wa::log::Level::Debug, "EtwInit", "CloseTrace", "",
                   wa::log::hrName(HRESULT_FROM_WIN32(err)));
            consumer = INVALID_PROCESSTRACE_HANDLE;
        }
        if (worker.joinable()) worker.join();
        if (name[0]) {
            SessionProps p{};
            fillSessionProps(p, name);
            const ULONG err = ControlTraceW(session, name, &p.props, EVENT_TRACE_CONTROL_STOP);
            if (err == ERROR_SUCCESS || err == ERROR_WMI_INSTANCE_NOT_FOUND) {
                WA_LOG(wa::log::Level::Debug, "EtwInit", "ControlTrace(STOP)",
                       wa::narrowAscii(name), wa::log::hrName(HRESULT_FROM_WIN32(err)));
            } else {
                WA_LOG(wa::log::Level::Warn, "EtwInit", "ControlTrace(STOP)",
                       wa::narrowAscii(name), wa::log::hrName(HRESULT_FROM_WIN32(err)));
            }
        }
        session = 0;
    }
};

EtwInitializeWatch::EtwInitializeWatch() : impl_(std::make_unique<Impl>()) {}

EtwInitializeWatch::~EtwInitializeWatch() { stop(); }

Result EtwInitializeWatch::start() {
    if (impl_->status.load() == EtwWatchStatus::Listening) return Result::Ok();
    stop();

    swprintf_s(impl_->name, L"WinAudioPplInit-%lu", GetCurrentProcessId());
    SessionProps props{};
    fillSessionProps(props, impl_->name);

    ULONG err = StartTraceW(&impl_->session, impl_->name, &props.props);
    WA_LOG(wa::log::Level::Debug, "EtwInit", "StartTrace", wa::narrowAscii(impl_->name),
           wa::log::hrName(HRESULT_FROM_WIN32(err)));
    if (err == ERROR_ALREADY_EXISTS) {
        const ULONG stopErr = ControlTraceW(0, impl_->name, &props.props, EVENT_TRACE_CONTROL_STOP);
        WA_LOG(wa::log::Level::Debug, "EtwInit", "ControlTrace(STOP leftover)",
               wa::narrowAscii(impl_->name), wa::log::hrName(HRESULT_FROM_WIN32(stopErr)));
        fillSessionProps(props, impl_->name);
        err = StartTraceW(&impl_->session, impl_->name, &props.props);
        WA_LOG(wa::log::Level::Debug, "EtwInit", "StartTrace(retry)", wa::narrowAscii(impl_->name),
               wa::log::hrName(HRESULT_FROM_WIN32(err)));
    }
    if (err != ERROR_SUCCESS) {
        impl_->status = EtwWatchStatus::Unavailable;
        impl_->session = 0;
        WA_LOG(wa::log::Level::Info, "EtwInit", "start", wa::narrowAscii(impl_->name),
               "unavailable");
        return win32Result(err, "EtwInit: StartTrace");
    }

    Result enabled = impl_->enableProviders();
    if (!enabled) {
        impl_->stopSession();
        impl_->status = EtwWatchStatus::Unavailable;
        WA_LOG(wa::log::Level::Info, "EtwInit", "start", enabled.message, "unavailable");
        return enabled;
    }

    EVENT_TRACE_LOGFILEW log{};
    log.LoggerName = impl_->name;
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback = &Impl::onRecord;
    log.Context = impl_.get();
    impl_->consumer = OpenTraceW(&log);
    WA_LOG(wa::log::Level::Debug, "EtwInit", "OpenTrace", wa::narrowAscii(impl_->name),
           impl_->consumer == INVALID_PROCESSTRACE_HANDLE ? "INVALID_PROCESSTRACE_HANDLE" : "ok");
    if (impl_->consumer == INVALID_PROCESSTRACE_HANDLE) {
        const DWORD oe = GetLastError();
        impl_->stopSession();
        impl_->status = EtwWatchStatus::Unavailable;
        WA_LOG(wa::log::Level::Info, "EtwInit", "OpenTrace", wa::narrowAscii(impl_->name),
               wa::log::hrName(HRESULT_FROM_WIN32(oe)));
        return win32Result(oe, "EtwInit: OpenTrace");
    }

    const TRACEHANDLE consumer = impl_->consumer;
    impl_->worker = std::thread([consumer]() {
        wa::log::setThreadName("etw");
        TRACEHANDLE h = consumer;
        ULONG pr = ProcessTrace(&h, 1, nullptr, nullptr);
        WA_LOG(wa::log::Level::Debug, "EtwInit", "ProcessTrace", "",
               wa::log::hrName(HRESULT_FROM_WIN32(pr)));
    });
    impl_->status = EtwWatchStatus::Listening;
    WA_LOG(wa::log::Level::Info, "EtwInit", "start", wa::narrowAscii(impl_->name), "listening");
    return Result::Ok();
}

void EtwInitializeWatch::stop() {
    if (!impl_) return;
    if (impl_->session == 0 && !impl_->worker.joinable())
        return;
    impl_->stopSession();
    if (impl_->status.load() != EtwWatchStatus::Unavailable)
        impl_->status = EtwWatchStatus::Stopped;
    WA_LOG(wa::log::Level::Info, "EtwInit", "stop", wa::narrowAscii(impl_->name), "ok");
}

EtwWatchStatus EtwInitializeWatch::status() const {
    return impl_ ? impl_->status.load() : EtwWatchStatus::Stopped;
}

std::vector<EtwEventFields> EtwInitializeWatch::snapshot() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->events;
}

}  // namespace wa
