#pragma once
#include <complex>
#include <cstddef>
#include <vector>
namespace wa {
void fftRadix2(std::complex<float>* data, size_t n);   // n must be a power of two
void applyHann(float* inout, size_t n);
// Hann window over L=count, zero-pad to next pow2, FFT, single-sided magnitude in dBFS.
// Normalized by window length L (full-scale on-bin sine -> 0 dBFS). workBuf size >= padded N.
void magnitudeSpectrumDb(const float* samples, size_t count, std::complex<float>* workBuf,
                         std::vector<float>& magDbOut, float floorDb = -120.f);
} // namespace wa
