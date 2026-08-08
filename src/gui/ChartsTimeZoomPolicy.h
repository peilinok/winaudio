#pragma once

#include <algorithm>
#include <cmath>

// Pure shared-time-axis zoom policy for GUI charts (no ImGui / no engine).
// Zoom mutates only the linked X window; Y defaults are restored by the draw path.
namespace wa::charts_time_zoom {

struct Interval {
    double x0 = 0.0;
    double x1 = 0.0;
};

// Absolute floor for the visible time window (~10 ms).
inline constexpr double kMinWidthSecondsFloor = 0.010;

// Epsilon for enable/disable comparisons against full history / min width.
inline constexpr double kWidthEps = 1e-9;

// Minimum window width: max(10 ms, hopSeconds), capped by history H.
inline double minWidthSeconds(double hopSeconds, double historyH) noexcept {
    if (!(historyH > 0.0)) return 0.0;
    double w = hopSeconds;
    if (w < kMinWidthSecondsFloor) w = kMinWidthSecondsFloor;
    if (w > historyH) w = historyH;
    if (w < 0.0) w = 0.0;
    return w;
}

// Full buffered history on the shared time axis.
inline Interval fullHistory(double historyH) noexcept {
    if (!(historyH > 0.0)) return Interval{0.0, 0.0};
    return Interval{0.0, historyH};
}

// Clamp [x0, x1] into [0, H], preserving width when possible.
inline Interval clampToHistory(double x0, double x1, double historyH) noexcept {
    if (!(historyH > 0.0)) return Interval{0.0, 0.0};
    if (x1 < x0) std::swap(x0, x1);
    double w = x1 - x0;
    if (w > historyH) w = historyH;
    if (w < 0.0) w = 0.0;
    if (x0 < 0.0) {
        x1 -= x0;
        x0 = 0.0;
    }
    if (x1 > historyH) {
        x0 -= (x1 - historyH);
        x1 = historyH;
    }
    if (x0 < 0.0) x0 = 0.0;
    // Re-assert width after edge clamp (numerical safety).
    if (x1 - x0 > historyH) x1 = x0 + historyH;
    if (x1 > historyH) {
        x1 = historyH;
        x0 = historyH - w;
        if (x0 < 0.0) x0 = 0.0;
    }
    return Interval{x0, x1};
}

// True when the window can still shrink (zoom in).
inline bool canZoomIn(double x0, double x1, double historyH, double hopSeconds) noexcept {
    if (!(historyH > 0.0)) return false;
    const double w = x1 - x0;
    const double minW = minWidthSeconds(hopSeconds, historyH);
    return w > minW + kWidthEps;
}

// True when the window is not yet the full history (zoom out).
inline bool canZoomOut(double x0, double x1, double historyH) noexcept {
    if (!(historyH > 0.0)) return false;
    const double w = x1 - x0;
    return w + kWidthEps < historyH;
}

// Centered zoom: new width = current width * widthFactor (0.5 = in, 2.0 = out),
// then clamp into [0, H] and enforce min width.
inline Interval zoomCentered(double x0, double x1, double historyH, double hopSeconds,
                             double widthFactor) noexcept {
    if (!(historyH > 0.0) || !(widthFactor > 0.0)) return fullHistory(historyH);
    if (x1 < x0) std::swap(x0, x1);
    double w = x1 - x0;
    if (!(w > 0.0)) w = historyH;
    const double minW = minWidthSeconds(hopSeconds, historyH);
    double newW = w * widthFactor;
    if (newW < minW) newW = minW;
    if (newW > historyH) newW = historyH;
    const double center = 0.5 * (x0 + x1);
    return clampToHistory(center - 0.5 * newW, center + 0.5 * newW, historyH);
}

} // namespace wa::charts_time_zoom
