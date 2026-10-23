#pragma once
#include <algorithm>

namespace wa::pipeline_split {

// Default fractions (match the pre-draggable Pipeline layout).
inline constexpr float kDefaultSessionsFrac = 0.68f;
inline constexpr float kDefaultCallTableFrac = 0.40f;
inline constexpr float kDefaultTopRowFrac = 0.52f;

inline constexpr float kSplitterPx = 6.0f;

// Apply a mouse delta along an axis of length `spanPx` to a fraction in [lo, hi].
inline float nudgeFrac(float frac, float deltaPx, float spanPx, float lo, float hi) {
    if (!(spanPx > 1.0f)) return std::clamp(frac, lo, hi);
    return std::clamp(frac + deltaPx / spanPx, lo, hi);
}

// Width/height for the first pane given available span (after splitter thickness removed).
inline float firstPaneSize(float usableSpan, float frac, float minFirst, float minSecond) {
    if (!(usableSpan > 0.0f)) return 0.0f;
    const float lo = (std::min)(minFirst, usableSpan * 0.5f);
    const float hi = (std::max)(lo, usableSpan - minSecond);
    return std::clamp(usableSpan * frac, lo, hi);
}

}  // namespace wa::pipeline_split
