#include "Spectrogram.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace wa {

Spectrogram::Spectrogram(int logRows, int cols, double fmin, double fmax,
                         uint32_t sampleRate, float floorDb)
    : rows_(logRows), cols_(cols), fmin_(fmin), fmax_(fmax), sr_(sampleRate), floor_(floorDb)
{
    buf_.assign(rows_ * cols_, floor_);
}

void Spectrogram::clear() {
    std::fill(buf_.begin(), buf_.end(), floor_);
}

void Spectrogram::buildMap(size_t magBins) {
    // N = FFT size; magBins = N/2+1
    int N = (int)((magBins - 1) * 2);
    rowBinLo_.resize(rows_);
    rowBinHi_.resize(rows_);
    int maxBin = (int)magBins - 1;
    for (int r = 0; r < rows_; ++r) {
        // edge(e) = fmax_ * pow(fmin_/fmax_, e/rows_)
        // edge(0)=fmax_, edge(rows_)=fmin_, decreasing with e.
        // Row r covers the band [edge(r+1), edge(r)] (lower to higher frequency).
        double edgeLo = fmax_ * std::pow(fmin_ / fmax_, (double)(r + 1) / rows_);
        double edgeHi = fmax_ * std::pow(fmin_ / fmax_, (double)r / rows_);

        int lo = (int)std::floor(edgeLo * N / sr_);
        int hi = (int)std::ceil(edgeHi * N / sr_);

        // Clamp both to [0, maxBin] (inclusive range, so hi==maxBin is valid and includes Nyquist)
        lo = std::max(0, std::min(lo, maxBin));
        hi = std::max(0, std::min(hi, maxBin));

        // Ensure at least 1 bin in the inclusive range
        if (hi < lo) hi = lo;

        rowBinLo_[r] = lo;
        rowBinHi_[r] = hi;
    }
    mappedBins_ = magBins;
    mapped_ = true;
}

void Spectrogram::pushColumn(const std::vector<float>& linMagDb) {
    if (linMagDb.empty()) return;
    if (!mapped_) buildMap(linMagDb.size());
    // Defensive: ignore if size changed since buildMap
    if (linMagDb.size() != mappedBins_) return;

    int magSize = (int)linMagDb.size();
    for (int r = 0; r < rows_; ++r) {
        int lo = rowBinLo_[r];
        int hi = rowBinHi_[r];

        // MAX reduction over the inclusive bin range [lo, hi]
        float v = floor_;
        for (int b = lo; b <= hi && b < magSize; ++b)
            v = std::max(v, linMagDb[b]);

        // Scroll left: discard oldest (col 0), shift remaining left by 1
        float* row = &buf_[r * cols_];
        std::memmove(row, row + 1, (size_t)(cols_ - 1) * sizeof(float));
        row[cols_ - 1] = v;
    }
}

const float* Spectrogram::data() const {
    return buf_.data();
}

} // namespace wa
