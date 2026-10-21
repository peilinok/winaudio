#include "Fft.h"
#include <cmath>
#include <vector>
namespace wa {
namespace {
constexpr double kPi = 3.14159265358979323846;

// Cached Hann weights + coherent-gain sum for the last window length.
// GUI always uses fftWin=2048; tests may use other sizes (rebuild is cheap).
struct HannCache {
    size_t n = 0;
    double winSum = 0.0;
    std::vector<float> w;
};
HannCache& hannCache() {
    static HannCache c;
    return c;
}

void ensureHann(size_t count) {
    HannCache& c = hannCache();
    if (c.n == count) return;
    c.n = count;
    c.w.resize(count);
    c.winSum = 0.0;
    if (count < 2) {
        if (count == 1) {
            c.w[0] = 1.0f;
            c.winSum = 1.0;
        }
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const double wi = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) /
                                                static_cast<double>(count - 1)));
        c.w[i] = static_cast<float>(wi);
        c.winSum += wi;
    }
}
} // namespace

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
    ensureHann(n);
    const float* w = hannCache().w.data();
    for (size_t i = 0; i < n; ++i) x[i] *= w[i];
}

void magnitudeSpectrumDb(const float* samples, size_t count, std::complex<float>* work,
                         std::vector<float>& out, float floorDb) {
    size_t N = 1; while (N < count) N <<= 1;
    ensureHann(count);
    const HannCache& hc = hannCache();
    const float* w = hc.w.data();
    for (size_t i = 0; i < count; ++i)
        work[i] = std::complex<float>(samples[i] * w[i], 0.0f);
    for (size_t i = count; i < N; ++i) work[i] = std::complex<float>(0.0f, 0.0f);
    fftRadix2(work, N);
    const double norm = (hc.winSum > 0.0 ? hc.winSum : 1.0);
    const double invNorm = 1.0 / norm;
    const size_t bins = N/2 + 1;                       // single-sided incl. Nyquist (N/2+1 bins)
    if (out.size() != bins) out.resize(bins);
    // db = 20*log10(scale * sqrt(re^2+im^2)) = 10*log10(re^2+im^2) + 20*log10(scale)
    // scale is invNorm for DC/Nyquist, 2*invNorm otherwise.
    const double dcOff = 20.0 * std::log10(invNorm);
    const double ssOff = 20.0 * std::log10(2.0 * invNorm);
    for (size_t k = 0; k < bins; ++k) {
        const double re = static_cast<double>(work[k].real());
        const double im = static_cast<double>(work[k].imag());
        const double magSq = re * re + im * im;
        double db;
        if (magSq <= 0.0) {
            db = static_cast<double>(floorDb);
        } else {
            const double off = (k == 0 || k == N / 2) ? dcOff : ssOff;
            db = 10.0 * std::log10(magSq) + off;
            if (db < floorDb) db = floorDb;
        }
        out[k] = static_cast<float>(db);
    }
}
} // namespace wa