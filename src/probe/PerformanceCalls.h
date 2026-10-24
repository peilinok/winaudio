#pragma once
#include "HookedCall.h"
#include <cstddef>
#include <vector>

namespace wa {
// Deterministic structured input for the opt-in Inspector fixture.
std::vector<HookedCall> performanceCalls(size_t count, unsigned pidCount);
}
