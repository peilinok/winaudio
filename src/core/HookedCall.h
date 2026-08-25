#pragma once
#include "PipelineGraph.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wa {

bool isPumpMethod(const std::string& method);

std::string sanitizeHookedArgs(std::string args);

inline constexpr size_t kDefaultPumpRingCap = 64;

struct CallLogView {
    std::vector<HookedCall> entries;
    uint32_t pumpXruns = 0;
};

// Control-path records are kept in full. Pump GetBuffer/ReleaseBuffer are
// omitted unless pumpEnabled; when on, only the last pumpCap pump rows remain.
// xrun counts include dropped pump records. Args never keep a pcm= payload.
CallLogView shapeCallLog(const std::vector<HookedCall>& in, bool pumpEnabled,
                         size_t pumpCap = kDefaultPumpRingCap);

EtwInitializeHint extractHookedInitialize(const std::vector<HookedCall>& calls);

// Hooked fields replace matching ETW fields when present.
EtwInitializeHint mergeInitializeHint(const EtwInitializeHint& etw,
                                      const EtwInitializeHint& hooked);

}  // namespace wa
