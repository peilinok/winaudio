#pragma once
#include <cstdint>
#include <cstdio>

namespace wa {
namespace hook_ipc {

constexpr uint32_t kMagic = 0x5741484Bu;
constexpr uint32_t kCap = 2048;

struct CallPod {
    uint32_t streamId;
    int64_t timeMs;
    char iface[40];
    char method[40];
    char args[192];
    int32_t hresult;
    uint8_t pump;
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
    CallPod slots[kCap];
};

inline void mapName(uint32_t pid, wchar_t* out, size_t n) {
    swprintf_s(out, n, L"Local\\WinAudioHook-%u", pid);
}

}  // namespace hook_ipc
}  // namespace wa
