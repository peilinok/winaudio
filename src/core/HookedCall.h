#pragma once
#include "PipelineGraph.h"
#include <string>
#include <vector>

namespace wa {

bool isPumpMethod(const std::string& method);

std::string sanitizeHookedArgs(std::string args);

// Control-path records are kept. Pump GetBuffer/ReleaseBuffer are omitted
// unless pumpEnabled (later ticket). Args never keep a pcm= payload.
std::vector<HookedCall> shapeCallLog(const std::vector<HookedCall>& in, bool pumpEnabled);

EtwInitializeHint extractHookedInitialize(const std::vector<HookedCall>& calls);

// Hooked fields replace matching ETW fields when present.
EtwInitializeHint mergeInitializeHint(const EtwInitializeHint& etw,
                                      const EtwInitializeHint& hooked);

}  // namespace wa
