#pragma once
#include <cstdint>

namespace wa::injection_probe {
inline constexpr uint32_t kValue = 0x16C05A7u;
struct Shared {
    uintptr_t entry = 0;
    uint32_t value = 0;
    uint32_t readOk = 0;
};
}  // namespace wa::injection_probe
