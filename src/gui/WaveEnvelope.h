#pragma once
#include "WaveHistoryView.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace wa {

// Waveform dB display floor (center line reads -inf). Matches AppUi kWaveDbFloor.
constexpr float kWaveEnvDbFloor = -60.0f;

// Block size for the min/max pyramid (samples). Zoomed-out columns with
// samples-per-pixel >= kWaveEnvBlock use the pyramid; boundary snap is at most
// one block (~sub-pixel when spp is large).
constexpr int kWaveEnvBlock = 64;

// Map a linear sample onto the symmetric dB display scale. Monotonic, so
// min/max envelopes commute with the warp.
inline float waveDbWarp(float x) {
    const float ax = x < 0.f ? -x : x;
    if (ax <= 1e-6f) return 0.f;
    const float db = 20.0f * std::log10(ax);
    if (db <= kWaveEnvDbFloor) return 0.f;
    const float p = 1.0f - db / kWaveEnvDbFloor;  // 0 dB -> 1, floor -> 0
    return (x < 0.f) ? -p : p;
}

// Physical 64-sample blocks stay in place when the history ring advances.
struct MinMaxPyramid {
    uint64_t epoch = UINT64_MAX;  // invalid until first build
    uint64_t generation = UINT64_MAX;
    int      n     = 0;           // sample count at build
    uint64_t updatedSamples = UINT64_MAX;
    std::vector<float> blockMin;
    std::vector<float> blockMax;
    // Monotonic dB warp commutes with min/max; reuse it for contiguous queries.
    std::vector<float> blockMinWarped;
    std::vector<float> blockMaxWarped;
};

struct WaveEnvelopeWork {
    uint64_t rebuildSamples = 0;
    uint64_t updateSamples = 0;
    uint64_t blockQueries = 0;
    uint64_t exactSamples = 0;
    uint64_t columns = 0;
};

// Refresh physical blocks. Tracked rings can update only overwritten blocks.
void ensureMinMaxPyramid(MinMaxPyramid& pyr, WaveHistoryView wave, uint64_t epoch,
                         WaveEnvelopeWork* work = nullptr, uint64_t generation = 0);

// Exact per-column min/max over wave[iLo..iHi] (inclusive), then waveDbWarp.
// envX/envMin/envMax must hold cols entries. Returns false if nothing filled.
bool fillWaveEnvelopeExact(WaveHistoryView wave, int iLo, int iHi, int cols, double invSr,
                           float* envX, float* envMin, float* envMax,
                           WaveEnvelopeWork* work = nullptr);

// Fill envelope columns. When pyr is non-null and samples-per-pixel is large enough,
// builds/reuses a block min/max pyramid (avoids a full raw scan on later frames with
// the same epoch, e.g. chart freeze + pan/zoom). Otherwise exact raw scan.
bool fillWaveEnvelope(WaveHistoryView wave, int iLo, int iHi, int cols, double invSr,
                      MinMaxPyramid* pyr, uint64_t epoch, float* envX, float* envMin,
                      float* envMax, WaveEnvelopeWork* work = nullptr, uint64_t generation = 0);

}  // namespace wa
