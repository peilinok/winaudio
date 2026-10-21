#pragma once
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

// Persistent min/max pyramid for one wave history buffer. Rebuild when epoch/n change.
struct MinMaxPyramid {
    uint64_t epoch = UINT64_MAX;  // invalid until first build
    int      n     = 0;           // sample count at build
    std::vector<float> blockMin;
    std::vector<float> blockMax;
};

// Build / refresh a block min/max pyramid over wave[0, n). No-op when epoch and n match.
void ensureMinMaxPyramid(MinMaxPyramid& pyr, const float* wave, int n, uint64_t epoch);

// Exact per-column min/max over wave[iLo..iHi] (inclusive), then waveDbWarp.
// envX/envMin/envMax must hold cols entries. Returns false if nothing filled.
bool fillWaveEnvelopeExact(const float* wave, int n, int iLo, int iHi, int cols, double invSr,
                           float* envX, float* envMin, float* envMax);

// Fill envelope columns. When pyr is non-null and samples-per-pixel is large enough,
// builds/reuses a block min/max pyramid (avoids a full raw scan on later frames with
// the same epoch, e.g. chart freeze + pan/zoom). Otherwise exact raw scan.
bool fillWaveEnvelope(const float* wave, int n, int iLo, int iHi, int cols, double invSr,
                      MinMaxPyramid* pyr, uint64_t epoch, float* envX, float* envMin,
                      float* envMax);

}  // namespace wa