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

void queryPhysicalRange(WaveHistoryView wave, const MinMaxPyramid& pyr, int start, int end,
                        float& mn, float& mx, WaveEnvelopeWork* work) {
    float minimum = mn, maximum = mx;
    if (start % kWaveEnvBlock) {
        const int rawEnd = std::min(end, (start / kWaveEnvBlock + 1) * kWaveEnvBlock);
        if (work) work->exactSamples += static_cast<uint64_t>(rawEnd - start);
        for (; start < rawEnd; ++start) {
            minimum = std::min(minimum, wave.data[start]);
            maximum = std::max(maximum, wave.data[start]);
        }
    }
    if (start == end) { mn = minimum; mx = maximum; return; }
    const int firstBlock = start / kWaveEnvBlock;
    const int lastBlock = end == wave.n ? (end + kWaveEnvBlock - 1) / kWaveEnvBlock
                                       : end / kWaveEnvBlock;
    if (work) work->blockQueries += static_cast<uint64_t>(lastBlock - firstBlock);
    for (int b = firstBlock; b < lastBlock; ++b) {
        minimum = std::min(minimum, pyr.blockMin[static_cast<size_t>(b)]);
        maximum = std::max(maximum, pyr.blockMax[static_cast<size_t>(b)]);
    }
    start = std::min(end, lastBlock * kWaveEnvBlock);
    if (work) work->exactSamples += static_cast<uint64_t>(end - start);
    for (; start < end; ++start) {
        minimum = std::min(minimum, wave.data[start]);
        maximum = std::max(maximum, wave.data[start]);
    }
    mn = minimum;
    mx = maximum;
}

// Compile the contiguous path separately so freeze queries retain their original cost.
template<bool Ring>
void fillIndexedColumns(WaveHistoryView wave, int iLo, int visN, int cols, double invSr,
                        const MinMaxPyramid* pyr, float* envX, float* envMin, float* envMax,
                        WaveEnvelopeWork* work) {
    const int n = wave.n;
    for (int c = 0; c < cols; ++c) {
        int s0 = iLo + static_cast<int>((static_cast<int64_t>(visN) * c) / cols);
        int s1 = iLo + static_cast<int>((static_cast<int64_t>(visN) * (c + 1)) / cols);
        if (s1 <= s0) s1 = s0 + 1;
        if (s1 > n) s1 = n;
        float mn, mx;
        if constexpr (!Ring) {
            // Common contiguous/frozen case: the logical blocks are already indexed.
            const int b0 = s0 / kWaveEnvBlock;
            const int b1 = (s1 - 1) / kWaveEnvBlock;
            mn = pyr->blockMin[static_cast<size_t>(b0)];
            mx = pyr->blockMax[static_cast<size_t>(b0)];
            for (int b = b0 + 1; b <= b1; ++b) {
                mn = std::min(mn, pyr->blockMin[static_cast<size_t>(b)]);
                mx = std::max(mx, pyr->blockMax[static_cast<size_t>(b)]);
            }
            if (work) work->blockQueries += static_cast<uint64_t>(b1 - b0 + 1);
        } else {
            // Keep logical block snapping; physical edges are scanned exactly.
            const int begin = (s0 / kWaveEnvBlock) * kWaveEnvBlock;
            const int end = std::min(n, ((s1 - 1) / kWaveEnvBlock + 1) * kWaveEnvBlock);
            int physical = wave.first + begin;
            if (physical >= n) physical -= n;
            const int firstEnd = std::min(n, physical + end - begin);
            mn = wave.data[physical];
            mx = mn;
            queryPhysicalRange(wave, *pyr, physical, firstEnd, mn, mx, work);
            if (physical + end - begin > n)
                queryPhysicalRange(wave, *pyr, 0, physical + end - begin - n, mn, mx, work);
        }
        envX[static_cast<size_t>(c)] = static_cast<float>(0.5 * static_cast<double>(s0 + s1) * invSr);
        envMin[static_cast<size_t>(c)] = waveDbWarp(mn);
        envMax[static_cast<size_t>(c)] = waveDbWarp(mx);
    }
}

}  // namespace

void ensureMinMaxPyramid(MinMaxPyramid& pyr, WaveHistoryView wave, uint64_t epoch,
                         WaveEnvelopeWork* work, uint64_t generation) {
    const int n = wave.n;
    if (!wave.data || n <= 0) {
        pyr.epoch = UINT64_MAX;
        pyr.n = 0;
        pyr.blockMin.clear();
        pyr.blockMax.clear();
        return;
    }
    const int nb = (n + kWaveEnvBlock - 1) / kWaveEnvBlock;
    const bool compatible = pyr.generation == generation && pyr.n == n &&
        static_cast<int>(pyr.blockMin.size()) == nb && pyr.blockMax.size() == pyr.blockMin.size();
    if (compatible && pyr.epoch == epoch && pyr.updatedSamples == wave.updatedSamples)
        return;

    const bool incremental = compatible && wave.updatedSamples != UINT64_MAX &&
        pyr.updatedSamples != UINT64_MAX && wave.updatedSamples >= pyr.updatedSamples &&
        wave.updatedSamples - pyr.updatedSamples < static_cast<uint64_t>(n);
    pyr.blockMin.resize(static_cast<size_t>(nb));
    pyr.blockMax.resize(static_cast<size_t>(nb));
    const auto updateBlock = [&](int b) {
        const int s0 = b * kWaveEnvBlock;
        const int s1 = std::min(n, s0 + kWaveEnvBlock);
        if (work) {
            if (incremental) work->updateSamples += static_cast<uint64_t>(s1 - s0);
            else work->rebuildSamples += static_cast<uint64_t>(s1 - s0);
        }
        float mn = wave.data[s0];
        float mx = wave.data[s0];
        for (int s = s0 + 1; s < s1; ++s) {
            const float v = wave.data[s];
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        pyr.blockMin[static_cast<size_t>(b)] = mn;
        pyr.blockMax[static_cast<size_t>(b)] = mx;
    };
    if (incremental) {
        const int count = static_cast<int>(wave.updatedSamples - pyr.updatedSamples);
        const int start = (wave.first + n - count) % n;
        const int end = std::min(n, start + count);
        if (count > 0) {
            for (int b = start / kWaveEnvBlock; b <= (end - 1) / kWaveEnvBlock; ++b)
                updateBlock(b);
            if (start + count > n)
                for (int b = 0; b <= (start + count - n - 1) / kWaveEnvBlock; ++b)
                    updateBlock(b);
        }
    } else {
        for (int b = 0; b < nb; ++b) updateBlock(b);
    }
    pyr.n = n;
    pyr.epoch = epoch;
    pyr.generation = generation;
    pyr.updatedSamples = wave.updatedSamples;
}

bool fillWaveEnvelopeExact(WaveHistoryView wave, int iLo, int iHi, int cols, double invSr,
                           float* envX, float* envMin, float* envMax, WaveEnvelopeWork* work) {
    const int n = wave.n;
    if (!wave.data || !envX || !envMin || !envMax || cols <= 0 || invSr <= 0.0) return false;
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

bool fillWaveEnvelope(WaveHistoryView wave, int iLo, int iHi, int cols, double invSr,
                      MinMaxPyramid* pyr, uint64_t epoch, float* envX, float* envMin,
                      float* envMax, WaveEnvelopeWork* work, uint64_t generation) {
    const int n = wave.n;
    if (!wave.data || !envX || !envMin || !envMax || cols <= 0 || invSr <= 0.0) return false;
    if (!clampVisibleRange(n, iLo, iHi)) return false;
    const int visN = iHi - iLo + 1;
    const int spp = (visN + cols - 1) / cols;

    // Fine zoom: exact scan of the (small) visible range; do not pay for a full pyramid.
    if (!pyr || spp < kWaveEnvBlock) {
        return fillWaveEnvelopeExact(wave, iLo, iHi, cols, invSr, envX, envMin, envMax, work);
    }

    ensureMinMaxPyramid(*pyr, wave, epoch, work, generation);
    if (pyr->n != n || pyr->blockMin.empty() ||
        pyr->blockMin.size() != pyr->blockMax.size()) {
        return fillWaveEnvelopeExact(wave, iLo, iHi, cols, invSr, envX, envMin, envMax, work);
    }

    if (wave.first == 0)
        fillIndexedColumns<false>(wave, iLo, visN, cols, invSr, pyr, envX, envMin, envMax, work);
    else
        fillIndexedColumns<true>(wave, iLo, visN, cols, invSr, pyr, envX, envMin, envMax, work);
    if (work) work->columns += static_cast<uint64_t>(cols);
    return true;
}

}  // namespace wa
