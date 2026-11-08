#pragma once
#include <cstdint>

namespace wa {

// GUI-owned history in chronological order, without linearizing its ring storage.
struct WaveHistoryView {
    const float* data = nullptr;
    int n = 0;
    int first = 0;
    // Cumulative physical writes. UINT64_MAX denotes an untracked linear buffer.
    uint64_t updatedSamples = UINT64_MAX;

    float operator[](int index) const {
        const int physical = first + index;
        return data[physical < n ? physical : physical - n];
    }
};

} // namespace wa
