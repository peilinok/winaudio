#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include <cstdio>

namespace wa {
namespace hook_ipc {

constexpr uint32_t kMagic = 0x5741484Bu;
constexpr uint32_t kCap = 2048;
constexpr uint32_t kPumpCap = 64;

constexpr uint32_t kInstallPending = 0;
constexpr uint32_t kInstallOk = 1;
constexpr uint32_t kInstallFailed = 2;
constexpr uint32_t kRingLayout = 2;

// IUnknown occupies slots 0-2. These match the Windows SDK vtables.
constexpr int kSlotDeviceActivate = 3;
constexpr int kSlotEnumeratorEnumEndpoints = 3;
constexpr int kSlotEnumeratorGetDefault = 4;
constexpr int kSlotEnumeratorGetDevice = 5;
constexpr int kSlotClientInitialize = 3;
constexpr int kSlotClientStart = 10;
constexpr int kSlotClientStop = 11;
constexpr int kSlotClientReset = 12;
constexpr int kSlotClientSetEventHandle = 13;
constexpr int kSlotClientGetService = 14;
constexpr int kSlotClientSetProperties = 16;
constexpr int kSlotClientInitializeShared = 20;
constexpr int kSlotBufferGetBuffer = 3;
constexpr int kSlotBufferReleaseBuffer = 4;

inline bool patchVtableSlot(void* obj, int slot, void* hook, void** orig) {
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

struct CallPod {
    uint32_t streamId;
    int64_t timeMs;
    char iface[40];
    char method[40];
    char args[192];
    int32_t hresult;
    uint8_t pump;
    uint8_t xrun;
    uint8_t hasCategory;
    uint8_t hasRaw;
    uint8_t hasMatchFormat;
    uint8_t hasExclusive;
    uint8_t hasFormat;
    uint8_t raw;
    uint8_t matchFormat;
    uint8_t exclusive;
    char category[32];
    char format[64];
};

struct Ring {
    uint32_t magic;
    uint32_t writeIndex;
    uint32_t cap;
    uint32_t dropped;
    uint32_t pumpEnabled;
    uint32_t pumpWriteIndex;
    uint32_t pumpXruns;
    uint32_t installed;
    uint32_t flow;  // 0 = capture, 1 = render
    wchar_t deviceId[256];
    CallPod slots[kCap];
    CallPod pumpSlots[kPumpCap];
};

inline void mapName(uint32_t pid, wchar_t* out, size_t n) {
    swprintf_s(out, n, L"Local\\WinAudioHook-%u-%u", kRingLayout, pid);
}

// Everyone + Low mandatory label so an elevated GUI mapping is visible to a
// medium-IL target (and the reverse). CreateFileMapping copies the SD.
inline HANDLE createHookMapping(uint32_t pid, bool* sdApplied = nullptr) {
    wchar_t name[64] = {};
    mapName(pid, name, 64);
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    const bool converted = ConvertStringSecurityDescriptorToSecurityDescriptorW(
                               L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr) !=
                           FALSE;
    if (sdApplied) *sdApplied = converted;
    if (converted) sa.lpSecurityDescriptor = sd;
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, converted ? &sa : nullptr, PAGE_READWRITE, 0,
                                  static_cast<DWORD>(sizeof(Ring)), name);
    const DWORD err = GetLastError();
    if (sd) LocalFree(sd);
    SetLastError(err);
    return h;
}

}  // namespace hook_ipc
}  // namespace wa
