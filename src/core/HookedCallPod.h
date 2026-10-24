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
#include <cstring>

namespace wa {
namespace hook_ipc {

constexpr uint32_t kMagic = 0x5741484Bu;
constexpr uint32_t kCap = 2048;
constexpr uint32_t kPumpCap = 64;

constexpr uint32_t kInstallPending = 0;
constexpr uint32_t kInstallOk = 1;
constexpr uint32_t kInstallFailed = 2;
// Layout 5: CallPod gains argBlob for API Monitor-style Type/Name/Pre/Post rows
// (nested WAVEFORMATEX / EXTENSIBLE fields). Mapping name embeds this version.
constexpr uint32_t kRingLayout = 5;

constexpr uint32_t kArgBlobCap = 640;
constexpr uint8_t kArgBlobVersion = 1;

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

enum CallArgDir : uint8_t {
    kArgIn = 0,
    kArgOut = 1,
    kArgInOut = 2,
    kArgRet = 3,
};

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

// Packed arg blob: [u8 ver=1][u8 count] then count records of
// [u8 depth][u8 dir][u8 tlen][type][u8 nlen][name][u8 plen][pre][u8 qlen][post].
struct ArgBlobWriter {
    uint8_t* buf = nullptr;
    uint16_t cap = 0;
    uint16_t len = 0;
    uint8_t count = 0;

    void reset(uint8_t* b, uint16_t c) {
        buf = b;
        cap = c;
        len = 0;
        count = 0;
        if (cap >= 2) {
            buf[0] = kArgBlobVersion;
            buf[1] = 0;
            len = 2;
        }
    }

    bool push(uint8_t depth, uint8_t dir, const char* type, const char* name, const char* pre,
              const char* post) {
        if (!buf || cap < 2 || len < 2) return false;
        const char* t = type ? type : "";
        const char* n = name ? name : "";
        const char* p = pre ? pre : "";
        const char* q = post ? post : p;
        const size_t tl = strnlen(t, 255);
        const size_t nl = strnlen(n, 255);
        const size_t pl = strnlen(p, 255);
        const size_t ql = strnlen(q, 255);
        const size_t need = 6 + tl + nl + pl + ql;
        if (static_cast<size_t>(len) + need > cap || count == 255) return false;
        buf[len++] = depth;
        buf[len++] = dir;
        buf[len++] = static_cast<uint8_t>(tl);
        if (tl) {
            memcpy(buf + len, t, tl);
            len = static_cast<uint16_t>(len + tl);
        }
        buf[len++] = static_cast<uint8_t>(nl);
        if (nl) {
            memcpy(buf + len, n, nl);
            len = static_cast<uint16_t>(len + nl);
        }
        buf[len++] = static_cast<uint8_t>(pl);
        if (pl) {
            memcpy(buf + len, p, pl);
            len = static_cast<uint16_t>(len + pl);
        }
        buf[len++] = static_cast<uint8_t>(ql);
        if (ql) {
            memcpy(buf + len, q, ql);
            len = static_cast<uint16_t>(len + ql);
        }
        ++count;
        buf[1] = count;
        return true;
    }
};

template <typename Fn>
inline bool decodeArgBlob(const uint8_t* data, uint16_t len, Fn&& fn) {
    if (!data || len < 2 || data[0] != kArgBlobVersion) return false;
    const uint8_t count = data[1];
    uint16_t off = 2;
    for (uint8_t i = 0; i < count; ++i) {
        if (off + 6 > len) return false;
        const uint8_t depth = data[off++];
        const uint8_t dir = data[off++];
        const uint8_t tl = data[off++];
        if (off + tl >= len) return false;
        const char* type = reinterpret_cast<const char*>(data + off);
        off = static_cast<uint16_t>(off + tl);
        const uint8_t nl = data[off++];
        if (off + nl >= len) return false;
        const char* name = reinterpret_cast<const char*>(data + off);
        off = static_cast<uint16_t>(off + nl);
        const uint8_t pl = data[off++];
        if (off + pl >= len) return false;
        const char* pre = reinterpret_cast<const char*>(data + off);
        off = static_cast<uint16_t>(off + pl);
        const uint8_t ql = data[off++];
        if (off + ql > len) return false;
        const char* post = reinterpret_cast<const char*>(data + off);
        off = static_cast<uint16_t>(off + ql);
        fn(depth, dir, type, tl, name, nl, pre, pl, post, ql);
    }
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
    uint16_t argBlobLen;
    uint8_t argBlob[kArgBlobCap];
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
    uint32_t captureEnabled;  // 0 = detach/quiet; 1 = record calls
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

