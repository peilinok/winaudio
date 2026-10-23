#pragma once
// Include the production translation unit. Renaming DllMain prevents any
// automatic installation in an executable, without copying hook algorithms.
#define DllMain HookBufferUnusedDllMain
#include "../hook/WinAudioHook.cpp"
#undef DllMain

namespace hook_harness {
inline Ring ring{};
inline volatile unsigned calls = 0;
inline HRESULT result = S_OK;
inline DWORD outputFlags = 0;
inline void* seenSelf = nullptr;
inline UINT32 seenFrames = 0;
inline DWORD seenFlags = 0;
inline void (*afterOriginal)() = nullptr;
inline BYTE bytes[8]{};
inline BYTE* data = nullptr;
inline UINT32 frames = 0;
inline DWORD flags = 0;
inline UINT64 position = 0, qpc = 0;

__declspec(noinline) inline HRESULT STDMETHODCALLTYPE capGet(
    IAudioCaptureClient* self, BYTE** d, UINT32* n, DWORD* f, UINT64* p, UINT64* q) {
    ++calls;
    seenSelf = self;
    if (d) *d = bytes;
    if (n) *n = 137;
    if (f) *f = outputFlags;
    if (p) *p = 123456;
    if (q) *q = 654321;
    if (afterOriginal) afterOriginal();
    return result;
}
__declspec(noinline) inline HRESULT STDMETHODCALLTYPE capRelease(IAudioCaptureClient* self, UINT32 n) {
    ++calls;
    seenSelf = self;
    seenFrames = n;
    if (afterOriginal) afterOriginal();
    return result;
}
__declspec(noinline) inline HRESULT STDMETHODCALLTYPE renGet(IAudioRenderClient* self, UINT32 n, BYTE** d) {
    ++calls;
    seenSelf = self;
    seenFrames = n;
    if (d) *d = bytes;
    if (afterOriginal) afterOriginal();
    return result;
}
__declspec(noinline) inline HRESULT STDMETHODCALLTYPE renRelease(IAudioRenderClient* self, UINT32 n, DWORD f) {
    ++calls;
    seenSelf = self;
    seenFrames = n;
    seenFlags = f;
    if (afterOriginal) afterOriginal();
    return result;
}
inline void reset(int state = 5) {
    std::memset(&ring, 0, sizeof(ring));
    g_ring = state == 1 ? nullptr : &ring;
    InterlockedExchange(&g_quiet, state == 2 ? 1 : 0);
    ring.captureEnabled = state == 3 ? 0u : 1u;
    ring.pumpEnabled = state == 4 ? 0u : 1u;
    g_capGet = capGet;
    g_capRel = capRelease;
    g_renGet = renGet;
    g_renRel = renRelease;
    calls = 0;
    result = S_OK;
    outputFlags = 0;
    seenSelf = nullptr;
    seenFrames = 0;
    seenFlags = 0;
    afterOriginal = nullptr;
    data = nullptr;
    frames = flags = 0;
    position = qpc = 0;
}
__declspec(noinline) inline HRESULT invoke(int entry, bool direct = false) {
    auto* cap = reinterpret_cast<IAudioCaptureClient*>(static_cast<uintptr_t>(0x1234));
    auto* ren = reinterpret_cast<IAudioRenderClient*>(static_cast<uintptr_t>(0x5678));
    switch (entry) {
    case 0: return direct ? capGet(cap, &data, &frames, &flags, &position, &qpc)
                         : HookCapGetBuffer(cap, &data, &frames, &flags, &position, &qpc);
    case 1: return direct ? capRelease(cap, 139) : HookCapReleaseBuffer(cap, 139);
    case 2: return direct ? renGet(ren, 141, &data) : HookRenGetBuffer(ren, 141, &data);
    default: return direct ? renRelease(ren, 143, AUDCLNT_BUFFERFLAGS_SILENT)
                          : HookRenReleaseBuffer(ren, 143, AUDCLNT_BUFFERFLAGS_SILENT);
    }
}
}  // namespace hook_harness
