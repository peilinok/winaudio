#pragma once
#include "ScopeReader.h"
#include "Spectrogram.h"
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace wa {

// Parameters for one Chart Data Pipeline refresh (one stream side).
struct ChartRefreshParams {
    const ScopeReader* reader = nullptr;
    bool               frozen = false;       // chart freeze: no writes
    bool               streamActive = false; // e.g. capture overall running / render running
    size_t             fftWin = 2048;
    size_t             fftHop = 1024;
    size_t             maxCatchup = 8;
};

// External buffers owned by VisualState (or tests). Mono vs multi-channel:
// - Mono/downmix: set wave + spectrogram + specWin; channelCount = 0 or 1.
// - Multi-channel capture: set channelWaves / channelSpecs / channelSpecWindows; channelCount >= 2.
struct ChartBuffers {
    // Wave history (latest rolling window, progressive fill into tail).
    float*                         wave = nullptr;          // mono path, size waveN
    std::vector<std::vector<float>>* channelWaves = nullptr;  // multi-ch
    uint32_t                       channelCount = 0;        // 0/1 => mono; >=2 multi
    int                            waveN = 0;
    uint32_t                       waveSr = 0;              // >0 when wave/spec buffers ready

    // Spectrogram hop windows + FFT scratch.
    std::vector<float>*                       specWin = nullptr;           // mono, size fftWin
    std::vector<std::vector<float>>*          channelSpecWindows = nullptr; // multi
    std::vector<std::complex<float>>*         work = nullptr;
    std::vector<float>*                       mag = nullptr;
    Spectrogram*                              spectrogram = nullptr;       // mono
    std::vector<std::unique_ptr<Spectrogram>>* channelSpecs = nullptr;     // multi
    uint64_t*                                 nextEnd = nullptr;           // analysis cursor
    uint32_t                                  specSr = 0;                  // >0 when analysis ready
};

struct ChartRefreshResult {
    bool haveWave = false;
};

// Refresh chart data from a Scope Reader into external buffers. No ImGui.
// Wave: latest history window. Spectrogram: hop-aligned ending-at for all paths.
ChartRefreshResult refreshCharts(const ChartRefreshParams& params, ChartBuffers& buffers);

} // namespace wa
