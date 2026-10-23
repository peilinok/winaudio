#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <audiopolicy.h>
#include <objbase.h>
#include <cstring>
#include <cstdint>
#include "HookedCallPod.h"

using wa::hook_ipc::CallPod;
using wa::hook_ipc::Ring;
using wa::hook_ipc::kCap;
using wa::hook_ipc::kPumpCap;
using wa::hook_ipc::kMagic;
using wa::hook_ipc::kInstallPending;
using wa::hook_ipc::kInstallOk;
using wa::hook_ipc::kInstallFailed;

namespace {

Ring* g_ring = nullptr;
HANDLE g_map = nullptr;
volatile LONG g_quiet = 0;

using ActivateFn = HRESULT(STDMETHODCALLTYPE*)(IMMDevice*, REFIID, DWORD, PROPVARIANT*, void**);
using GetDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, LPCWSTR, IMMDevice**);
using GetDefaultFn = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, EDataFlow, ERole, IMMDevice**);
using EnumFn = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, EDataFlow, DWORD, IMMDeviceCollection**);
using InitializeFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, AUDCLNT_SHAREMODE, DWORD,
                                                 REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*,
                                                 LPCGUID);
using InitSharedFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient3*, DWORD, UINT32, const WAVEFORMATEX*,
                                                 LPCGUID);
using SimpleHrFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*);
using SetEventFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, HANDLE);
using GetServiceFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, REFIID, void**);
using SetPropsFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient2*, const AudioClientProperties*);
using VolGetFn = HRESULT(STDMETHODCALLTYPE*)(ISimpleAudioVolume*, float*);
using VolSetFn = HRESULT(STDMETHODCALLTYPE*)(ISimpleAudioVolume*, float, LPCGUID);
using MuteGetFn = HRESULT(STDMETHODCALLTYPE*)(ISimpleAudioVolume*, BOOL*);
using MuteSetFn = HRESULT(STDMETHODCALLTYPE*)(ISimpleAudioVolume*, BOOL, LPCGUID);
using SessStateFn = HRESULT(STDMETHODCALLTYPE*)(IAudioSessionControl*, AudioSessionState*);
using ClockFreqFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClock*, UINT64*);
using CapGetBufFn = HRESULT(STDMETHODCALLTYPE*)(IAudioCaptureClient*, BYTE**, UINT32*, DWORD*,
                                                UINT64*, UINT64*);
using CapRelBufFn = HRESULT(STDMETHODCALLTYPE*)(IAudioCaptureClient*, UINT32);
using RenGetBufFn = HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, BYTE**);
using RenRelBufFn = HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, DWORD);
using GetBufSizeFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, UINT32*);
using StreamVolCountFn = HRESULT(STDMETHODCALLTYPE*)(IAudioStreamVolume*, UINT32*);
using StreamVolSetFn = HRESULT(STDMETHODCALLTYPE*)(IAudioStreamVolume*, UINT32, float);
using StreamVolGetFn = HRESULT(STDMETHODCALLTYPE*)(IAudioStreamVolume*, UINT32, float*);
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
using FxGetFn = HRESULT(STDMETHODCALLTYPE*)(IAudioEffectsManager*, AUDIO_EFFECT**, UINT32*);
using FxSetFn = HRESULT(STDMETHODCALLTYPE*)(IAudioEffectsManager*, GUID, AUDIO_EFFECT_STATE);
#endif

ActivateFn g_activate = nullptr;
GetDeviceFn g_getDevice = nullptr;
GetDefaultFn g_getDefault = nullptr;
EnumFn g_enum = nullptr;
InitializeFn g_initialize = nullptr;
InitSharedFn g_initShared = nullptr;
SimpleHrFn g_start = nullptr;
SimpleHrFn g_stop = nullptr;
SimpleHrFn g_reset = nullptr;
SetEventFn g_setEvent = nullptr;
GetServiceFn g_getService = nullptr;
SetPropsFn g_setProps = nullptr;
VolGetFn g_volGet = nullptr;
VolSetFn g_volSet = nullptr;
MuteGetFn g_muteGet = nullptr;
MuteSetFn g_muteSet = nullptr;
SessStateFn g_sessState = nullptr;
ClockFreqFn g_clockFreq = nullptr;
CapGetBufFn g_capGet = nullptr;
CapRelBufFn g_capRel = nullptr;
RenGetBufFn g_renGet = nullptr;
RenRelBufFn g_renRel = nullptr;
GetBufSizeFn g_getBufSize = nullptr;
StreamVolCountFn g_streamVolCount = nullptr;
StreamVolSetFn g_streamVolSet = nullptr;
StreamVolGetFn g_streamVolGet = nullptr;
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
FxGetFn g_fxGet = nullptr;
FxSetFn g_fxSet = nullptr;
#endif

void copyStr(char* dst, size_t n, const char* src) {
    if (!dst || n == 0) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    strncpy_s(dst, n, src, _TRUNCATE);
}

int64_t nowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart / 10000ULL) - 11644473600000LL;
}

void emit(CallPod pod) {
    if (!g_ring || g_quiet || !g_ring->captureEnabled) return;
    pod.timeMs = nowMs();
    if (pod.pump) {
        if (!g_ring->pumpEnabled) return;
        if (pod.xrun)
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_ring->pumpXruns));
        const LONG idx =
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_ring->pumpWriteIndex));
        g_ring->pumpSlots[static_cast<uint32_t>(idx - 1) % kPumpCap] = pod;
        return;
    }
    const LONG idx = InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_ring->writeIndex));
    const uint32_t slot = static_cast<uint32_t>(idx - 1) % kCap;
    g_ring->slots[slot] = pod;
}

void emitSimple(uint32_t streamId, const char* iface, const char* method, const char* args,
                HRESULT hr) {
    CallPod p{};
    p.streamId = streamId;
    copyStr(p.iface, sizeof(p.iface), iface);
    copyStr(p.method, sizeof(p.method), method);
    copyStr(p.args, sizeof(p.args), args);
    p.hresult = static_cast<int32_t>(hr);
    emit(p);
}

uint32_t sid(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }

const char* iidName(REFIID iid) {
    if (iid == __uuidof(IAudioClient)) return "IAudioClient";
    if (iid == __uuidof(IAudioClient2)) return "IAudioClient2";
    if (iid == __uuidof(IAudioClient3)) return "IAudioClient3";
    if (iid == __uuidof(IAudioCaptureClient)) return "IAudioCaptureClient";
    if (iid == __uuidof(IAudioRenderClient)) return "IAudioRenderClient";
    if (iid == __uuidof(IAudioClock)) return "IAudioClock";
    if (iid == __uuidof(ISimpleAudioVolume)) return "ISimpleAudioVolume";
    if (iid == __uuidof(IAudioSessionControl)) return "IAudioSessionControl";
    if (iid == __uuidof(IAudioSessionControl2)) return "IAudioSessionControl2";
    if (iid == __uuidof(IAudioStreamVolume)) return "IAudioStreamVolume";
#ifdef __IAudioEffectsManager_INTERFACE_DEFINED__
    if (iid == __uuidof(IAudioEffectsManager)) return "IAudioEffectsManager";
#endif
    return "unknown";
}

const char* categoryName(int v) {
    switch (v) {
        case 0: return "Other";
        case 3: return "Communications";
        case 5: return "SoundEffects";
        case 7: return "GameMedia";
        case 8: return "GameChat";
        case 9: return "Speech";
        case 10: return "Movie";
        case 11: return "Media";
        default: return "Other";
    }
}

void formatWave(const WAVEFORMATEX* fmt, char* out, size_t n) {
    if (!fmt) {
        copyStr(out, n, "null");
        return;
    }
    const char* fl = (fmt->wFormatTag == 3) ? "f" : "";
    sprintf_s(out, n, "%u/%u%s/%u", fmt->nSamplesPerSec, fmt->wBitsPerSample, fl, fmt->nChannels);
}


void formatPtr(const void* p, char* out, size_t n) {
    if (!p) {
        copyStr(out, n, "NULL");
        return;
    }
    sprintf_s(out, n, "0x%p", p);
}

void formatHr(HRESULT hr, char* out, size_t n) {
    switch (hr) {
        case S_OK: copyStr(out, n, "S_OK"); return;
        case S_FALSE: copyStr(out, n, "S_FALSE"); return;
        case E_INVALIDARG: copyStr(out, n, "E_INVALIDARG"); return;
        case E_POINTER: copyStr(out, n, "E_POINTER"); return;
        case E_NOINTERFACE: copyStr(out, n, "E_NOINTERFACE"); return;
        case E_OUTOFMEMORY: copyStr(out, n, "E_OUTOFMEMORY"); return;
        case E_FAIL: copyStr(out, n, "E_FAIL"); return;
        case E_ACCESSDENIED: copyStr(out, n, "E_ACCESSDENIED"); return;
        case AUDCLNT_E_NOT_INITIALIZED: copyStr(out, n, "AUDCLNT_E_NOT_INITIALIZED"); return;
        case AUDCLNT_E_ALREADY_INITIALIZED: copyStr(out, n, "AUDCLNT_E_ALREADY_INITIALIZED"); return;
        case AUDCLNT_E_WRONG_ENDPOINT_TYPE: copyStr(out, n, "AUDCLNT_E_WRONG_ENDPOINT_TYPE"); return;
        case AUDCLNT_E_DEVICE_INVALIDATED: copyStr(out, n, "AUDCLNT_E_DEVICE_INVALIDATED"); return;
        case AUDCLNT_E_NOT_STOPPED: copyStr(out, n, "AUDCLNT_E_NOT_STOPPED"); return;
        case AUDCLNT_E_BUFFER_TOO_LARGE: copyStr(out, n, "AUDCLNT_E_BUFFER_TOO_LARGE"); return;
        case AUDCLNT_E_UNSUPPORTED_FORMAT: copyStr(out, n, "AUDCLNT_E_UNSUPPORTED_FORMAT"); return;
        case AUDCLNT_E_DEVICE_IN_USE: copyStr(out, n, "AUDCLNT_E_DEVICE_IN_USE"); return;
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
            copyStr(out, n, "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED");
            return;
        case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED:
            copyStr(out, n, "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED");
            return;
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
            copyStr(out, n, "AUDCLNT_E_ENDPOINT_CREATE_FAILED");
            return;
        case AUDCLNT_E_SERVICE_NOT_RUNNING: copyStr(out, n, "AUDCLNT_E_SERVICE_NOT_RUNNING"); return;
        case AUDCLNT_S_BUFFER_EMPTY: copyStr(out, n, "AUDCLNT_S_BUFFER_EMPTY"); return;
        default:
            sprintf_s(out, n, "0x%08X", static_cast<unsigned>(hr));
            return;
    }
}

const char* shareModeName(AUDCLNT_SHAREMODE share) {
    return share == AUDCLNT_SHAREMODE_EXCLUSIVE ? "AUDCLNT_SHAREMODE_EXCLUSIVE"
                                                : "AUDCLNT_SHAREMODE_SHARED";
}

void formatStreamFlags(DWORD flags, char* out, size_t n) {
    if (flags == 0) {
        copyStr(out, n, "0x0");
        return;
    }
    char buf[160] = {};
    size_t used = 0;
    auto add = [&](const char* name, DWORD bit) {
        if (!(flags & bit)) return;
        if (used && used + 1 < sizeof(buf)) buf[used++] = '|';
        const size_t ln = strlen(name);
        if (used + ln < sizeof(buf)) {
            memcpy(buf + used, name, ln);
            used += ln;
        }
    };
    add("AUDCLNT_STREAMFLAGS_CROSSPROCESS", AUDCLNT_STREAMFLAGS_CROSSPROCESS);
    add("AUDCLNT_STREAMFLAGS_LOOPBACK", AUDCLNT_STREAMFLAGS_LOOPBACK);
    add("AUDCLNT_STREAMFLAGS_EVENTCALLBACK", AUDCLNT_STREAMFLAGS_EVENTCALLBACK);
    add("AUDCLNT_STREAMFLAGS_NOPERSIST", AUDCLNT_STREAMFLAGS_NOPERSIST);
    add("AUDCLNT_STREAMFLAGS_RATEADJUST", AUDCLNT_STREAMFLAGS_RATEADJUST);
    add("AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY", AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);
    add("AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM", AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM);
    add("AUDCLNT_SESSIONFLAGS_EXPIREWHENUNOWNED", AUDCLNT_SESSIONFLAGS_EXPIREWHENUNOWNED);
    add("AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE", AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE);
    add("AUDCLNT_SESSIONFLAGS_DISPLAY_HIDEWHENEXPIRED", AUDCLNT_SESSIONFLAGS_DISPLAY_HIDEWHENEXPIRED);
    if (used == 0)
        sprintf_s(out, n, "0x%lX", static_cast<unsigned long>(flags));
    else {
        buf[used] = 0;
        sprintf_s(out, n, "0x%lX (%s)", static_cast<unsigned long>(flags), buf);
    }
}

void formatGuid(const GUID* g, char* out, size_t n) {
    if (!g) {
        copyStr(out, n, "NULL");
        return;
    }
    sprintf_s(out, n, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
              static_cast<unsigned long>(g->Data1), g->Data2, g->Data3, g->Data4[0], g->Data4[1],
              g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

const char* waveFormatTagName(WORD tag) {
    switch (tag) {
        case WAVE_FORMAT_PCM: return "WAVE_FORMAT_PCM";
        case WAVE_FORMAT_IEEE_FLOAT: return "WAVE_FORMAT_IEEE_FLOAT";
        case WAVE_FORMAT_EXTENSIBLE: return "WAVE_FORMAT_EXTENSIBLE";
        default: return nullptr;
    }
}

void formatWaveTag(WORD tag, char* out, size_t n) {
    if (const char* named = waveFormatTagName(tag))
        sprintf_s(out, n, "%s (0x%04X)", named, tag);
    else
        sprintf_s(out, n, "0x%04X", tag);
}

void formatSubFormat(const GUID& g, char* out, size_t n) {
    if (g == KSDATAFORMAT_SUBTYPE_PCM)
        copyStr(out, n, "KSDATAFORMAT_SUBTYPE_PCM");
    else if (g == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
        copyStr(out, n, "KSDATAFORMAT_SUBTYPE_IEEE_FLOAT");
    else
        formatGuid(&g, out, n);
}

void pushWaveFormat(wa::hook_ipc::ArgBlobWriter& w, uint8_t depth, const WAVEFORMATEX* fmt) {
    using wa::hook_ipc::kArgIn;
    if (!fmt) return;
    char num[64] = {};
    char tag[64] = {};
    formatWaveTag(fmt->wFormatTag, tag, sizeof(tag));
    w.push(depth, kArgIn, "WORD", "wFormatTag", tag, tag);
    sprintf_s(num, "%u", fmt->nChannels);
    w.push(depth, kArgIn, "WORD", "nChannels", num, num);
    sprintf_s(num, "%u", fmt->nSamplesPerSec);
    w.push(depth, kArgIn, "DWORD", "nSamplesPerSec", num, num);
    sprintf_s(num, "%u", fmt->nAvgBytesPerSec);
    w.push(depth, kArgIn, "DWORD", "nAvgBytesPerSec", num, num);
    sprintf_s(num, "%u", fmt->nBlockAlign);
    w.push(depth, kArgIn, "WORD", "nBlockAlign", num, num);
    sprintf_s(num, "%u", fmt->wBitsPerSample);
    w.push(depth, kArgIn, "WORD", "wBitsPerSample", num, num);
    sprintf_s(num, "%u", fmt->cbSize);
    w.push(depth, kArgIn, "WORD", "cbSize", num, num);
    const bool extensible =
        fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        fmt->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    if (!extensible) return;
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
    sprintf_s(num, "%u", ext->Samples.wValidBitsPerSample);
    w.push(depth, kArgIn, "WORD", "wValidBitsPerSample", num, num);
    sprintf_s(num, "0x%08lX", static_cast<unsigned long>(ext->dwChannelMask));
    w.push(depth, kArgIn, "DWORD", "dwChannelMask", num, num);
    char sub[80] = {};
    formatSubFormat(ext->SubFormat, sub, sizeof(sub));
    w.push(depth, kArgIn, "GUID", "SubFormat", sub, sub);
}

void sealArgs(CallPod& p, wa::hook_ipc::ArgBlobWriter& w) {
    p.argBlobLen = (w.len <= sizeof(p.argBlob)) ? w.len : 0;
}

void beginArgs(CallPod& p, wa::hook_ipc::ArgBlobWriter& w) {
    w.reset(p.argBlob, static_cast<uint16_t>(sizeof(p.argBlob)));
    p.argBlobLen = 0;
}

void pushReturn(wa::hook_ipc::ArgBlobWriter& w, HRESULT hr) {
    char buf[64] = {};
    formatHr(hr, buf, sizeof(buf));
    w.push(0, wa::hook_ipc::kArgRet, "HRESULT", "Return", buf, buf);
}


bool patchSlot(void* obj, int slot, void* hook, void** orig) {
    return wa::hook_ipc::patchVtableSlot(obj, slot, hook, orig);
}

HRESULT STDMETHODCALLTYPE HookInitialize(IAudioClient* self, AUDCLNT_SHAREMODE share, DWORD flags,
                                         REFERENCE_TIME dur, REFERENCE_TIME per,
                                         const WAVEFORMATEX* fmt, LPCGUID session) {
    const HRESULT hr = g_initialize ? g_initialize(self, share, flags, dur, per, fmt, session)
                                    : E_UNEXPECTED;
    CallPod p{};
    p.streamId = sid(self);
    copyStr(p.iface, sizeof(p.iface), "IAudioClient");
    copyStr(p.method, sizeof(p.method), "Initialize");
    char wave[64] = {};
    formatWave(fmt, wave, sizeof(wave));
    sprintf_s(p.args, "ShareMode=%s StreamFlags=0x%lX hnsBufferDuration=%lld hnsPeriodicity=%lld fmt=%s",
              share == AUDCLNT_SHAREMODE_EXCLUSIVE ? "EXCLUSIVE" : "SHARED",
              static_cast<unsigned long>(flags), static_cast<long long>(dur),
              static_cast<long long>(per), wave);
    p.hresult = static_cast<int32_t>(hr);
    p.hasExclusive = 1;
    p.exclusive = share == AUDCLNT_SHAREMODE_EXCLUSIVE ? 1 : 0;
    p.hasFormat = 1;
    copyStr(p.format, sizeof(p.format), wave);

    wa::hook_ipc::ArgBlobWriter w;
    beginArgs(p, w);
    char tmp[96] = {};
    formatPtr(self, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "IAudioClient*", "this", tmp, tmp);
    copyStr(tmp, sizeof(tmp), shareModeName(share));
    w.push(0, wa::hook_ipc::kArgIn, "AUDCLNT_SHAREMODE", "ShareMode", tmp, tmp);
    formatStreamFlags(flags, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "DWORD", "StreamFlags", tmp, tmp);
    sprintf_s(tmp, "%lld", static_cast<long long>(dur));
    w.push(0, wa::hook_ipc::kArgIn, "REFERENCE_TIME", "hnsBufferDuration", tmp, tmp);
    sprintf_s(tmp, "%lld", static_cast<long long>(per));
    w.push(0, wa::hook_ipc::kArgIn, "REFERENCE_TIME", "hnsPeriodicity", tmp, tmp);
    formatPtr(fmt, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "WAVEFORMATEX*", "pFormat", tmp, tmp);
    if (fmt) {
        w.push(1, wa::hook_ipc::kArgIn, "WAVEFORMATEX", "*pFormat", wave, wave);
        pushWaveFormat(w, 2, fmt);
    }
    formatPtr(session, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "LPCGUID", "AudioSessionGuid", tmp, tmp);
    if (session) {
        formatGuid(session, tmp, sizeof(tmp));
        w.push(1, wa::hook_ipc::kArgIn, "GUID", "*AudioSessionGuid", tmp, tmp);
    }
    pushReturn(w, hr);
    sealArgs(p, w);
    emit(p);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookStart(IAudioClient* self) {
    const HRESULT hr = g_start ? g_start(self) : E_UNEXPECTED;
    emitSimple(sid(self), "IAudioClient", "Start", "", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookStop(IAudioClient* self) {
    const HRESULT hr = g_stop ? g_stop(self) : E_UNEXPECTED;
    emitSimple(sid(self), "IAudioClient", "Stop", "", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookReset(IAudioClient* self) {
    const HRESULT hr = g_reset ? g_reset(self) : E_UNEXPECTED;
    emitSimple(sid(self), "IAudioClient", "Reset", "", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookSetEventHandle(IAudioClient* self, HANDLE ev) {
    const HRESULT hr = g_setEvent ? g_setEvent(self, ev) : E_UNEXPECTED;
    char args[48] = {};
    sprintf_s(args, "handle=0x%p", ev);
    emitSimple(sid(self), "IAudioClient", "SetEventHandle", args, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookVolGet(ISimpleAudioVolume* self, float* level) {
    const HRESULT hr = g_volGet ? g_volGet(self, level) : E_UNEXPECTED;
    char args[48] = {};
    if (SUCCEEDED(hr) && level) sprintf_s(args, "level=%.3f", static_cast<double>(*level));
    emitSimple(sid(self), "ISimpleAudioVolume", "GetMasterVolume", args, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookVolSet(ISimpleAudioVolume* self, float level, LPCGUID ctx) {
    const HRESULT hr = g_volSet ? g_volSet(self, level, ctx) : E_UNEXPECTED;
    char args[48] = {};
    sprintf_s(args, "level=%.3f", static_cast<double>(level));
    emitSimple(sid(self), "ISimpleAudioVolume", "SetMasterVolume", args, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookMuteGet(ISimpleAudioVolume* self, BOOL* mute) {
    const HRESULT hr = g_muteGet ? g_muteGet(self, mute) : E_UNEXPECTED;
    emitSimple(sid(self), "ISimpleAudioVolume", "GetMute",
               (SUCCEEDED(hr) && mute && *mute) ? "true" : "false", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookMuteSet(ISimpleAudioVolume* self, BOOL mute, LPCGUID ctx) {
    const HRESULT hr = g_muteSet ? g_muteSet(self, mute, ctx) : E_UNEXPECTED;
    emitSimple(sid(self), "ISimpleAudioVolume", "SetMute", mute ? "true" : "false", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookSessState(IAudioSessionControl* self, AudioSessionState* st) {
    const HRESULT hr = g_sessState ? g_sessState(self, st) : E_UNEXPECTED;
    const char* name = "unknown";
    if (SUCCEEDED(hr) && st) {
        if (*st == AudioSessionStateActive) name = "Active";
        else if (*st == AudioSessionStateInactive) name = "Inactive";
        else if (*st == AudioSessionStateExpired) name = "Expired";
    }
    emitSimple(sid(self), "IAudioSessionControl", "GetState", name, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookClockFreq(IAudioClock* self, UINT64* freq) {
    const HRESULT hr = g_clockFreq ? g_clockFreq(self, freq) : E_UNEXPECTED;
    char args[48] = {};
    if (SUCCEEDED(hr) && freq) sprintf_s(args, "hz=%llu", static_cast<unsigned long long>(*freq));
    emitSimple(sid(self), "IAudioClock", "GetFrequency", args, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCapGetBuffer(IAudioCaptureClient* self, BYTE** data, UINT32* frames,
                                           DWORD* flags, UINT64* pos, UINT64* qpc) {
    const HRESULT hr =
        g_capGet ? g_capGet(self, data, frames, flags, pos, qpc) : E_UNEXPECTED;
    CallPod p{};
    p.streamId = sid(self);
    p.pump = 1;
    copyStr(p.iface, sizeof(p.iface), "IAudioCaptureClient");
    copyStr(p.method, sizeof(p.method), "GetBuffer");
    const UINT32 n = (SUCCEEDED(hr) && frames) ? *frames : 0;
    const DWORD fl = flags ? *flags : 0;
    sprintf_s(p.args, "frames=%u flags=0x%lX", n, static_cast<unsigned long>(fl));
    p.hresult = static_cast<int32_t>(hr);
    p.xrun = (FAILED(hr) || (fl & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) ? 1 : 0;
    emit(p);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCapReleaseBuffer(IAudioCaptureClient* self, UINT32 frames) {
    const HRESULT hr = g_capRel ? g_capRel(self, frames) : E_UNEXPECTED;
    CallPod p{};
    p.streamId = sid(self);
    p.pump = 1;
    copyStr(p.iface, sizeof(p.iface), "IAudioCaptureClient");
    copyStr(p.method, sizeof(p.method), "ReleaseBuffer");
    sprintf_s(p.args, "frames=%u", frames);
    p.hresult = static_cast<int32_t>(hr);
    p.xrun = FAILED(hr) ? 1 : 0;
    emit(p);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookRenGetBuffer(IAudioRenderClient* self, UINT32 frames, BYTE** data) {
    const HRESULT hr = g_renGet ? g_renGet(self, frames, data) : E_UNEXPECTED;
    CallPod p{};
    p.streamId = sid(self);
    p.pump = 1;
    copyStr(p.iface, sizeof(p.iface), "IAudioRenderClient");
    copyStr(p.method, sizeof(p.method), "GetBuffer");
    sprintf_s(p.args, "frames=%u", frames);
    p.hresult = static_cast<int32_t>(hr);
    p.xrun = FAILED(hr) ? 1 : 0;
    emit(p);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookRenReleaseBuffer(IAudioRenderClient* self, UINT32 frames,
                                               DWORD flags) {
    const HRESULT hr = g_renRel ? g_renRel(self, frames, flags) : E_UNEXPECTED;
    CallPod p{};
    p.streamId = sid(self);
    p.pump = 1;
    copyStr(p.iface, sizeof(p.iface), "IAudioRenderClient");
    copyStr(p.method, sizeof(p.method), "ReleaseBuffer");
    sprintf_s(p.args, "frames=%u flags=0x%lX", frames, static_cast<unsigned long>(flags));
    p.hresult = static_cast<int32_t>(hr);
    p.xrun = FAILED(hr) ? 1 : 0;
    emit(p);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookGetBufferSize(IAudioClient* self, UINT32* frames) {
    const HRESULT hr = g_getBufSize ? g_getBufSize(self, frames) : E_UNEXPECTED;
    char args[48] = {};
    if (SUCCEEDED(hr) && frames) sprintf_s(args, "frames=%u", *frames);
    emitSimple(sid(self), "IAudioClient", "GetBufferSize", args, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookStreamVolCount(IAudioStreamVolume* self, UINT32* count) {
    const HRESULT hr = g_streamVolCount ? g_streamVolCount(self, count) : E_UNEXPECTED;
    char args[48] = {};
    if (SUCCEEDED(hr) && count) sprintf_s(args, "channels=%u", *count);
    emitSimple(sid(self), "IAudioStreamVolume", "GetChannelCount", args, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookStreamVolSet(IAudioStreamVolume* self, UINT32 index, float level) {
    const HRESULT hr = g_streamVolSet ? g_streamVolSet(self, index, level) : E_UNEXPECTED;
    char args[64] = {};
    sprintf_s(args, "ch=%u level=%.3f", index, static_cast<double>(level));
    emitSimple(sid(self), "IAudioStreamVolume", "SetChannelVolume", args, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookStreamVolGet(IAudioStreamVolume* self, UINT32 index, float* level) {
    const HRESULT hr = g_streamVolGet ? g_streamVolGet(self, index, level) : E_UNEXPECTED;
    char args[64] = {};
    if (SUCCEEDED(hr) && level)
        sprintf_s(args, "ch=%u level=%.3f", index, static_cast<double>(*level));
    else
        sprintf_s(args, "ch=%u", index);
    emitSimple(sid(self), "IAudioStreamVolume", "GetChannelVolume", args, hr);
    return hr;
}

#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
HRESULT STDMETHODCALLTYPE HookFxGet(IAudioEffectsManager* self, AUDIO_EFFECT** effects, UINT32* n) {
    const HRESULT hr = g_fxGet ? g_fxGet(self, effects, n) : E_UNEXPECTED;
    char args[48] = {};
    if (SUCCEEDED(hr) && n) sprintf_s(args, "count=%u", *n);
    emitSimple(sid(self), "IAudioEffectsManager", "GetAudioEffects", args, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookFxSet(IAudioEffectsManager* self, GUID id, AUDIO_EFFECT_STATE st) {
    const HRESULT hr = g_fxSet ? g_fxSet(self, id, st) : E_UNEXPECTED;
    char args[80] = {};
    sprintf_s(args, "state=%s", st == AUDIO_EFFECT_STATE_ON ? "on" : "off");
    emitSimple(sid(self), "IAudioEffectsManager", "SetAudioEffectState", args, hr);
    return hr;
}
#endif

void patchService(void* obj, REFIID iid) {
    if (!obj) return;
    using wa::hook_ipc::kSlotBufferGetBuffer;
    using wa::hook_ipc::kSlotBufferReleaseBuffer;
    if (iid == __uuidof(IAudioCaptureClient)) {
        patchSlot(obj, kSlotBufferGetBuffer, reinterpret_cast<void*>(&HookCapGetBuffer),
                  reinterpret_cast<void**>(&g_capGet));
        patchSlot(obj, kSlotBufferReleaseBuffer, reinterpret_cast<void*>(&HookCapReleaseBuffer),
                  reinterpret_cast<void**>(&g_capRel));
    } else if (iid == __uuidof(IAudioRenderClient)) {
        patchSlot(obj, kSlotBufferGetBuffer, reinterpret_cast<void*>(&HookRenGetBuffer),
                  reinterpret_cast<void**>(&g_renGet));
        patchSlot(obj, kSlotBufferReleaseBuffer, reinterpret_cast<void*>(&HookRenReleaseBuffer),
                  reinterpret_cast<void**>(&g_renRel));
    } else if (iid == __uuidof(ISimpleAudioVolume)) {
        patchSlot(obj, 3, reinterpret_cast<void*>(&HookVolGet), reinterpret_cast<void**>(&g_volGet));
        patchSlot(obj, 4, reinterpret_cast<void*>(&HookVolSet), reinterpret_cast<void**>(&g_volSet));
        patchSlot(obj, 5, reinterpret_cast<void*>(&HookMuteGet), reinterpret_cast<void**>(&g_muteGet));
        patchSlot(obj, 6, reinterpret_cast<void*>(&HookMuteSet), reinterpret_cast<void**>(&g_muteSet));
    } else if (iid == __uuidof(IAudioSessionControl) || iid == __uuidof(IAudioSessionControl2)) {
        patchSlot(obj, 3, reinterpret_cast<void*>(&HookSessState),
                  reinterpret_cast<void**>(&g_sessState));
    } else if (iid == __uuidof(IAudioClock)) {
        patchSlot(obj, 3, reinterpret_cast<void*>(&HookClockFreq),
                  reinterpret_cast<void**>(&g_clockFreq));
    } else if (iid == __uuidof(IAudioStreamVolume)) {
        patchSlot(obj, 3, reinterpret_cast<void*>(&HookStreamVolCount),
                  reinterpret_cast<void**>(&g_streamVolCount));
        patchSlot(obj, 4, reinterpret_cast<void*>(&HookStreamVolSet),
                  reinterpret_cast<void**>(&g_streamVolSet));
        patchSlot(obj, 5, reinterpret_cast<void*>(&HookStreamVolGet),
                  reinterpret_cast<void**>(&g_streamVolGet));
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
    } else if (iid == __uuidof(IAudioEffectsManager)) {
        patchSlot(obj, 3, reinterpret_cast<void*>(&HookFxGet), reinterpret_cast<void**>(&g_fxGet));
        patchSlot(obj, 4, reinterpret_cast<void*>(&HookFxSet), reinterpret_cast<void**>(&g_fxSet));
#endif
    }
}

HRESULT STDMETHODCALLTYPE HookGetService(IAudioClient* self, REFIID iid, void** pp) {
    const HRESULT hr = g_getService ? g_getService(self, iid, pp) : E_UNEXPECTED;
    CallPod p{};
    p.streamId = sid(self);
    copyStr(p.iface, sizeof(p.iface), "IAudioClient");
    copyStr(p.method, sizeof(p.method), "GetService");
    sprintf_s(p.args, "iid=%s", iidName(iid));
    p.hresult = static_cast<int32_t>(hr);
    wa::hook_ipc::ArgBlobWriter w;
    beginArgs(p, w);
    char tmp[96] = {};
    formatPtr(self, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "IAudioClient*", "this", tmp, tmp);
    copyStr(tmp, sizeof(tmp), iidName(iid));
    w.push(0, wa::hook_ipc::kArgIn, "REFIID", "riid", tmp, tmp);
    formatPtr(pp ? *pp : nullptr, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgOut, "void**", "ppv", "", tmp);
    pushReturn(w, hr);
    sealArgs(p, w);
    emit(p);
    if (SUCCEEDED(hr) && pp && *pp) patchService(*pp, iid);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookSetClientProperties(IAudioClient2* self,
                                                  const AudioClientProperties* props) {
    const HRESULT hr = g_setProps ? g_setProps(self, props) : E_UNEXPECTED;
    CallPod p{};
    p.streamId = sid(self);
    copyStr(p.iface, sizeof(p.iface), "IAudioClient2");
    copyStr(p.method, sizeof(p.method), "SetClientProperties");
    p.hresult = static_cast<int32_t>(hr);
    wa::hook_ipc::ArgBlobWriter w;
    beginArgs(p, w);
    char tmp[96] = {};
    formatPtr(self, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "IAudioClient2*", "this", tmp, tmp);
    formatPtr(props, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "AudioClientProperties*", "pProperties", tmp, tmp);
    if (props) {
        p.hasCategory = 1;
        copyStr(p.category, sizeof(p.category), categoryName(static_cast<int>(props->eCategory)));
        p.hasRaw = 1;
        p.raw = (props->Options & AUDCLNT_STREAMOPTIONS_RAW) ? 1 : 0;
        p.hasMatchFormat = 1;
        p.matchFormat = (props->Options & AUDCLNT_STREAMOPTIONS_MATCH_FORMAT) ? 1 : 0;
        sprintf_s(p.args, "category=%s raw=%u matchformat=%u", p.category, p.raw, p.matchFormat);
        sprintf_s(tmp, "%u", props->cbSize);
        w.push(1, wa::hook_ipc::kArgIn, "UINT32", "cbSize", tmp, tmp);
        w.push(1, wa::hook_ipc::kArgIn, "AUDIO_STREAM_CATEGORY", "eCategory", p.category, p.category);
        sprintf_s(tmp, "0x%lX", static_cast<unsigned long>(props->Options));
        w.push(1, wa::hook_ipc::kArgIn, "AUDCLNT_STREAMOPTIONS", "Options", tmp, tmp);
        sprintf_s(tmp, "%u", p.raw);
        w.push(2, wa::hook_ipc::kArgIn, "BOOL", "RAW", tmp, tmp);
        sprintf_s(tmp, "%u", p.matchFormat);
        w.push(2, wa::hook_ipc::kArgIn, "BOOL", "MATCH_FORMAT", tmp, tmp);
    }
    pushReturn(w, hr);
    sealArgs(p, w);
    emit(p);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookInitShared(IAudioClient3* self, DWORD flags, UINT32 period,
                                         const WAVEFORMATEX* fmt, LPCGUID session) {
    const HRESULT hr =
        g_initShared ? g_initShared(self, flags, period, fmt, session) : E_UNEXPECTED;
    CallPod p{};
    p.streamId = sid(self);
    copyStr(p.iface, sizeof(p.iface), "IAudioClient3");
    copyStr(p.method, sizeof(p.method), "InitializeSharedAudioStream");
    char wave[64] = {};
    formatWave(fmt, wave, sizeof(wave));
    sprintf_s(p.args, "StreamFlags=0x%lX PeriodInFrames=%u fmt=%s",
              static_cast<unsigned long>(flags), period, wave);
    p.hresult = static_cast<int32_t>(hr);
    p.hasExclusive = 1;
    p.exclusive = 0;
    p.hasFormat = 1;
    copyStr(p.format, sizeof(p.format), wave);

    wa::hook_ipc::ArgBlobWriter w;
    beginArgs(p, w);
    char tmp[96] = {};
    formatPtr(self, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "IAudioClient3*", "this", tmp, tmp);
    formatStreamFlags(flags, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "DWORD", "StreamFlags", tmp, tmp);
    sprintf_s(tmp, "%u", period);
    w.push(0, wa::hook_ipc::kArgIn, "UINT32", "PeriodInFrames", tmp, tmp);
    formatPtr(fmt, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "WAVEFORMATEX*", "pFormat", tmp, tmp);
    if (fmt) {
        w.push(1, wa::hook_ipc::kArgIn, "WAVEFORMATEX", "*pFormat", wave, wave);
        pushWaveFormat(w, 2, fmt);
    }
    formatPtr(session, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "LPCGUID", "AudioSessionGuid", tmp, tmp);
    if (session) {
        formatGuid(session, tmp, sizeof(tmp));
        w.push(1, wa::hook_ipc::kArgIn, "GUID", "*AudioSessionGuid", tmp, tmp);
    }
    pushReturn(w, hr);
    sealArgs(p, w);
    emit(p);
    return hr;
}

void patchClient(void* obj) {
    if (!obj) return;
    using wa::hook_ipc::kSlotClientInitialize;
    using wa::hook_ipc::kSlotClientStart;
    using wa::hook_ipc::kSlotClientStop;
    using wa::hook_ipc::kSlotClientReset;
    using wa::hook_ipc::kSlotClientSetEventHandle;
    using wa::hook_ipc::kSlotClientGetService;
    using wa::hook_ipc::kSlotClientSetProperties;
    using wa::hook_ipc::kSlotClientInitializeShared;
    patchSlot(obj, kSlotClientInitialize, reinterpret_cast<void*>(&HookInitialize),
              reinterpret_cast<void**>(&g_initialize));
    patchSlot(obj, 4, reinterpret_cast<void*>(&HookGetBufferSize),
              reinterpret_cast<void**>(&g_getBufSize));
    patchSlot(obj, kSlotClientStart, reinterpret_cast<void*>(&HookStart),
              reinterpret_cast<void**>(&g_start));
    patchSlot(obj, kSlotClientStop, reinterpret_cast<void*>(&HookStop),
              reinterpret_cast<void**>(&g_stop));
    patchSlot(obj, kSlotClientReset, reinterpret_cast<void*>(&HookReset),
              reinterpret_cast<void**>(&g_reset));
    patchSlot(obj, kSlotClientSetEventHandle, reinterpret_cast<void*>(&HookSetEventHandle),
              reinterpret_cast<void**>(&g_setEvent));
    patchSlot(obj, kSlotClientGetService, reinterpret_cast<void*>(&HookGetService),
              reinterpret_cast<void**>(&g_getService));
    IAudioClient2* c2 = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(obj)->QueryInterface(__uuidof(IAudioClient2),
                                                              reinterpret_cast<void**>(&c2))) &&
        c2) {
        patchSlot(c2, kSlotClientSetProperties, reinterpret_cast<void*>(&HookSetClientProperties),
                  reinterpret_cast<void**>(&g_setProps));
        c2->Release();
    }
    IAudioClient3* c3 = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(obj)->QueryInterface(__uuidof(IAudioClient3),
                                                              reinterpret_cast<void**>(&c3))) &&
        c3) {
        patchSlot(c3, kSlotClientInitializeShared, reinterpret_cast<void*>(&HookInitShared),
                  reinterpret_cast<void**>(&g_initShared));
        c3->Release();
    }
}

HRESULT STDMETHODCALLTYPE HookActivate(IMMDevice* self, REFIID iid, DWORD ctx, PROPVARIANT* params,
                                       void** ppInterface) {
    const HRESULT hr =
        g_activate ? g_activate(self, iid, ctx, params, ppInterface) : E_UNEXPECTED;
    CallPod p{};
    p.streamId = sid(self);
    copyStr(p.iface, sizeof(p.iface), "IMMDevice");
    copyStr(p.method, sizeof(p.method), "Activate");
    sprintf_s(p.args, "iid=%s", iidName(iid));
    p.hresult = static_cast<int32_t>(hr);
    wa::hook_ipc::ArgBlobWriter w;
    beginArgs(p, w);
    char tmp[96] = {};
    formatPtr(self, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "IMMDevice*", "this", tmp, tmp);
    copyStr(tmp, sizeof(tmp), iidName(iid));
    w.push(0, wa::hook_ipc::kArgIn, "REFIID", "iid", tmp, tmp);
    sprintf_s(tmp, "0x%lX", static_cast<unsigned long>(ctx));
    w.push(0, wa::hook_ipc::kArgIn, "DWORD", "dwClsCtx", tmp, tmp);
    formatPtr(params, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgIn, "PROPVARIANT*", "pActivationParams", tmp, tmp);
    formatPtr(ppInterface ? *ppInterface : nullptr, tmp, sizeof(tmp));
    w.push(0, wa::hook_ipc::kArgOut, "void**", "ppInterface", "", tmp);
    pushReturn(w, hr);
    sealArgs(p, w);
    emit(p);
    if (SUCCEEDED(hr) && ppInterface && *ppInterface) {
        if (iid == __uuidof(IAudioClient) || iid == __uuidof(IAudioClient2) ||
            iid == __uuidof(IAudioClient3)) {
            patchClient(*ppInterface);
        }
    }
    return hr;
}

void patchDevice(IMMDevice* dev) {
    if (!dev) return;
    patchSlot(dev, wa::hook_ipc::kSlotDeviceActivate, reinterpret_cast<void*>(&HookActivate),
              reinterpret_cast<void**>(&g_activate));
}

HRESULT STDMETHODCALLTYPE HookEnum(IMMDeviceEnumerator* self, EDataFlow flow, DWORD mask,
                                   IMMDeviceCollection** col) {
    const HRESULT hr = g_enum ? g_enum(self, flow, mask, col) : E_UNEXPECTED;
    if (SUCCEEDED(hr) && col && *col) {
        UINT n = 0;
        if (SUCCEEDED((*col)->GetCount(&n))) {
            for (UINT i = 0; i < n; ++i) {
                IMMDevice* d = nullptr;
                if (SUCCEEDED((*col)->Item(i, &d)) && d) {
                    patchDevice(d);
                    d->Release();
                }
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookGetDevice(IMMDeviceEnumerator* self, LPCWSTR id, IMMDevice** device) {
    const HRESULT hr = g_getDevice ? g_getDevice(self, id, device) : E_UNEXPECTED;
    if (SUCCEEDED(hr) && device && *device) patchDevice(*device);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookGetDefault(IMMDeviceEnumerator* self, EDataFlow flow, ERole role,
                                         IMMDevice** device) {
    const HRESULT hr = g_getDefault ? g_getDefault(self, flow, role, device) : E_UNEXPECTED;
    if (SUCCEEDED(hr) && device && *device) patchDevice(*device);
    return hr;
}

// Patch shared IAudioClient vtable, then briefly Initialize (no Start) so
// GetService can patch capture/render/volume/session/clock vtables too.
// Without that, attach-after on an already-pumping stream leaves Call log and
// pump metadata empty until the target calls GetService again.
void patchServicesFromClient(IAudioClient* client) {
    if (!client) return;
    void* svc = nullptr;
    if (SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient), &svc)) && svc) {
        patchService(svc, __uuidof(IAudioCaptureClient));
        static_cast<IUnknown*>(svc)->Release();
        svc = nullptr;
    }
    if (SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), &svc)) && svc) {
        patchService(svc, __uuidof(IAudioRenderClient));
        static_cast<IUnknown*>(svc)->Release();
        svc = nullptr;
    }
    if (SUCCEEDED(client->GetService(__uuidof(ISimpleAudioVolume), &svc)) && svc) {
        patchService(svc, __uuidof(ISimpleAudioVolume));
        static_cast<IUnknown*>(svc)->Release();
        svc = nullptr;
    }
    if (SUCCEEDED(client->GetService(__uuidof(IAudioSessionControl), &svc)) && svc) {
        patchService(svc, __uuidof(IAudioSessionControl));
        static_cast<IUnknown*>(svc)->Release();
        svc = nullptr;
    }
    if (SUCCEEDED(client->GetService(__uuidof(IAudioClock), &svc)) && svc) {
        patchService(svc, __uuidof(IAudioClock));
        static_cast<IUnknown*>(svc)->Release();
        svc = nullptr;
    }
    if (SUCCEEDED(client->GetService(__uuidof(IAudioStreamVolume), &svc)) && svc) {
        patchService(svc, __uuidof(IAudioStreamVolume));
        static_cast<IUnknown*>(svc)->Release();
        svc = nullptr;
    }
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
    if (SUCCEEDED(client->GetService(__uuidof(IAudioEffectsManager), &svc)) && svc) {
        patchService(svc, __uuidof(IAudioEffectsManager));
        static_cast<IUnknown*>(svc)->Release();
        svc = nullptr;
    }
#endif
}

void tryActivate(IMMDevice* dev) {
    if (!dev) return;
    patchDevice(dev);
    void* raw = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &raw)) || !raw) return;
    IAudioClient* client = static_cast<IAudioClient*>(raw);
    patchClient(client);
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
        // Shared only; no Start - avoid audible glitch while still reaching GetService.
        const HRESULT ihr =
            client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (SUCCEEDED(ihr)) patchServicesFromClient(client);
    }
    client->Release();
}

void patchAllDevices(IMMDeviceEnumerator* enumer, EDataFlow flow) {
    if (!enumer) return;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(enumer->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) || !col) return;
    UINT n = 0;
    if (SUCCEEDED(col->GetCount(&n))) {
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (SUCCEEDED(col->Item(i, &d)) && d) {
                patchDevice(d);
                d->Release();
            }
        }
    }
    col->Release();
}

bool install() {
    InterlockedExchange(&g_quiet, 1);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = (hr == S_OK);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        InterlockedExchange(&g_quiet, 0);
        return false;
    }

    bool ok = false;
    IMMDeviceEnumerator* enumer = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumer));
    if (SUCCEEDED(hr) && enumer) {
        const bool patchedEnum =
            patchSlot(enumer, wa::hook_ipc::kSlotEnumeratorEnumEndpoints,
                      reinterpret_cast<void*>(&HookEnum), reinterpret_cast<void**>(&g_enum)) &&
            patchSlot(enumer, wa::hook_ipc::kSlotEnumeratorGetDefault,
                      reinterpret_cast<void*>(&HookGetDefault),
                      reinterpret_cast<void**>(&g_getDefault)) &&
            patchSlot(enumer, wa::hook_ipc::kSlotEnumeratorGetDevice,
                      reinterpret_cast<void*>(&HookGetDevice),
                      reinterpret_cast<void**>(&g_getDevice));
        patchAllDevices(enumer, eCapture);
        patchAllDevices(enumer, eRender);

        const bool capture = !g_ring || g_ring->flow == 0;
        IMMDevice* chosen = nullptr;
        if (g_ring && g_ring->deviceId[0])
            enumer->GetDevice(g_ring->deviceId, &chosen);
        if (!chosen) {
            enumer->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &chosen);
        }
        if (chosen) {
            tryActivate(chosen);
            chosen->Release();
        }
        enumer->Release();
        ok = patchedEnum;
    }
    if (needUninit) CoUninitialize();
    InterlockedExchange(&g_quiet, 0);
    return ok;
}

DWORD WINAPI HookThread(LPVOID) {
    g_map = wa::hook_ipc::createHookMapping(GetCurrentProcessId());
    if (!g_map) return 1;
    g_ring = static_cast<Ring*>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring)));
    if (!g_ring) {
        CloseHandle(g_map);
        g_map = nullptr;
        return 1;
    }
    if (g_ring->magic != kMagic) {
        ZeroMemory(g_ring, sizeof(Ring));
        g_ring->magic = kMagic;
        g_ring->cap = kCap;
        g_ring->writeIndex = 0;
        g_ring->installed = kInstallPending;
    }
    const bool ok = install();
    g_ring->installed = ok ? kInstallOk : kInstallFailed;
    if (ok) g_ring->captureEnabled = 1;
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        const HANDLE th = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
        if (th) CloseHandle(th);
    }
    return TRUE;
}
