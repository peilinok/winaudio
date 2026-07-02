#include "Fft.h"
#include <cmath>
namespace wa {
namespace { constexpr double kPi = 3.14159265358979323846; }

void fftRadix2(std::complex<float>* a, size_t n) {
    // iterative radix-2 DIT; n power of two
    for (size_t i = 1, j = 0; i < n; ++i) {           // bit reversal
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * kPi / (double)len;
        std::complex<float> wlen((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len/2; ++k) {
                std::complex<float> u = a[i+k];
                std::complex<float> v = a[i+k+len/2] * w;
                a[i+k] = u + v;
                a[i+k+len/2] = u - v;
                w *= wlen;
            }
        }
    }
}

void applyHann(float* x, size_t n) {
    if (n < 2) return;
    for (size_t i = 0; i < n; ++i)
        x[i] *= (float)(0.5 * (1.0 - std::cos(2.0*kPi*i/(n-1))));
}

void magnitudeSpectrumDb(const float* samples, size_t count, std::complex<float>* work,
                         std::vector<float>& out, float floorDb) {
    size_t N = 1; while (N < count) N <<= 1;
    double winSum = 0.0;                               // = L * coherentGain
    for (size_t i = 0; i < count; ++i) {
        double w = (count < 2) ? 1.0 : 0.5 * (1.0 - std::cos(2.0*kPi*i/(count-1)));
        work[i] = std::complex<float>((float)(samples[i]*w), 0.0f);
        winSum += w;
    }
    for (size_t i = count; i < N; ++i) work[i] = std::complex<float>(0.0f, 0.0f);
    fftRadix2(work, N);
    const double norm = (winSum > 0.0 ? winSum : 1.0);
    const size_t bins = N/2 + 1;                       // single-sided incl. Nyquist (N/2+1 bins)
    out.resize(bins);
    for (size_t k = 0; k < bins; ++k) {
        double mag = (double)std::abs(work[k]) / norm;
        if (k != 0 && k != N/2) mag *= 2.0;            // single-sided: DC and Nyquist are not doubled
        double db = (mag > 0.0) ? 20.0*std::log10(mag) : (double)floorDb;
        if (db < floorDb) db = floorDb;
        out[k] = (float)db;
    }
}
} // namespace wa
