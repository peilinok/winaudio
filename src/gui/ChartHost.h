#pragma once
#include <vector>

// Pure Chart Host panel-id resolution (no ImGui). AppUi::drawChartHost consumes this.
namespace wa::chart_host {

enum class Mode {
    DualReorderable, // Monitor: capture + render combos, reorderable
    CaptureOnly,     // Loopback pages: single capture combo
};

inline constexpr int kPanelCapture = 0;
inline constexpr int kPanelRender  = 1;

// Returns panel ids to draw.
// CaptureOnly: always {kPanelCapture}.
// DualReorderable: first occurrence of each valid id (0 or 1) in order, then any missing
// of {0,1} appended in ascending id order. Unknown ids ignored. Empty order -> {0,1}.
inline std::vector<int> resolvePanelIds(Mode mode, const std::vector<int>& order) {
    if (mode == Mode::CaptureOnly)
        return {kPanelCapture};

    std::vector<int> out;
    bool seen0 = false;
    bool seen1 = false;
    for (int id : order) {
        if (id == kPanelCapture && !seen0) {
            out.push_back(kPanelCapture);
            seen0 = true;
        } else if (id == kPanelRender && !seen1) {
            out.push_back(kPanelRender);
            seen1 = true;
        }
    }
    if (!seen0) out.push_back(kPanelCapture);
    if (!seen1) out.push_back(kPanelRender);
    return out;
}

} // namespace wa::chart_host
