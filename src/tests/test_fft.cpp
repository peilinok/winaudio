#include <gtest/gtest.h>
#include <vector>
#include <complex>
#include <cmath>
#include "Fft.h"
using namespace wa;
static constexpr double kPi = 3.14159265358979323846;

TEST(Fft, OnBinFullScaleSineIs0dBFS) {
    const size_t N = 2048; const double fs = 48000.0;
    const int bin = 43; const double freq = bin * fs / N;     // on-bin: 1007.8125 Hz
    std::vector<float> x(N);
    for (size_t i = 0; i < N; ++i) x[i] = (float)std::sin(2*kPi*freq*i/fs); // amplitude 1.0
    std::vector<std::complex<float>> work(N);
    std::vector<float> mag;
    magnitudeSpectrumDb(x.data(), N, work.data(), mag);
    ASSERT_EQ(mag.size(), N/2 + 1);                            // single-sided incl. Nyquist
    EXPECT_NEAR(mag[bin], 0.0f, 0.1f);                         // full-scale on-bin sine = 0 dBFS
    EXPECT_LT(mag[bin/2], -40.0f);                             // far bins well below
}

TEST(Fft, ZeroPaddedCalibrationUsesWindowLength) {
    const size_t count = 1500;                                 // not a power of two -> pad to 2048
    const size_t Nfft = 2048; const double fs = 48000.0;
    const int bin = 40; const double freq = bin * fs / Nfft;   // on a 2048-bin
    std::vector<float> x(count);
    for (size_t i = 0; i < count; ++i) x[i] = (float)std::sin(2*kPi*freq*i/fs);
    std::vector<std::complex<float>> work(Nfft);
    std::vector<float> mag;
    magnitudeSpectrumDb(x.data(), count, work.data(), mag);
    ASSERT_EQ(mag.size(), Nfft/2 + 1);                         // single-sided incl. Nyquist
    EXPECT_NEAR(mag[bin], 0.0f, 0.6f);                         // normalized by window length L=count
}

TEST(Fft, SilenceFloored) {
    const size_t N = 1024;
    std::vector<float> x(N, 0.0f);
    std::vector<std::complex<float>> work(N);
    std::vector<float> mag;
    magnitudeSpectrumDb(x.data(), N, work.data(), mag, -120.f);
    for (float d : mag) EXPECT_FLOAT_EQ(d, -120.f);
}

TEST(Fft, ImpulseApproximatelyFlat) {
    // A single windowed impulse at the window CENTER transforms to a flat magnitude spectrum
    // (|FFT of a delta| is constant across bins). Placement matters: the Hann window zeros
    // index 0, so an impulse at x[0] vanishes -- the old version asserted only isfinite and
    // was vacuous.
    const size_t N = 1024;
    std::vector<float> x(N, 0.0f);
    x[N / 2] = 1.0f;
    std::vector<std::complex<float>> work(N);
    std::vector<float> mag;
    magnitudeSpectrumDb(x.data(), N, work.data(), mag);
    ASSERT_EQ(mag.size(), N / 2 + 1);
    // Interior bins (excluding DC and Nyquist, which are not doubled by the single-sided
    // scaling) must be flat to within a tight band, and clearly above the floor.
    float lo = mag[1], hi = mag[1];
    for (size_t k = 1; k < N / 2; ++k) {
        if (mag[k] < lo) lo = mag[k];
        if (mag[k] > hi) hi = mag[k];
    }
    EXPECT_LT(hi - lo, 0.1f);     // spectrally flat
    EXPECT_GT(mag[1], -60.0f);    // real energy present (not floored)
}
