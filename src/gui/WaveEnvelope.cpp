#include "WaveEnvelope.h"
#include <algorithm>
#include <cmath>

namespace wa {
namespace {

bool clampVisibleRange(int n, int& iLo, int& iHi) {
    if (!n || iHi < iLo) return false;
    if (iLo < 0) iLo = 0;
    if (iHi > n - 1) iHi = n - 1;
    return iHi > iLo;
}

}  // namespace

void ensureMinMaxPyramid(MinMaxPyramid& pyr, const float* wave, int n, uint64_t epoch,
                         WaveEnvelopeWork* work) {
    if (!wave || n <= 0) {
        pyr.epoch = UINT64_MAX;
        pyr.n = 0;
        pyr.blockMin.clear();
        pyr.blockMax.clear();
        return;
    }
    if (pyr.epoch == epoch && pyr.n == n &&
        static_cast<int>(pyr.blockMin.size()) == (n + kWaveEnvBlock - 1) / kWaveEnvBlock)
        return;

    const int nb = (n + kWaveEnvBlock - 1) / kWaveEnvBlock;
    if (work) work->rebuildSamples += static_cast<uint64_t>(n);
    pyr.blockMin.resize(static_cast<size_t>(nb));
    pyr.blockMax.resize(static_cast<size_t>(nb));
    for (int b = 0; b < nb; ++b) {
        const int s0 = b * kWaveEnvBlock;
        const int s1 = std::min(n, s0 + kWaveEnvBlock);
        float mn = wave[s0];
        float mx = wave[s0];
        for (int s = s0 + 1; s < s1; ++s) {
            const float v = wave[s];
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        pyr.blockMin[static_cast<size_t>(b)] = mn;
        pyr.blockMax[static_cast<size_t>(b)] = mx;
    }
    pyr.n = n;
    pyr.epoch = epoch;
}

bool fillWaveEnvelopeExact(const float* wave, int n, int iLo, int iHi, int cols, double invSr,
                           float* envX, float* envMin, float* envMax, WaveEnvelopeWork* work) {
    if (!wave || !envX || !envMin || !envMax || cols <= 0 || invSr <= 0.0) return false;
    if (!clampVisibleRange(n, iLo, iHi)) return false;
    const int visN = iHi - iLo + 1;
    for (int c = 0; c < cols; ++c) {
        int s0 = iLo + static_cast<int>((static_cast<int64_t>(visN) * c) / cols);
        int s1 = iLo + static_cast<int>((static_cast<int64_t>(visN) * (c + 1)) / cols);
        if (s1 <= s0) s1 = s0 + 1;
        if (s1 > n) s1 = n;
        if (work) work->exactSamples += static_cast<uint64_t>(s1 - s0);
        float mn = wave[s0];
        float mx = wave[s0];
        for (int s = s0 + 1; s < s1; ++s) {
            const float v = wave[s];
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        envX[static_cast<size_t>(c)] = static_cast<float>(0.5 * static_cast<double>(s0 + s1) * invSr);
        envMin[static_cast<size_t>(c)] = waveDbWarp(mn);
        envMax[static_cast<size_t>(c)] = waveDbWarp(mx);
    }
    if (work) work->columns += static_cast<uint64_t>(cols);
    return true;
}

bool fillWaveEnvelope(const float* wave, int n, int iLo, int iHi, int cols, double invSr,
                      MinMaxPyramid* pyr, uint64_t epoch, float* envX, float* envMin,
                      float* envMax, WaveEnvelopeWork* work) {
    if (!wave || !envX || !envMin || !envMax || cols <= 0 || invSr <= 0.0) return false;
    if (!clampVisibleRange(n, iLo, iHi)) return false;
    const int visN = iHi - iLo + 1;
    const int spp = (visN + cols - 1) / cols;

    // Fine zoom: exact scan of the (small) visible range; do not pay for a full pyramid.
    if (!pyr || spp < kWaveEnvBlock) {
        return fillWaveEnvelopeExact(wave, n, iLo, iHi, cols, invSr, envX, envMin, envMax, work);
    }

    ensureMinMaxPyramid(*pyr, wave, n, epoch, work);
    if (pyr->n != n || pyr->blockMin.empty() ||
        pyr->blockMin.size() != pyr->blockMax.size()) {
        return fillWaveEnvelopeExact(wave, n, iLo, iHi, cols, invSr, envX, envMin, envMax, work);
    }

    const int nb = static_cast<int>(pyr->blockMin.size());
    for (int c = 0; c < cols; ++c) {
        int s0 = iLo + static_cast<int>((static_cast<int64_t>(visN) * c) / cols);
        int s1 = iLo + static_cast<int>((static_cast<int64_t>(visN) * (c + 1)) / cols);
        if (s1 <= s0) s1 = s0 + 1;
        if (s1 > n) s1 = n;
        int b0 = s0 / kWaveEnvBlock;
        int b1 = (s1 - 1) / kWaveEnvBlock;
        if (b0 < 0) b0 = 0;
        if (b1 >= nb) b1 = nb - 1;
        if (work) work->blockQueries += static_cast<uint64_t>(b1 - b0 + 1);
        float mn = pyr->blockMin[static_cast<size_t>(b0)];
        float mx = pyr->blockMax[static_cast<size_t>(b0)];
        for (int b = b0 + 1; b <= b1; ++b) {
            mn = std::min(mn, pyr->blockMin[static_cast<size_t>(b)]);
            mx = std::max(mx, pyr->blockMax[static_cast<size_t>(b)]);
        }
        envX[static_cast<size_t>(c)] = static_cast<float>(0.5 * static_cast<double>(s0 + s1) * invSr);
        envMin[static_cast<size_t>(c)] = waveDbWarp(mn);
        envMax[static_cast<size_t>(c)] = waveDbWarp(mx);
    }
    if (work) work->columns += static_cast<uint64_t>(cols);
    return true;
}

}  // namespace wa
