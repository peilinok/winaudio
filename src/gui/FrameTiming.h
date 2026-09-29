#pragma once

// Temporary instrument for "Where does one GUI frame go on the agreed scene?".
// Delete this header and every wa::frame_timing use in AppUi.cpp and main.cpp
// to remove it. Nothing here runs unless WINAUDIO_FRAME_TIMING=1.
//
// That mode shows the Render page, creates one 8-channel Render Track, starts
// playback on every channel, scrolls the track list so the spectrogram block
// starts at the top of the child, and after a warmup writes frame-timing.txt
// next to the exe and asks the loop to quit. Wall time is NewFrame through
// Present. Present is its own sample: on this swap chain that wait is the
// vertical blank, not draw CPU. PlotHeatmap is counted only when ImPlot
// submits it; a plot that fails the clip test does not add a call.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wa::frame_timing {

inline constexpr int kWarmupFrames = 45;
inline constexpr int kSampleFrames = 180;
inline constexpr int kGiveUpFrames = 1500;

struct Frame {
    double logDrainMs = 0;
    double monitorPollMs = 0;
    double renderPollMs = 0;
    double refreshChartsMs = 0;
    double waveformMs = 0;
    double heatmapMs = 0;
    double logWidgetsMs = 0;
    double imguiRenderMs = 0;
    double presentMs = 0;
    int heatmapCalls = 0;
    bool scene = false;
};

inline bool enabled() {
    static const bool on = [] {
        char buf[8]{};
        const DWORD n = GetEnvironmentVariableA("WINAUDIO_FRAME_TIMING", buf, sizeof(buf));
        return n > 0 && buf[0] == '1';
    }();
    return on;
}

inline double elapsedMs(LARGE_INTEGER start, LARGE_INTEGER now) {
    static const double ticksToMs = [] {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        return 1000.0 / static_cast<double>(freq.QuadPart);
    }();
    return static_cast<double>(now.QuadPart - start.QuadPart) * ticksToMs;
}

struct Section {
    double* slot = nullptr;
    LARGE_INTEGER start{};

    explicit Section(double& dest) {
        if (!enabled()) return;
        slot = &dest;
        QueryPerformanceCounter(&start);
    }
    ~Section() {
        if (slot == nullptr) return;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        *slot += elapsedMs(start, now);
    }

    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;
};

struct State {
    Frame current{};
    LARGE_INTEGER stamp{};
    int seenFrames = 0;
    int warmup = 0;
    std::vector<double> wall;
    std::vector<double> refresh;
    std::vector<double> waveform;
    std::vector<double> heatmap;
    std::vector<double> present;
    double logDrain = 0;
    double monitorPoll = 0;
    double renderPoll = 0;
    double refreshSum = 0;
    double waveformSum = 0;
    double heatmapSum = 0;
    double logWidgets = 0;
    double imguiRender = 0;
    double presentSum = 0;
    double wallSum = 0;
    int heatmapCalls = 0;
    std::string why;
    bool wrote = false;
    bool quit = false;
    int lastHeatmap = -1;
    int lastPlaying = -1;
    int lastCharts = -1;
    float specOrigin = -1.f;
};

inline State& state() {
    static State s;
    return s;
}

inline Frame& frame() { return state().current; }

inline void fail(std::string message) {
    State& s = state();
    if (s.why.empty()) s.why = std::move(message);
    s.quit = true;
}

inline void noteScene(bool on, int heatmap, int playing, int charts) {
    if (!enabled()) return;
    State& s = state();
    s.current.scene = on;
    s.lastHeatmap = heatmap;
    s.lastPlaying = playing;
    s.lastCharts = charts;
}

// Content Y of the first 8-channel spectrogram inside the track-list child.
// The timing run scrolls that child here so PlotHeatmap is actually submitted.
inline void noteSpecOrigin(float contentY) {
    if (!enabled()) return;
    state().specOrigin = contentY;
}

inline float specOrigin() { return state().specOrigin; }

inline void beginFrame() {
    if (!enabled() || state().wrote) return;
    State& s = state();
    s.current = {};
    QueryPerformanceCounter(&s.stamp);
    ++s.seenFrames;
    if (s.seenFrames == 1) {
        s.wall.reserve(kSampleFrames);
        s.refresh.reserve(kSampleFrames);
        s.waveform.reserve(kSampleFrames);
        s.heatmap.reserve(kSampleFrames);
        s.present.reserve(kSampleFrames);
    }
}

inline double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const size_t idx = static_cast<size_t>((values.size() - 1) * p);
    return values[idx];
}

inline int countAbove(const std::vector<double>& values, double limit) {
    int n = 0;
    for (double v : values)
        if (v > limit) ++n;
    return n;
}

inline std::string reportText() {
    const State& s = state();
    const double n = s.wall.empty() ? 1.0 : static_cast<double>(s.wall.size());
    const double wallMean = s.wallSum / n;
    const double slices = (s.logDrain + s.monitorPoll + s.renderPoll + s.refreshSum +
                           s.waveformSum + s.heatmapSum + s.logWidgets + s.imguiRender +
                           s.presentSum) /
                          n;
    char buf[3072];
    std::snprintf(
        buf, sizeof(buf),
        "scene=Render page, one 8-channel track, playback started, spectrogram block scrolled into view\n"
        "frames=%d\n"
        "warmup_skipped=%d\n"
        "failure=%s\n"
        "last_heatmap=%d\n"
        "last_playing=%d\n"
        "last_charts=%d\n"
        "wall_mean_ms=%.3f\n"
        "wall_p95_ms=%.3f\n"
        "wall_max_ms=%.3f\n"
        "wall_over_25ms=%d\n"
        "wall_over_40ms=%d\n"
        "log_drain_mean_ms=%.3f\n"
        "monitor_poll_mean_ms=%.3f\n"
        "render_poll_mean_ms=%.3f\n"
        "refresh_charts_mean_ms=%.3f\n"
        "refresh_charts_p95_ms=%.3f\n"
        "waveform_mean_ms=%.3f\n"
        "waveform_p95_ms=%.3f\n"
        "plot_heatmap_mean_ms=%.3f\n"
        "plot_heatmap_p95_ms=%.3f\n"
        "log_widgets_mean_ms=%.3f\n"
        "imgui_render_mean_ms=%.3f\n"
        "present_mean_ms=%.3f\n"
        "present_p95_ms=%.3f\n"
        "remainder_mean_ms=%.3f\n"
        "heatmap_calls_mean=%.2f\n"
        "note=wall is NewFrame through Present return. Present waits for the vertical blank. "
        "A wall above about 25 ms missed a refresh. Remainder is NewFrame, channel buttons, "
        "plot chrome, and the D3D11 draw. The track list is scrolled so the spectrogram block "
        "starts at the top of the child. heatmap_calls_mean counts PlotHeatmap submissions; "
        "ImPlot skips a plot that misses the clip rect, so the mean can be under 8.\n",
        static_cast<int>(s.wall.size()), s.warmup, s.why.empty() ? "none" : s.why.c_str(),
        s.lastHeatmap, s.lastPlaying, s.lastCharts,
        wallMean, percentile(s.wall, 0.95),
        s.wall.empty() ? 0.0 : *std::max_element(s.wall.begin(), s.wall.end()),
        countAbove(s.wall, 25.0), countAbove(s.wall, 40.0), s.logDrain / n, s.monitorPoll / n,
        s.renderPoll / n, s.refreshSum / n, percentile(s.refresh, 0.95), s.waveformSum / n,
        percentile(s.waveform, 0.95), s.heatmapSum / n, percentile(s.heatmap, 0.95),
        s.logWidgets / n, s.imguiRender / n, s.presentSum / n, percentile(s.present, 0.95),
        wallMean - slices, s.heatmapCalls / n);
    return buf;
}

inline void writeReport() {
    State& s = state();
    if (s.wrote) return;
    s.wrote = true;
    wchar_t exe[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = L"frame-timing.txt";
    if (n > 0 && n < MAX_PATH) {
        path.assign(exe, n);
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) path.resize(slash + 1);
        else path.clear();
        path += L"frame-timing.txt";
    }
    const std::string text = reportText();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f != nullptr) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

inline bool endFrame() {
    if (!enabled()) return false;
    State& s = state();
    if (s.wrote) return s.quit;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double wall = elapsedMs(s.stamp, now);
    if (!s.why.empty()) {
        writeReport();
        s.quit = true;
        return true;
    }
    if (s.current.scene) {
        if (s.warmup < kWarmupFrames) {
            ++s.warmup;
        } else {
            const Frame& f = s.current;
            s.wall.push_back(wall);
            s.refresh.push_back(f.refreshChartsMs);
            s.waveform.push_back(f.waveformMs);
            s.heatmap.push_back(f.heatmapMs);
            s.present.push_back(f.presentMs);
            s.logDrain += f.logDrainMs;
            s.monitorPoll += f.monitorPollMs;
            s.renderPoll += f.renderPollMs;
            s.refreshSum += f.refreshChartsMs;
            s.waveformSum += f.waveformMs;
            s.heatmapSum += f.heatmapMs;
            s.logWidgets += f.logWidgetsMs;
            s.imguiRender += f.imguiRenderMs;
            s.presentSum += f.presentMs;
            s.wallSum += wall;
            s.heatmapCalls += f.heatmapCalls;
            if (static_cast<int>(s.wall.size()) >= kSampleFrames) {
                writeReport();
                s.quit = true;
                return true;
            }
        }
    } else if (s.seenFrames >= kGiveUpFrames) {
        fail("scene never drew a spectrogram on the playing 8-channel Render Track");
        writeReport();
        return true;
    }
    return false;
}

inline bool shouldQuit() { return enabled() && state().quit; }

// Counts a loop turn that never reached Present, such as an occluded swap chain.
// Returns true when the run should stop.
inline bool noteSkippedFrame() {
    if (!enabled() || state().wrote) return state().quit;
    State& s = state();
    ++s.seenFrames;
    if (s.seenFrames < kGiveUpFrames) return false;
    if (s.why.empty()) s.why = "present stayed occluded or the scene never ran";
    writeReport();
    s.quit = true;
    return true;
}

}  // namespace wa::frame_timing
