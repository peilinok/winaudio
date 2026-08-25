#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <objbase.h>
#include <cstring>
#include <cstdint>
#include "HookedCallPod.h"

using wa::hook_ipc::CallPod;
using wa::hook_ipc::Ring;
using wa::hook_ipc::kCap;
using wa::hook_ipc::kMagic;

namespace {

Ring* g_ring = nullptr;
HANDLE g_map = nullptr;
volatile LONG g_quiet = 0;

using ActivateFn = HRESULT(STDMETHODCALLTYPE*)(IMMDevice*, REFIID, DWORD, PROPVARIANT*, void**);
using GetDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, LPCWSTR, IMMDevice**);
using GetDefaultFn = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, EDataFlow, ERole, IMMDevice**);
using InitializeFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, AUDCLNT_SHAREMODE, DWORD,
                                                 REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*,
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

ActivateFn g_activate = nullptr;
GetDeviceFn g_getDevice = nullptr;
GetDefaultFn g_getDefault = nullptr;
InitializeFn g_initialize = nullptr;
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
    if (!g_ring || g_quiet) return;
    pod.timeMs = nowMs();
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

bool patchSlot(void* obj, int slot, void* hook, void** orig) {
    if (!obj || !hook || !orig) return false;
    void** vt = *reinterpret_cast<void***>(obj);
    void** cell = vt + slot;
    if (*orig == nullptr) *orig = *cell;
    if (*cell == hook) return true;
    DWORD old = 0;
    if (!VirtualProtect(cell, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *cell = hook;
    DWORD tmp = 0;
    VirtualProtect(cell, sizeof(void*), old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), cell, sizeof(void*));
    return true;
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
    sprintf_s(p.args, "share=%s flags=0x%lX fmt=%s",
              share == AUDCLNT_SHAREMODE_EXCLUSIVE ? "exclusive" : "shared",
              static_cast<unsigned long>(flags), wave);
    p.hresult = static_cast<int32_t>(hr);
    p.hasExclusive = 1;
    p.exclusive = share == AUDCLNT_SHAREMODE_EXCLUSIVE ? 1 : 0;
    p.hasFormat = 1;
    copyStr(p.format, sizeof(p.format), wave);
    emit(p);
    (void)per;
    (void)dur;
    (void)session;
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
    emitSimple(sid(self), "IAudioClient", "SetEventHandle", "", hr);
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

void patchService(void* obj, REFIID iid) {
    if (!obj) return;
    if (iid == __uuidof(ISimpleAudioVolume)) {
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
    }
}

HRESULT STDMETHODCALLTYPE HookGetService(IAudioClient* self, REFIID iid, void** pp) {
    const HRESULT hr = g_getService ? g_getService(self, iid, pp) : E_UNEXPECTED;
    char args[80] = {};
    sprintf_s(args, "iid=%s", iidName(iid));
    emitSimple(sid(self), "IAudioClient", "GetService", args, hr);
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
    if (props) {
        p.hasCategory = 1;
        copyStr(p.category, sizeof(p.category), categoryName(static_cast<int>(props->eCategory)));
        p.hasRaw = 1;
        p.raw = (props->Options & AUDCLNT_STREAMOPTIONS_RAW) ? 1 : 0;
        p.hasMatchFormat = 1;
        p.matchFormat = (props->Options & AUDCLNT_STREAMOPTIONS_MATCH_FORMAT) ? 1 : 0;
        sprintf_s(p.args, "category=%s raw=%u matchformat=%u", p.category, p.raw, p.matchFormat);
    }
    emit(p);
    return hr;
}

void patchClient(void* obj) {
    if (!obj) return;
    patchSlot(obj, 3, reinterpret_cast<void*>(&HookInitialize), reinterpret_cast<void**>(&g_initialize));
    patchSlot(obj, 10, reinterpret_cast<void*>(&HookStart), reinterpret_cast<void**>(&g_start));
    patchSlot(obj, 11, reinterpret_cast<void*>(&HookStop), reinterpret_cast<void**>(&g_stop));
    patchSlot(obj, 12, reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&g_reset));
    patchSlot(obj, 13, reinterpret_cast<void*>(&HookSetEventHandle),
              reinterpret_cast<void**>(&g_setEvent));
    patchSlot(obj, 14, reinterpret_cast<void*>(&HookGetService),
              reinterpret_cast<void**>(&g_getService));
    IAudioClient2* c2 = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(obj)->QueryInterface(__uuidof(IAudioClient2),
                                                              reinterpret_cast<void**>(&c2))) &&
        c2) {
        patchSlot(c2, 16, reinterpret_cast<void*>(&HookSetClientProperties),
                  reinterpret_cast<void**>(&g_setProps));
        c2->Release();
    }
}

HRESULT STDMETHODCALLTYPE HookActivate(IMMDevice* self, REFIID iid, DWORD ctx, PROPVARIANT* params,
                                       void** ppInterface) {
    const HRESULT hr =
        g_activate ? g_activate(self, iid, ctx, params, ppInterface) : E_UNEXPECTED;
    char args[80] = {};
    sprintf_s(args, "iid=%s", iidName(iid));
    emitSimple(sid(self), "IMMDevice", "Activate", args, hr);
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
    patchSlot(dev, 3, reinterpret_cast<void*>(&HookActivate), reinterpret_cast<void**>(&g_activate));
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

void tryActivate(IMMDevice* dev, REFIID iid) {
    if (!dev) return;
    patchDevice(dev);
    void* client = nullptr;
    HRESULT hr = dev->Activate(iid, CLSCTX_ALL, nullptr, &client);
    if (SUCCEEDED(hr) && client) {
        patchClient(client);
        static_cast<IUnknown*>(client)->Release();
    }
    (void)hr;
}

void install() {
    InterlockedExchange(&g_quiet, 1);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = (hr == S_OK);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return;

    IMMDeviceEnumerator* enumer = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumer));
    if (SUCCEEDED(hr) && enumer) {
        patchSlot(enumer, 4, reinterpret_cast<void*>(&HookGetDefault),
                  reinterpret_cast<void**>(&g_getDefault));
        patchSlot(enumer, 5, reinterpret_cast<void*>(&HookGetDevice),
                  reinterpret_cast<void**>(&g_getDevice));
        IMMDevice* cap = nullptr;
        IMMDevice* ren = nullptr;
        enumer->GetDefaultAudioEndpoint(eCapture, eConsole, &cap);
        enumer->GetDefaultAudioEndpoint(eRender, eConsole, &ren);
        if (cap) {
            tryActivate(cap, __uuidof(IAudioClient));
            tryActivate(cap, __uuidof(IAudioClient2));
            cap->Release();
        }
        if (ren) {
            tryActivate(ren, __uuidof(IAudioClient));
            tryActivate(ren, __uuidof(IAudioClient2));
            ren->Release();
        }
        enumer->Release();
    }
    if (needUninit) CoUninitialize();
    InterlockedExchange(&g_quiet, 0);
}

DWORD WINAPI HookThread(LPVOID) {
    wchar_t name[64] = {};
    wa::hook_ipc::mapName(GetCurrentProcessId(), name, 64);
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                               sizeof(Ring), name);
    if (!g_map) return 1;
    g_ring = static_cast<Ring*>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring)));
    if (!g_ring) return 1;
    if (g_ring->magic != kMagic) {
        ZeroMemory(g_ring, sizeof(Ring));
        g_ring->magic = kMagic;
        g_ring->cap = kCap;
        g_ring->writeIndex = 0;
    }
    install();
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
