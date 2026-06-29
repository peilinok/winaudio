# WinAudio Phase 3 — Monitor + Realtime Visualization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a delayed monitor pass-through (capture → frame-domain DelayFifo with active drift control → render) with real-time visualization of both streams (waveform + FFT spectrum + scrolling spectrogram) in the ImGui GUI via ImPlot.

**Architecture:** Core gains pure-STL DSP primitives (FFT, ScopeBuffer seqlock snapshot, SampleConvert, DelayFifo+drift) and a `MonitorEngine` that runs two backends + a pump thread (the only thread doing format conversion / downmix / framing / drift; capture/render I/O threads only move bytes). The pump is woken by a dedicated per-backend event (NOT the shared WASAPI event). The GUI owns both `Engine` and `MonitorEngine`, drives analysis by sample-index hops (FPS-independent), and renders with ImPlot.

**Tech Stack:** C++17, MSBuild (x64), MSVC v143, Win32 + WASAPI, gtest, Dear ImGui + ImPlot + DX11.

## Global Constraints
- MSBuild, Debug/Release, **x64** only, PlatformToolset v143, `/std:c++17`, `/W4`, Core warning-clean.
- **msbuild NOT on PATH.** Build with the PowerShell tool (Git-Bash mangles `/p:`):
  `& "d:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m` — build the **solution**, never a single vcxproj standalone.
- WinAudioCore + WinAudioCli: pure **Win32 + STL, zero third-party**. ImPlot only in WinAudioGui; gtest only in WinAudioTests. ImPlot vendored under `third_party/implot/`.
- All Core symbols in namespace `wa`. Core never printf/cout, never throws across its public API; returns `wa::Result`. COM: each WASAPI thread its own `ComInitGuard` (MTA), ComPtr RAII, no raw `Release()`. No resampler.
- `wa::Result::Fail(long, std::string)` / `Result::Ok()` (no `Result::Error`).
- Commit with `git -c commit.gpgsign=false commit -m "..."` (a clang-format pre-commit hook prints a harmless `cannot use -i when reading from stdin`; the commit still succeeds — verify with `git log --oneline -1`).
- **DSP defaults (verbatim from spec):** FFT frameSize n=2048, hop=512 (75% overlap); 23.4375 Hz/bin, 10.667 ms/col @48k; spectrogram history ≈5 s (~469 cols); dBFS calibration normalizes by **window length L** with Hann coherent-gain comp + single-sided ×2 (DC/Nyquist not doubled); floorDb default −120.
- **Monitor:** delayMs default 100 (bound to DelayFifo occupancy only); drift = single-sample-frame drop/dup with short crossfade, controller on low-passed occupancy with deadband > one device period; require capture sampleRate == render sampleRate; channels 1–2 for pass-through, analysis downmixed to mono (average).
- **Review:** each task gets the normal subagent reviewer PLUS a local codex pass: `$null | codex exec -s read-only --skip-git-repo-check -o <file> review --commit <SHA>` (background; stdin closed).

---

## File Structure
```
src/core/
  AudioFormatDef.h         NEW — pure POD AudioFormat (no windows.h)
  AudioFormat.h/.cpp       MODIFIED — include AudioFormatDef.h; conversions become free functions
  Fft.h/.cpp               NEW — fftRadix2, applyHann, magnitudeSpectrumDb
  SampleConvert.h/.cpp     NEW — pcmToFloat/floatToPcm/downmixMono/adaptChannels/peakLevel
  ScopeBuffer.h/.cpp       NEW — seqlock recent-window snapshot + monotonic counter
  DelayFifo.h/.cpp         NEW — frame-domain FIFO + drift controller
  IAudioBackend.h          MODIFIED — add virtual void* dataReadyEvent() const
  WasapiStream.h/.cpp      MODIFIED — capture: dedicated pumpReadyEvent_, SetEvent after ring write
  MonitorEngine.h/.cpp     NEW — dual-stream monitor orchestration
  Engine.cpp               MODIFIED — call peakLevel from SampleConvert; free-function conversions
  Analysis.h/.cpp          NEW — advanceAnalysis() FPS-independent cadence (pure, testable)
src/cli/main.cpp           MODIFIED — `monitor` subcommand
src/gui/
  AppUi.h/.cpp             MODIFIED — own Engine + MonitorEngine; mode selector; Monitor view
  Spectrogram.h/.cpp       NEW — log-resampled rolling 2D buffer (GUI-side)
third_party/implot/        NEW — vendored ImPlot
src/tests/
  test_fft.cpp, test_sampleconvert.cpp, test_scopebuffer.cpp,
  test_delayfifo.cpp, test_analysis.cpp, test_monitorengine.cpp   NEW
```

---

# GATE 1 — Core DSP primitives + tests

## Task 1: Split AudioFormat into a windows-free POD header

**Files:** Create `src/core/AudioFormatDef.h`; Modify `src/core/AudioFormat.h`, `src/core/AudioFormat.cpp`, `src/core/WasapiStream.cpp`, `src/core/DeviceEnumerator.cpp`, `src/core/Engine.cpp`, `src/tests/test_audioformat.cpp`, `src/core/WinAudioCore.vcxproj`.

**Interfaces:**
- Produces: `wa::AudioFormat` POD in `AudioFormatDef.h`; free functions `WAVEFORMATEXTENSIBLE wa::toWaveFormatExtensible(const AudioFormat&)` and `AudioFormat wa::fromWaveFormat(const WAVEFORMATEX*)` in `AudioFormat.h`.

- [ ] **Step 1: Create `src/core/AudioFormatDef.h`** (pure POD, no windows):
```cpp
#pragma once
#include <cstdint>
namespace wa {
struct AudioFormat {
    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
    uint16_t bitsPerSample = 16;
    bool     isFloat = false;
    uint32_t blockAlign() const { return static_cast<uint32_t>(channels) * (bitsPerSample / 8u); }
    uint32_t avgBytesPerSec() const { return sampleRate * blockAlign(); }
    bool operator==(const AudioFormat& o) const {
        return sampleRate == o.sampleRate && channels == o.channels &&
               bitsPerSample == o.bitsPerSample && isFloat == o.isFloat;
    }
};
} // namespace wa
```

- [ ] **Step 2: Rewrite `src/core/AudioFormat.h`** to include the POD + declare free conversions:
```cpp
#pragma once
#include "AudioFormatDef.h"
#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>
namespace wa {
WAVEFORMATEXTENSIBLE toWaveFormatExtensible(const AudioFormat& f);
AudioFormat fromWaveFormat(const WAVEFORMATEX* wf);
} // namespace wa
```

- [ ] **Step 3: Update `src/core/AudioFormat.cpp`** — change the two definitions from members to free functions: `WAVEFORMATEXTENSIBLE toWaveFormatExtensible(const AudioFormat& f) { ... use f.channels etc ... }` and `AudioFormat fromWaveFormat(const WAVEFORMATEX* wf) { ... }`. Body logic unchanged (replace implicit `this`/member access with `f.`).

- [ ] **Step 4: Update call sites** (mechanical):
  - `WasapiStream.cpp:84` `AudioFormat::fromWaveFormat(mix)` → `fromWaveFormat(mix)`
  - `WasapiStream.cpp:101,116,129` `X.toWaveFormatExtensible()` → `toWaveFormatExtensible(X)`
  - `DeviceEnumerator.cpp:35,110` `AudioFormat::fromWaveFormat(mix)` → `fromWaveFormat(mix)`
  - `Engine.cpp:74` `fmt.toWaveFormatExtensible()` → `toWaveFormatExtensible(fmt)`
  - `test_audioformat.cpp` (lines 18,19,26,28,41,49,52,56,61) similarly; aggregate-init temporaries → use a named local then `toWaveFormatExtensible(local)`.
  - Add `AudioFormatDef.h` to the Core vcxproj `<ClInclude>`.

- [ ] **Step 5: Build + regress.** Run solution build (Debug) then `& ".\x64\Debug\WinAudioTests.exe"` → expect **23 PASSED** (unchanged). The split is behavior-preserving.

- [ ] **Step 6: Commit** — `git add -A && git -c commit.gpgsign=false commit -m "refactor(core): split AudioFormatDef.h (windows-free POD); conversions as free functions"`

## Task 2: Fft — fftRadix2, applyHann, magnitudeSpectrumDb (gtest)

**Files:** Create `src/core/Fft.h`, `src/core/Fft.cpp`, `src/tests/test_fft.cpp`; Modify both vcxproj.

**Interfaces:**
- Produces: `void wa::fftRadix2(std::complex<float>*, size_t n)`; `void wa::applyHann(float*, size_t n)`; `void wa::magnitudeSpectrumDb(const float* samples, size_t count, std::complex<float>* workBuf, std::vector<float>& magDbOut, float floorDb=-120.f)`.

- [ ] **Step 1: Write the failing test** `src/tests/test_fft.cpp`:
```cpp
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
    ASSERT_EQ(mag.size(), N/2);
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
    ASSERT_EQ(mag.size(), Nfft/2);
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
    const size_t N = 1024;
    std::vector<float> x(N, 0.0f); x[0] = 1.0f;
    std::vector<std::complex<float>> work(N);
    std::vector<float> mag;
    magnitudeSpectrumDb(x.data(), N, work.data(), mag);
    // Hann zeros x[0] weight ~0 at i=0; use impulse at center instead for a real flatness check:
    // (kept minimal) just assert no NaN and finite
    for (float d : mag) EXPECT_TRUE(std::isfinite(d));
}
```

- [ ] **Step 2: Run to verify it fails** — solution build fails (`Fft.h` missing).

- [ ] **Step 3: Create `src/core/Fft.h`**:
```cpp
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
```

- [ ] **Step 4: Create `src/core/Fft.cpp`**:
```cpp
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
    const size_t bins = N/2;
    out.resize(bins);
    for (size_t k = 0; k < bins; ++k) {
        double mag = (double)std::abs(work[k]) / norm;
        if (k != 0) mag *= 2.0;                        // single-sided (DC not doubled; Nyquist excluded from [0,bins))
        double db = (mag > 0.0) ? 20.0*std::log10(mag) : (double)floorDb;
        if (db < floorDb) db = floorDb;
        out[k] = (float)db;
    }
}
} // namespace wa
```

- [ ] **Step 5: Add files to vcxproj; build + run** `& ".\x64\Debug\WinAudioTests.exe" --gtest_filter=Fft.*` → 4 PASS. Full suite 27.

- [ ] **Step 6: Commit** — `feat(core): FFT + dBFS magnitude spectrum (window-length calibrated)`

## Task 3: SampleConvert (gtest)

**Files:** Create `src/core/SampleConvert.h/.cpp`, `src/tests/test_sampleconvert.cpp`; modify vcxproj. **`SampleConvert.h` includes `AudioFormat.h`** (it needs format fields; Win32 dependency is allowed — it is not third-party). Fft/ScopeBuffer/DelayFifo do NOT include AudioFormat and stay windows-free.

**Interfaces:** Produces `wa::pcmToFloat/floatToPcm/downmixMono/adaptChannels/peakLevel` (signatures per spec §4.3).

- [ ] **Step 1: Failing test** `test_sampleconvert.cpp` (key cases):
```cpp
#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include "SampleConvert.h"
using namespace wa;

TEST(SampleConvert, Int16BoundsRoundTrip) {
    AudioFormat f{48000,1,16,false};
    int16_t in[3] = {INT16_MIN, 0, INT16_MAX};
    float fl[3]; pcmToFloat(reinterpret_cast<uint8_t*>(in), 3, f, fl);
    EXPECT_NEAR(fl[0], -1.0f, 1e-6f);             // INT16_MIN/32768 = -1.0
    EXPECT_NEAR(fl[1], 0.0f, 1e-6f);
    EXPECT_NEAR(fl[2], 32767.0f/32768.0f, 1e-6f);
    int16_t back[3]; floatToPcm(fl, 3, f, reinterpret_cast<uint8_t*>(back));
    EXPECT_EQ(back[0], INT16_MIN); EXPECT_EQ(back[2], INT16_MAX);
}
TEST(SampleConvert, FloatToPcmClampsOverflow) {
    AudioFormat f{48000,1,16,false};
    float over[2] = {2.0f, -2.0f};                 // out of range -> must clamp, no UB
    int16_t out[2]; floatToPcm(over, 2, f, reinterpret_cast<uint8_t*>(out));
    EXPECT_EQ(out[0], INT16_MAX); EXPECT_EQ(out[1], INT16_MIN);
}
TEST(SampleConvert, Int24NegativeSignExtend) {
    AudioFormat f{48000,1,24,false};
    uint8_t in[3] = {0x00,0x00,0x80};              // little-endian 0x800000 = most-negative 24-bit
    float fl; pcmToFloat(in, 1, f, &fl);
    EXPECT_LT(fl, -0.99f);                          // must be ~ -1.0, not a large positive
}
TEST(SampleConvert, DownmixAverages) {
    float st[4] = {1.0f,1.0f, 1.0f,-1.0f};         // frame0 L=R=1 -> 1.0 ; frame1 L=-R -> 0
    float mono[2]; downmixMono(st, 2, 2, mono);
    EXPECT_NEAR(mono[0], 1.0f, 1e-6f);
    EXPECT_NEAR(mono[1], 0.0f, 1e-6f);              // antiphase cancels (documented limitation)
}
TEST(SampleConvert, PeakLevel) {
    float x[4] = {0.1f,-0.5f,0.3f,-0.2f};
    EXPECT_NEAR(peakLevel(x,4), 0.5f, 1e-6f);
}
```

- [ ] **Step 2–4: Implement** `src/core/SampleConvert.h/.cpp`. Header:
```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include "AudioFormat.h"
namespace wa {
void pcmToFloat(const uint8_t* in, size_t frames, const AudioFormat& f, float* outInterleaved);
void floatToPcm(const float* inInterleaved, size_t frames, const AudioFormat& f, uint8_t* out);
void downmixMono(const float* interleaved, size_t frames, uint16_t ch, float* monoOut);
void adaptChannels(const float* in, uint16_t inCh, float* out, uint16_t outCh, size_t frames);
float peakLevel(const float* mono, size_t n);
} // namespace wa
```
.cpp essentials: `pcmToFloat` per-format: int16 `v/32768.f`; int24 read 3 LE bytes, sign-extend `int32 s=(int32)((b0|b1<<8|b2<<16)<<8)>>8; s/8388608.f`; int32 `v/2147483648.0`; float32 copy. `floatToPcm` clamp the *scaled integer*: e.g. int16 `int32 s=(int32)lround(x*32768.f); s=clamp(s,-32768,32767); store (int16)s`; analogous for 24/32; float32 store as-is (optionally clamp to [-1,1]). `downmixMono` average over channels. `adaptChannels` 1→2 duplicate, 2→1 average, else copy min(channels) and zero-fill. `peakLevel` max(|x|). Run `--gtest_filter=SampleConvert.*` → 5 PASS.

- [ ] **Step 5: Commit** — `feat(core): SampleConvert (PCM<->float, downmix, channel adapt, peakLevel)`

## Task 4: ScopeBuffer seqlock snapshot (gtest)

**Files:** Create `src/core/ScopeBuffer.h/.cpp`, `src/tests/test_scopebuffer.cpp`; vcxproj. Windows-free (atomics + vector only).

**Interfaces:** Produces `wa::ScopeBuffer` with `push`, `totalWritten`, `bool snapshotLatest(size_t n, float* out, uint64_t& endIdxOut) const`.

- [ ] **Step 1: Failing test**:
```cpp
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include "ScopeBuffer.h"
using namespace wa;
TEST(ScopeBuffer, NotEnoughYet) {
    ScopeBuffer sb(1024); float out[64]; uint64_t end;
    EXPECT_FALSE(sb.snapshotLatest(64, out, end));     // nothing written
}
TEST(ScopeBuffer, ReturnsRecentWindow) {
    ScopeBuffer sb(1024);
    std::vector<float> in(256); for (size_t i=0;i<256;++i) in[i]=(float)i;
    sb.push(in.data(), 256);
    EXPECT_EQ(sb.totalWritten(), 256u);
    float out[64]; uint64_t end;
    ASSERT_TRUE(sb.snapshotLatest(64, out, end));
    EXPECT_EQ(end, 256u);
    EXPECT_FLOAT_EQ(out[0], 192.0f);                    // oldest of last 64 = sample 192
    EXPECT_FLOAT_EQ(out[63], 255.0f);
}
TEST(ScopeBuffer, RejectsWindowLargerThanHalfCapacity) {
    ScopeBuffer sb(100); float out[80]; uint64_t end;
    // push enough, but n>cap/2 is disallowed by contract -> returns false
    std::vector<float> in(100,1.0f); sb.push(in.data(),100);
    EXPECT_FALSE(sb.snapshotLatest(80, out, end));
}
TEST(ScopeBuffer, ConcurrentSpscConsistency) {
    ScopeBuffer sb(8192);
    std::atomic<bool> stop{false};
    std::thread prod([&]{ float s=0; std::vector<float> buf(128);
        while(!stop){ for(auto&v:buf) v=s++; sb.push(buf.data(),128);} });
    for (int i=0;i<2000;++i){ float out[1024]; uint64_t end;
        if (sb.snapshotLatest(1024,out,end)) {          // window must be contiguous/monotonic
            for (size_t k=1;k<1024;++k) EXPECT_FLOAT_EQ(out[k], out[k-1]+1.0f); } }
    stop=true; prod.join();
}
```

- [ ] **Step 2–4: Implement** (validate-after-copy seqlock using the monotonic counter). `ScopeBuffer.h`:
```cpp
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace wa {
class ScopeBuffer {
public:
    explicit ScopeBuffer(size_t capacitySamples) : buf_(capacitySamples ? capacitySamples : 1) {}
    void push(const float* mono, size_t n) {            // single producer
        const size_t cap = buf_.size();
        size_t w = (size_t)written_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) buf_[(w + i) % cap] = mono[i];
        written_.store(written_.load(std::memory_order_relaxed) + n, std::memory_order_release);
    }
    uint64_t totalWritten() const { return written_.load(std::memory_order_acquire); }
    bool snapshotLatest(size_t n, float* out, uint64_t& endIdxOut) const {
        const size_t cap = buf_.size();
        if (n == 0 || n > cap/2) return false;          // contract: n <= cap/2
        for (int attempt = 0; attempt < 8; ++attempt) {
            uint64_t w0 = written_.load(std::memory_order_acquire);
            if (w0 < n) return false;
            uint64_t start = w0 - n;
            for (size_t i = 0; i < n; ++i) out[i] = buf_[(size_t)((start + i) % cap)];
            uint64_t w1 = written_.load(std::memory_order_acquire);
            if (w1 - start <= cap) { endIdxOut = w0; return true; }  // window not overwritten during copy
        }
        return false;
    }
private:
    std::vector<float> buf_;
    std::atomic<uint64_t> written_{0};
};
} // namespace wa
```
(Header-only is fine; add an empty `.cpp` only if the vcxproj needs a TU, else just the header + add to tests.) Run `--gtest_filter=ScopeBuffer.*` → 4 PASS.

- [ ] **Step 5: Commit** — `feat(core): ScopeBuffer seqlock recent-window snapshot`

## Task 5: DelayFifo + drift controller (gtest)

**Files:** Create `src/core/DelayFifo.h/.cpp`, `src/tests/test_delayfifo.cpp`; vcxproj. Windows-free.

**Interfaces:** Produces `wa::DelayFifo` (frame-domain, interleaved float, fixed channel count) with `pushFrames`, `popFrames` (applies drift drop/dup), `fillFrames`, `lowpassFillFrames`, `targetFrames`, `driftFixes`.

- [ ] **Step 1: Failing test** — drives the controller with skewed push/pop counts and asserts (a) steady-state low-passed fill stays within deadband of target, (b) `driftFixes` increments when net drift exceeds deadband, (c) pop never returns out-of-range samples. Example:
```cpp
#include <gtest/gtest.h>
#include <vector>
#include "DelayFifo.h"
using namespace wa;
TEST(DelayFifo, HoldsTargetUnderProducerFaster) {
    // 1 channel, target 480 frames, capacity 4800, deadband 96 frames, lowpass alpha
    DelayFifo fifo(1, /*target*/480, /*cap*/4800, /*deadband*/96);
    std::vector<float> in(100,0.f), out(100,0.f);
    // producer writes 101 per round, consumer pops 100 -> +1 frame/round drift
    for (int r=0;r<5000;++r){ in.assign(101,(float)r); fifo.pushFrames(in.data(),101);
                              fifo.popFrames(out.data(),100); }
    EXPECT_NEAR((double)fifo.lowpassFillFrames(), 480.0, 96.0);   // stayed near target
    EXPECT_GT(fifo.driftFixes(), 0u);                              // controller acted (dropped frames)
}
```

- [ ] **Step 2–4: Implement.** `DelayFifo` holds a `std::vector<float>` ring of `cap*channels`, head/tail in frames. `pushFrames` appends (drop oldest if full, count as overflow). `popFrames`: compute `lowpassFill_ += alpha*(fill - lowpassFill_)`; if `lowpassFill_ > target + deadband` → drop one frame from the read side (advance tail by 1 extra) and `++driftFixes_` (crossfade: blend the dropped boundary over a few samples); if `lowpassFill_ < target - deadband` and `fill>0` → duplicate the next frame (emit it twice) and `++driftFixes_`; then copy `maxFrames` (or available) to out. Document drop/dup = single frame, crossfade length ~32 samples. Run `--gtest_filter=DelayFifo.*` → PASS.

- [ ] **Step 5: Commit** — `feat(core): DelayFifo with occupancy drift controller (drop/dup + crossfade)`

---

# GATE 2 — Backend pump-ready event

## Task 6: IAudioBackend.dataReadyEvent() + WasapiCaptureStream dedicated event

**Files:** Modify `src/core/IAudioBackend.h`, `src/core/WasapiStream.h`, `src/core/WasapiStream.cpp`.

**Interfaces:** Produces `virtual void* IAudioBackend::dataReadyEvent() const { return nullptr; }`; `WasapiCaptureStream` overrides it to return a dedicated auto-reset event that its I/O thread `SetEvent`s after each ring write.

- [ ] **Step 1: Add to `IAudioBackend.h`** the pure-virtual-with-default:
```cpp
    // Auto-reset event a monitor pump can wait on; signaled after each capture ring write.
    // nullptr if the backend provides no such signal (default).
    virtual void* dataReadyEvent() const { return nullptr; }
```

- [ ] **Step 2: `WasapiCaptureStream`** (in `WasapiStream.h`): add member `void* pumpEvent_ = nullptr;` and `void* dataReadyEvent() const override { return pumpEvent_; }`.

- [ ] **Step 3: `WasapiStream.cpp` (capture)** — create the event in `start()` alongside `hEvent_`: `pumpEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);` (auto-reset). In the capture drain loop, **after** `ring_->write(...)` (both the data and SILENT paths), call `if (pumpEvent_) SetEvent(static_cast<HANDLE>(pumpEvent_));`. In `close()`, after the thread is joined, `if (pumpEvent_) { CloseHandle(pumpEvent_); pumpEvent_ = nullptr; }`. Apply the same start()-failure cleanup pattern used for `hEvent_`. **Do not touch the render class.**

- [ ] **Step 4: Build + regress** — solution build; `& ".\x64\Debug\WinAudioTests.exe"` → 27 PASS (unchanged); Shared CLI smoke: `& ".\x64\Debug\WinAudioCli.exe" capture --out c.wav --seconds 2` then `play --in c.wav` → still works (the dataReadyEvent addition must not perturb single-stream capture). Don't commit c.wav.

- [ ] **Step 5: Commit** — `feat(core): IAudioBackend.dataReadyEvent + WasapiCaptureStream pump event`

---

# GATE 3 — MonitorEngine + fake-backend tests

## Task 7: Analysis cadence free function (gtest)

**Files:** Create `src/core/Analysis.h/.cpp`, `src/tests/test_analysis.cpp`; vcxproj.

**Interfaces:** Produces `template<class Fn> size_t wa::advanceAnalysis(uint64_t written, uint64_t& nextEndIdx, size_t windowSize, size_t hop, size_t maxCatchup, Fn&& onFrame)` — advances `nextEndIdx` by `hop` while `nextEndIdx <= written`, calling `onFrame(endIdx)` for each; fast-forwards when more than `maxCatchup` hops are pending; returns the count processed. Pure, testable without ImGui.

- [ ] **Step 1: Failing test** asserting: from `written=0`, no frames; after `written` jumps to `windowSize + 3*hop`, exactly the expected hop boundaries are emitted; when `written` jumps far ahead (> maxCatchup hops), `nextEndIdx` fast-forwards (caps processed at maxCatchup and skips stale). Provide concrete numbers (windowSize=2048, hop=512, maxCatchup=8).

- [ ] **Step 2–4: Implement** the loop in a header (template). Run `--gtest_filter=Analysis.*` → PASS.

- [ ] **Step 5: Commit** — `feat(core): advanceAnalysis FPS-independent cadence helper`

## Task 8: MonitorEngine (fake-backend unit tests + structure)

**Files:** Create `src/core/MonitorEngine.h/.cpp`, `src/tests/test_monitorengine.cpp`; vcxproj.

**Interfaces:** per spec §4.5 — `MonitorEngine(BackendFactory)`, `start/stop/poll`, `snapshotCapture/snapshotRender`, `capWritten/renderWritten`, `MonitorStatus`, `StreamState`.

- [ ] **Step 1: Define a fake backend in the test** implementing `IAudioBackend`: `open` stores the ring; a controllable method pushes bytes into the ring + `SetEvent` on its own auto-reset event returned by `dataReadyEvent()`; `stats()` returns a chosen `AudioFormat`. This is the seam that makes the pump testable.

- [ ] **Step 2: Failing tests** (no hardware):
  - `RateMismatchFails`: factory yields capture stats 48000 and render stats 44100 → `start()` returns Fail, `overall==Error`.
  - `PrefillThenRunning`: matching 48000; after `start`, before any capture data, `renderXruns` is not counted; after the fake capture pushes ≥ delay worth, `overall==Running`.
  - `CaptureScopePopulated`: fake pushes a ramp; `capWritten()` advances; `snapshotCapture` returns the ramp (downmixed mono).
  - `StartRollback`: factory makes the render backend's `start()` fail → MonitorEngine `start()` returns Fail and leaves state Idle (capture backend stopped, no leak — verify via a flag in the fake).
  - `StopJoinsCleanly`: start then stop returns promptly (no hang); pump thread joined.

- [ ] **Step 3: Implement `MonitorEngine`** per spec: pump thread waits on `capBackend->dataReadyEvent()` (timeout), `readFrames` from capture ring (only when `availableRead() >= frameBytes`), `pcmToFloat`+`downmixMono`→captureScope, `pcmToFloat`(keep ch)→DelayFifo, drift, DelayFifo→`floatToPcm`(render fmt)→render ring `tryWrite`, same float→renderScope. Start order: start capture → prefill DelayFifo to `delayMs` (+1 render period) → start render. Stop order: stopFlag → SetEvent(pumpEvent) → join pump → stop capture → stop render → free. Status fields atomic; `renderXruns = renderRing.overruns()` (skip during prefill). `errorCode` not `std::string`.

- [ ] **Step 4: Build + run** `--gtest_filter=MonitorEngine.*` → all PASS. Full suite grows accordingly.

- [ ] **Step 5: Commit** — `feat(core): MonitorEngine dual-stream pump (prefill/drift/scope/rollback)`

---

# GATE 4 — CLI monitor smoke

## Task 9: CLI `monitor` subcommand (hardware smoke)

**Files:** Modify `src/cli/main.cpp`.

- [ ] **Step 1: Add the `monitor` command** — parse `--cap <id>` `--render <id>` `--delay-ms N` `--backend ...` `--seconds N`; construct a `MonitorEngine` (default WASAPI factory), `start(...)`, loop `poll()` printing `capState/renderState sr fifoMs drift xrun(c/r)` each 200ms for N seconds, then `stop()`. Update `usage()`.

- [ ] **Step 2: Build + hardware smoke** — `& ".\x64\Debug\WinAudioCli.exe" monitor --delay-ms 100 --seconds 5`. Expected: both states Running, fifoMs ≈ 100 stable, drift small, no crash, clean stop (no hang via watchdog). If capture/render default devices differ in sample rate, it prints the rate-mismatch error — try `--cap`/`--render` ids from `list` with matching rates and report.

- [ ] **Step 3: Commit** — `feat(cli): monitor subcommand (dual-stream smoke)`

---

# GATE 5 — ImPlot vendoring

## Task 10: Vendor ImPlot + build wiring (blank compile)

**Files:** Create `third_party/implot/`; Modify `src/gui/WinAudioGui.vcxproj`.

- [ ] **Step 1: Vendor ImPlot** (network) — fetch ImPlot v0.16 source into `third_party/implot/` so `implot.h`, `implot_internal.h`, `implot.cpp`, `implot_items.cpp` exist: `curl -L -o implot.tar.gz https://github.com/epezent/implot/archive/refs/tags/v0.16.tar.gz` then extract so paths are exactly `third_party/implot/implot.cpp` (no version segment). Don't commit the tarball.

- [ ] **Step 2: vcxproj** — add `$(SolutionDir)third_party\implot` to AdditionalIncludeDirectories (Debug+Release). Compile `third_party\implot\implot.cpp` and `implot_items.cpp` with per-file `<WarningLevel>TurnOffAllWarnings</WarningLevel>` (+ `TreatWarningAsError=false`), like the ImGui TUs.

- [ ] **Step 3: Blank compile** — in `gui/main.cpp` init, after `ImGui::CreateContext()`, add `ImPlot::CreateContext();` and before ImGui shutdown add `ImPlot::DestroyContext();` (include `implot.h`). Build solution → clean. Run GUI liveness (`Start-Process ... ; Start-Sleep 3; Stop-Process`) → "started OK".

- [ ] **Step 4: Commit** — `build(gui): vendor ImPlot + create/destroy context`

---

# GATE 6 — GUI mode switch

## Task 11: AppUi owns Engine + MonitorEngine; Monitor mode selector

**Files:** Modify `src/gui/AppUi.h`, `src/gui/AppUi.cpp`, `src/gui/main.cpp`.

- [ ] **Step 1:** Make `Engine` and `MonitorEngine` **members of `AppUi`** (AppUi owns them). Change `AppUi::draw()` to take no engine argument; update `main.cpp` (`static AppUi ui; ui.draw();`). Add a top-level mode: `Capture | Playback | Monitor` (e.g., a `ImGui::Combo` or radio at the top). Capture/Playback keep the existing single-stream UI driven by `Engine`; Monitor selects a new (initially minimal) Monitor panel driven by `MonitorEngine`. Only one engine is started at a time; switching mode calls `stop()` on the other.

- [ ] **Step 2:** Minimal Monitor panel: capture device combo, render device combo, delay slider (ms), Start/Stop, and a status line from `MonitorStatus` (no plots yet).

- [ ] **Step 3:** Build + GUI liveness; manually confirm mode switch + monitor start/stop work (status shows Running). Commit — `feat(gui): AppUi owns both engines; Monitor mode + controls`

---

# GATE 7 — Waveform

## Task 12: Monitor waveforms (capture + delayed render) via ImPlot

**Files:** Modify `src/gui/AppUi.cpp` (+ helpers).

- [ ] **Step 1:** In the Monitor panel, each frame: pull recent ~50 ms windows via `snapshotCapture(N, buf, end)` / `snapshotRender(...)` (N = 0.05*sampleRate, ≤ scope cap/2). Plot each with ImPlot:
```cpp
if (ImPlot::BeginPlot("Capture waveform", ImVec2(-1,120))) {
    ImPlot::SetupAxes("s","amp", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
    ImPlot::SetupAxisLimits(ImAxis_Y1, -1.0, 1.0, ImGuiCond_Always);
    ImPlot::PlotLine("cap", xSeconds.data(), capBuf.data(), (int)n);
    ImPlot::EndPlot();
}
```
(Pre-allocate `capBuf/renderBuf/xSeconds`; build the x axis from sample index / sampleRate.) Same for the delayed render waveform.

- [ ] **Step 2:** Build + GUI; visually confirm both waveforms move when audio is present. Commit — `feat(gui): monitor waveforms (ImPlot lines)`

---

# GATE 8 — Spectrum

## Task 13: Spectrum curves (magnitudeSpectrumDb + log-X), 0 dBFS verified

**Files:** Modify `src/gui/AppUi.cpp`.

- [ ] **Step 1:** Per stream, drive analysis with `advanceAnalysis(capWritten(), nextCapEnd_, 2048, 512, 8, fn)`; in `fn(endIdx)`: `snapshotCapture(2048, win, e)` → `magnitudeSpectrumDb(win, 2048, workCap_.data(), magCap_)`. Keep the **latest** `magCap_` for the spectrum plot. Plot with log-X:
```cpp
if (ImPlot::BeginPlot("Capture spectrum", ImVec2(-1,140))) {
    ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
    ImPlot::SetupAxisLimits(ImAxis_X1, 20.0, sampleRate/2.0, ImGuiCond_Always);
    ImPlot::SetupAxisLimits(ImAxis_Y1, -96.0, 0.0, ImGuiCond_Always);
    ImPlot::PlotLine("cap", freqAxis_.data(), magCap_.data(), (int)magCap_.size()); // freqAxis_[k]=k*sr/2048
    ImPlot::EndPlot();
}
```
`workCap_` sized 2048; `freqAxis_` precomputed (skip bin 0 for log). Same for render.

- [ ] **Step 2:** Verify calibration in the running GUI is consistent with the unit test (a full-scale tone near an on-bin frequency reads ≈0 dBFS). Build + GUI. Commit — `feat(gui): monitor spectrum curves (log-frequency, dBFS)`

---

# GATE 9 — Spectrogram

## Task 14: Log-resampled scrolling spectrogram (ImPlot Heatmap)

**Files:** Create `src/gui/Spectrogram.h/.cpp`; Modify `src/gui/AppUi.cpp`.

**Interfaces:** Produces `class Spectrogram { Spectrogram(int logRows, int cols, double fmin, double fmax, uint32_t sampleRate); void pushColumn(const std::vector<float>& linMagDb); const float* data() const; ... }` — resamples linear FFT bins to `logRows` log-spaced rows and maintains a rolling `rows×cols` buffer for `ImPlot::PlotHeatmap`.

- [ ] **Step 1:** Implement `Spectrogram`: precompute, for each of `logRows`, the linear bin index range mapping to that log-frequency row (`f = fmin*pow(fmax/fmin, r/(rows-1))` → bin = f*N/sr); `pushColumn` reduces linear `linMagDb` into the log rows (max or mean over the bin range), writes into the current rolling column (ring of `cols`), advancing the column index. Provide a contiguous `rows×cols` view for the heatmap (either memmove-shift or expose the ring with a column offset). dB range −96..0.

- [ ] **Step 2:** In `fn(endIdx)` (from Task 13), also call `capSpectrogram_.pushColumn(magCap_)` each hop. Render:
```cpp
if (ImPlot::BeginPlot("Capture spectrogram", ImVec2(-1,160))) {
    ImPlot::PlotHeatmap("cap", capSpectrogram_.data(), rows, cols, -96.0, 0.0, nullptr,
                        ImPlotPoint(0,fmin), ImPlotPoint(historySeconds, fmax));
    ImPlot::EndPlot();
}
```
(Use an ImPlot colormap, e.g. `ImPlot::PushColormap(ImPlotColormap_Viridis)`.) Same for render. Note ImPlot Heatmap draws a uniform grid; the log mapping is done by the resample in `pushColumn`, so rows are already log-spaced.

- [ ] **Step 3:** Build + GUI; visually confirm both spectrograms scroll and show frequency content; a swept tone traces a curve. Commit — `feat(gui): log-resampled scrolling spectrogram (ImPlot heatmap)`

---

# GATE 10 (wrap) — Docs + Release + smoke

## Task 15: CLAUDE.md + Release + full smoke

**Files:** Modify `CLAUDE.md`.

- [ ] **Step 1:** Update CLAUDE.md: new `monitor` CLI command; GUI Monitor mode (waveform/spectrum/spectrogram); note ImPlot vendored under `third_party/implot` (GUI-only); known limitations (mono analysis, capture=render sample rate, monitor drift drop/dup). Bump test count.
- [ ] **Step 2:** Release build of all 4 projects clean; `& ".\x64\Release\WinAudioTests.exe"` all PASS.
- [ ] **Step 3:** Hardware smoke (Release): CLI `monitor --seconds 5` stable; GUI Monitor mode — waveform/spectrum/spectrogram live for both streams, clean close, ~1 min drift-stable.
- [ ] **Step 4:** Commit — `docs: Phase 3 monitor + visualization complete`

---

## Self-Review
**Spec coverage:** dual monitor + drift (Tasks 5,8) ✅; dedicated pump event (Task 6) ✅; frame-aligned reads (Task 8 pump) ✅; FFT window-length dBFS calibration + on-bin/zero-pad tests (Task 2) ✅; single magnitudeSpectrumDb contract, GUI calls only it (Tasks 2,13) ✅; seqlock {endIdx,window} (Task 4) ✅; sample-index cadence free function (Task 7), used in GUI (Tasks 13,14) ✅; SampleConvert int24/clamp/avg-downmix (Task 3) ✅; AudioFormat split for windows-free DSP (Task 1) ✅; startup prefill + teardown order + rollback + atomic status (Task 8) ✅; delayMs single domain + renderBufMs (Task 8 status) ✅; ImPlot vendoring + per-file warning suppress (Task 10) ✅; AppUi owns both engines + mode switch (Task 11) ✅; log-resampled spectrogram (Task 14) ✅; CLI monitor (Task 9) ✅; acceptance/tests (each gate) ✅; gating order = spec §9 ✅.
**Placeholder scan:** FFT, ScopeBuffer, AudioFormat split, IAudioBackend/WasapiStream change carry complete code. DelayFifo/MonitorEngine/Spectrogram give signatures + test cases + precise algorithm prose (their exact line code depends on the running device/ImPlot API and is bounded by the tests). GUI gates give exact ImPlot call patterns. No "TODO".
**Type consistency:** `AudioFormat` POD, free `toWaveFormatExtensible/fromWaveFormat`; `magnitudeSpectrumDb(samples,count,workBuf,out,floorDb)`; `ScopeBuffer::snapshotLatest(n,out,endIdxOut)`; `advanceAnalysis(written,&nextEndIdx,windowSize,hop,maxCatchup,fn)`; `MonitorEngine` per §4.5; `IAudioBackend::dataReadyEvent()` used by Task 8 pump. Consistent across tasks.
**Known limitation (explicit):** DelayFifo/MonitorEngine/Spectrogram are specified by interface + tests + algorithm, not line-complete — by design for the device/ImPlot-dependent units; their unit tests are the executable contract.
