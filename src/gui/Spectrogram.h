#pragma once
#include <vector>
#include <cstdint>
namespace wa {
// Rolling log-frequency spectrogram. Each pushColumn reduces a linear FFT magnitude column
// (dBFS, size N/2+1) into logRows log-spaced frequency bands (MAX over each band's bins) and
// appends it as the newest (right-most) column, scrolling older columns left. data() is a
// contiguous rows*cols row-major buffer for ImPlot::PlotHeatmap: row 0 = HIGHEST freq (top),
// col 0 = OLDEST (left), col cols-1 = NEWEST (right). Empty cells = floorDb.
class Spectrogram {
public:
    Spectrogram(int logRows, int cols, double fmin, double fmax, uint32_t sampleRate,
                float floorDb = -96.f);
    void pushColumn(const std::vector<float>& linMagDb); // size N/2+1; N inferred on first call
    void clear();                          // reset all cells to floorDb
    const float* data() const;             // rows*cols, row-major (row0=fmax top, col scroll)
    int    rows() const { return rows_; }
    int    cols() const { return cols_; }
    double fmin() const { return fmin_; }
    double fmax() const { return fmax_; }
private:
    void buildMap(size_t magBins);         // precompute per-row [binLo,binHi] once (lazy)
    int rows_, cols_; double fmin_, fmax_; uint32_t sr_; float floor_;
    bool mapped_ = false;
    size_t mappedBins_ = 0;                // magBins value used when buildMap was called
    std::vector<int> rowBinLo_, rowBinHi_; // size rows_, inclusive bin range per row
    std::vector<float> buf_;               // rows_*cols_, row-major
};
} // namespace wa
