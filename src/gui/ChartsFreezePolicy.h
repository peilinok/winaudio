#pragma once

// Pure charts-freeze policy for GUI visualization (no ImGui / no engine).
// Freeze stops chart data refresh only; audio capture/render continue elsewhere.
namespace wa::charts_freeze {

// Scope snapshots + spectrogram analysis advance only when the session is running
// and the page charts are not frozen.
inline bool shouldRefreshCharts(bool overallRunning, bool frozen) noexcept {
    return overallRunning && !frozen;
}

// Pause/Resume control is interactive only while overall is Running.
inline bool isControlEnabled(bool overallRunning) noexcept {
    return overallRunning;
}

// Freeze must be cleared when the session is not running (Stop, error, idle).
inline bool shouldClearFreeze(bool overallRunning) noexcept {
    return !overallRunning;
}

// Returns the freeze flag after applying lifecycle rules for this frame/state.
inline bool applyLifecycle(bool overallRunning, bool frozen) noexcept {
    if (shouldClearFreeze(overallRunning)) return false;
    return frozen;
}

// When render leaves Running, wipe render-side chart buffers only if not frozen
// (frozen pages keep the last render snapshot for inspection).
inline bool shouldResetRenderVisuals(bool frozen) noexcept {
    return !frozen;
}

} // namespace wa::charts_freeze
