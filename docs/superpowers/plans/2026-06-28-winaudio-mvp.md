# WinAudio MVP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build WinAudio MVP — a Windows audio test tool that enumerates devices and, via a single WASAPI-Shared backend, captures audio to a `.wav` file and plays a `.wav` back, driven by a Dear ImGui GUI (primary) and a minimal CLI.

**Architecture:** A UI-agnostic static library `WinAudioCore` holds all logic: `AudioFormat`, a lock-free SPSC `RingBuffer`, WAV I/O, `DeviceEnumerator` (MMDevice), an `IAudioBackend` abstraction with WASAPI-Shared capture/render implementations (each owning its own I/O thread), and an `Engine` that wires backend ↔ ring ↔ WAV. Audio threads and UI threads never touch each other directly — the UI reads snapshots via `Engine::poll()`. Two front-ends link the Core: `WinAudioGui` (Dear ImGui + DX11) and `WinAudioCli`.

**Tech Stack:** C++17, MSBuild (`.sln` + `.vcxproj`), MSVC, x64. Win32 + WASAPI (`mmdeviceapi.h`, `audioclient.h`) + STL only in Core/Cli. Dear ImGui + Direct3D 11 in Gui. gtest in Tests.

## Global Constraints

- Build system: **MSBuild**, configurations Debug/Release, platform **x64** only.
- Language standard: **C++17** (`/std:c++17`), `/W4`, treat the Core as warning-clean.
- **WinAudioCore** and **WinAudioCli**: pure **Win32 + STL**, zero third-party deps. WAV read/write, CLI arg parsing, and logging are all hand-written.
- Third-party deps appear **only** in: `WinAudioGui` (Dear ImGui + DX11) and `WinAudioTests` (gtest).
- Vendored third-party source lives under `third_party/imgui/` (Gui only) and `third_party/googletest/` (Tests only).
- Namespace: all Core symbols in namespace `wa`.
- COM: every thread using WASAPI calls `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` and pairs it with `CoUninitialize`; all COM interfaces wrapped in RAII (`Microsoft::WRL::ComPtr` or equivalent) — no raw `Release()`.
- Core never calls `printf`/`std::cout` and never throws across the public API; operations return `wa::Result` (see Task 2).
- MVP backend is **WASAPI-Shared only**. waveIn/waveOut and WASAPI-Exclusive are out of scope for this plan (later phases).

---

## File Structure

```
WinAudio.sln
src/
  core/                       (WinAudioCore.vcxproj — static lib)
    Result.h                  Result{ok,code,message} + helpers
    AudioFormat.h/.cpp        PCM format + WAVEFORMATEXTENSIBLE conversion
    RingBuffer.h/.cpp         lock-free SPSC byte ring + xrun counters
    WavFile.h/.cpp            WavWriter / WavReader (RIFF/PCM)
    ComUtil.h                 ComInitGuard + ComPtr alias + HRESULT->Result
    DeviceEnumerator.h/.cpp   IMMDeviceEnumerator wrapper
    IAudioBackend.h           backend interface + DeviceId/DataFlow/BackendStats
    WasapiShared.h/.cpp       WasapiSharedCapture / WasapiSharedRender
    Engine.h/.cpp             orchestration + poll() snapshot
  cli/                        (WinAudioCli.vcxproj — console exe)
    main.cpp                  list / capture / play subcommands
  gui/                        (WinAudioGui.vcxproj — windows exe)
    main.cpp                  Win32 window + DX11 device + ImGui loop
    AppUi.h/.cpp              ImGui widgets bound to Engine
  tests/                      (WinAudioTests.vcxproj — console exe, gtest)
    test_ringbuffer.cpp
    test_audioformat.cpp
    test_wavfile.cpp
    main_gtest.cpp            (or gtest_main)
third_party/
  imgui/                      Dear ImGui source (Gui only)
  googletest/                 gtest source (Tests only)
docs/superpowers/...
```

---

## Task 1: Solution scaffold + Core lib + Tests project with one green gtest

**Files:**
- Create: `WinAudio.sln`
- Create: `src/core/WinAudioCore.vcxproj`
- Create: `src/core/Result.h`
- Create: `src/tests/WinAudioTests.vcxproj`
- Create: `src/tests/test_smoke.cpp`
- Create: `third_party/googletest/` (vendored gtest source)
- Create: `.gitignore`

**Interfaces:**
- Consumes: nothing.
- Produces: a buildable `WinAudio.sln` with `WinAudioCore` (static lib) and `WinAudioTests` (console exe linking gtest + Core) for x64 Debug/Release; `wa::Result` type.

- [ ] **Step 1: Vendor gtest**

Download googletest release source into `third_party/googletest/` so that `third_party/googletest/googletest/include/gtest/gtest.h` and `third_party/googletest/googletest/src/gtest-all.cc` exist. (Use the googletest 1.14.0 source tree.)

- [ ] **Step 2: Add `.gitignore`**

```gitignore
# Build output
[Bb]in/
[Oo]bj/
x64/
*.user
.vs/
# MSBuild
*.tlog
*.log
*.pdb
*.ilk
*.idb
```

- [ ] **Step 3: Create `src/core/Result.h`**

```cpp
#pragma once
#include <string>
#include <utility>

namespace wa {

// Core never throws across its public API and never prints. Operations that
// can fail return Result; UI layers turn it into log lines / stderr.
struct Result {
    bool        ok = true;
    long        code = 0;     // HRESULT or custom error code; 0 on success
    std::string message;

    static Result Ok() { return Result{true, 0, {}}; }
    static Result Fail(long code, std::string message) {
        return Result{false, code, std::move(message)};
    }

    explicit operator bool() const { return ok; }
};

} // namespace wa
```

- [ ] **Step 4: Create `src/core/WinAudioCore.vcxproj`**

A static library (`<ConfigurationType>StaticLibrary</ConfigurationType>`) targeting x64 Debug/Release, `LanguageStandard=stdcpp17`, `WarningLevel=Level4`, additional include dir `$(ProjectDir)`. Compile `Result.h` is header-only (no .cpp yet) — include at least one source file so the lib builds: temporarily add nothing else; instead make the first real `.cpp` (AudioFormat) appear in Task 2. For Task 1, add an empty translation unit `src/core/_placeholder.cpp` containing `// WinAudioCore` so the static lib has an object. (Remove `_placeholder.cpp` once AudioFormat.cpp lands in Task 2.)

- [ ] **Step 5: Create `src/tests/test_smoke.cpp`**

```cpp
#include <gtest/gtest.h>
#include "Result.h"

TEST(Smoke, ResultOkIsTruthy) {
    wa::Result r = wa::Result::Ok();
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.code, 0);
}

TEST(Smoke, ResultFailCarriesMessage) {
    wa::Result r = wa::Result::Fail(-1, "boom");
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(r.message, "boom");
}
```

- [ ] **Step 6: Create `src/tests/WinAudioTests.vcxproj`**

Console application (`Application`/`Subsystem=Console`), x64 Debug/Release, C++17. Additional include dirs: `$(SolutionDir)src/core`, `$(SolutionDir)third_party/googletest/googletest/include`, `$(SolutionDir)third_party/googletest/googletest`. Compile `test_smoke.cpp` and `third_party/googletest/googletest/src/gtest-all.cc` and `third_party/googletest/googletest/src/gtest_main.cc`. Add a project reference to `WinAudioCore.vcxproj`. Define `GTEST_HAS_PTHREAD=0`.

- [ ] **Step 7: Create `WinAudio.sln`**

Add both projects, x64 Debug/Release solution configurations mapped to project configs.

- [ ] **Step 8: Build and run the tests**

Run:
```powershell
msbuild WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m
x64\Debug\WinAudioTests.exe
```
Expected: build succeeds; test run prints `[  PASSED  ] 2 tests.`

- [ ] **Step 9: Commit**

```powershell
git add -A
git commit -m "build: scaffold solution, Core lib, gtest Tests project"
```

---

## Task 2: AudioFormat with WAVEFORMATEXTENSIBLE conversion

**Files:**
- Create: `src/core/AudioFormat.h`
- Create: `src/core/AudioFormat.cpp`
- Delete: `src/core/_placeholder.cpp`
- Test: `src/tests/test_audioformat.cpp`
- Modify: `src/core/WinAudioCore.vcxproj` (add AudioFormat.cpp, drop placeholder), `src/tests/WinAudioTests.vcxproj` (add test_audioformat.cpp)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct wa::AudioFormat { uint32_t sampleRate; uint16_t channels; uint16_t bitsPerSample; bool isFloat; }`
  - `uint32_t AudioFormat::blockAlign() const;` `uint32_t AudioFormat::avgBytesPerSec() const;`
  - `WAVEFORMATEXTENSIBLE AudioFormat::toWaveFormatExtensible() const;`
  - `static AudioFormat AudioFormat::fromWaveFormat(const WAVEFORMATEX* wf);`
  - `bool AudioFormat::operator==(const AudioFormat&) const;`

- [ ] **Step 1: Write the failing test**

`src/tests/test_audioformat.cpp`:
```cpp
#include <gtest/gtest.h>
#include <windows.h>
#include <mmreg.h>
#include "AudioFormat.h"

using wa::AudioFormat;

TEST(AudioFormat, DerivedFields) {
    AudioFormat f{48000, 2, 16, false};
    EXPECT_EQ(f.blockAlign(), 4u);          // 2ch * 2 bytes
    EXPECT_EQ(f.avgBytesPerSec(), 192000u); // 48000 * 4
}

TEST(AudioFormat, RoundTripPcm16) {
    AudioFormat f{44100, 2, 16, false};
    WAVEFORMATEXTENSIBLE w = f.toWaveFormatExtensible();
    AudioFormat back = AudioFormat::fromWaveFormat(
        reinterpret_cast<const WAVEFORMATEX*>(&w));
    EXPECT_TRUE(f == back);
}

TEST(AudioFormat, RoundTripFloat32) {
    AudioFormat f{48000, 2, 32, true};
    WAVEFORMATEXTENSIBLE w = f.toWaveFormatExtensible();
    EXPECT_EQ(w.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    AudioFormat back = AudioFormat::fromWaveFormat(
        reinterpret_cast<const WAVEFORMATEX*>(&w));
    EXPECT_TRUE(f == back);
}

TEST(AudioFormat, ParsePlainPcmWaveFormatEx) {
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = 16000;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = 32000;
    AudioFormat f = AudioFormat::fromWaveFormat(&wf);
    EXPECT_EQ(f.sampleRate, 16000u);
    EXPECT_EQ(f.channels, 1);
    EXPECT_EQ(f.bitsPerSample, 16);
    EXPECT_FALSE(f.isFloat);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `msbuild WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: FAIL — `AudioFormat.h` not found / unresolved symbols.

- [ ] **Step 3: Create `src/core/AudioFormat.h`**

```cpp
#pragma once
#include <cstdint>

struct tWAVEFORMATEX;          // forward-declare to keep windows.h out of header users that don't need it
typedef tWAVEFORMATEX WAVEFORMATEX;
struct tWAVEFORMATEXTENSIBLE;
typedef tWAVEFORMATEXTENSIBLE WAVEFORMATEXTENSIBLE;

namespace wa {

struct AudioFormat {
    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
    uint16_t bitsPerSample = 16;
    bool     isFloat = false;

    uint32_t blockAlign() const {
        return static_cast<uint32_t>(channels) * (bitsPerSample / 8u);
    }
    uint32_t avgBytesPerSec() const { return sampleRate * blockAlign(); }

    WAVEFORMATEXTENSIBLE toWaveFormatExtensible() const;
    static AudioFormat fromWaveFormat(const WAVEFORMATEX* wf);

    bool operator==(const AudioFormat& o) const {
        return sampleRate == o.sampleRate && channels == o.channels &&
               bitsPerSample == o.bitsPerSample && isFloat == o.isFloat;
    }
};

} // namespace wa
```

- [ ] **Step 4: Create `src/core/AudioFormat.cpp`**

```cpp
#include <windows.h>
#include <mmreg.h>
#include <ksmedia.h>
#include "AudioFormat.h"

namespace wa {

WAVEFORMATEXTENSIBLE AudioFormat::toWaveFormatExtensible() const {
    WAVEFORMATEXTENSIBLE w{};
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = channels;
    w.Format.nSamplesPerSec = sampleRate;
    w.Format.wBitsPerSample = bitsPerSample;
    w.Format.nBlockAlign = static_cast<WORD>(blockAlign());
    w.Format.nAvgBytesPerSec = avgBytesPerSec();
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = bitsPerSample;
    w.dwChannelMask = (channels == 1) ? SPEAKER_FRONT_CENTER
                                      : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    w.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                          : KSDATAFORMAT_SUBTYPE_PCM;
    return w;
}

AudioFormat AudioFormat::fromWaveFormat(const WAVEFORMATEX* wf) {
    AudioFormat f{};
    f.sampleRate = wf->nSamplesPerSec;
    f.channels = wf->nChannels;
    f.bitsPerSample = wf->wBitsPerSample;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        f.isFloat = true;
    } else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        f.isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else {
        f.isFloat = false; // WAVE_FORMAT_PCM
    }
    return f;
}

} // namespace wa
```

- [ ] **Step 5: Update projects**

Remove `src/core/_placeholder.cpp` from `WinAudioCore.vcxproj` and delete the file; add `AudioFormat.cpp` (and `AudioFormat.h`). Add `test_audioformat.cpp` to `WinAudioTests.vcxproj`.

- [ ] **Step 6: Build and run tests**

Run:
```powershell
msbuild WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m
x64\Debug\WinAudioTests.exe --gtest_filter=AudioFormat.*
```
Expected: PASS (4 tests).

- [ ] **Step 7: Commit**

```powershell
git add -A
git commit -m "feat(core): AudioFormat with WAVEFORMATEXTENSIBLE conversion"
```

---

## Task 3: Lock-free SPSC RingBuffer with xrun counters

**Files:**
- Create: `src/core/RingBuffer.h`
- Create: `src/core/RingBuffer.cpp`
- Test: `src/tests/test_ringbuffer.cpp`
- Modify: `src/core/WinAudioCore.vcxproj`, `src/tests/WinAudioTests.vcxproj`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `class wa::RingBuffer` with: `explicit RingBuffer(size_t capacityBytes);`
  - `size_t write(const void* data, size_t bytes);` (producer; returns bytes accepted)
  - `size_t read(void* out, size_t bytes);` (consumer; returns bytes delivered)
  - `size_t capacity() const; size_t availableRead() const; size_t availableWrite() const;`
  - `uint64_t overruns() const; uint64_t underruns() const; void reset();`

- [ ] **Step 1: Write the failing test**

`src/tests/test_ringbuffer.cpp`:
```cpp
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <numeric>
#include "RingBuffer.h"

using wa::RingBuffer;

TEST(RingBuffer, WriteThenReadRoundTrip) {
    RingBuffer rb(16);
    uint8_t in[4] = {1, 2, 3, 4};
    EXPECT_EQ(rb.write(in, 4), 4u);
    EXPECT_EQ(rb.availableRead(), 4u);
    uint8_t out[4] = {};
    EXPECT_EQ(rb.read(out, 4), 4u);
    EXPECT_EQ(0, memcmp(in, out, 4));
    EXPECT_EQ(rb.availableRead(), 0u);
}

TEST(RingBuffer, OverrunCountedWhenFull) {
    RingBuffer rb(4);
    uint8_t in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_EQ(rb.write(in, 8), 4u);     // only 4 fit
    EXPECT_EQ(rb.overruns(), 1u);
}

TEST(RingBuffer, UnderrunCountedWhenEmpty) {
    RingBuffer rb(4);
    uint8_t out[4] = {};
    EXPECT_EQ(rb.read(out, 4), 0u);
    EXPECT_EQ(rb.underruns(), 1u);
}

TEST(RingBuffer, WrapAround) {
    RingBuffer rb(8);
    uint8_t a[6] = {1,2,3,4,5,6};
    uint8_t tmp[6] = {};
    rb.write(a, 6);
    rb.read(tmp, 6);            // advance past the middle
    uint8_t b[6] = {7,8,9,10,11,12};
    EXPECT_EQ(rb.write(b, 6), 6u);  // must wrap
    uint8_t out[6] = {};
    EXPECT_EQ(rb.read(out, 6), 6u);
    EXPECT_EQ(0, memcmp(b, out, 6));
}

TEST(RingBuffer, SpscStress) {
    const size_t N = 1'000'000;
    RingBuffer rb(1024);
    std::thread producer([&] {
        size_t written = 0;
        uint8_t v = 0;
        while (written < N) {
            uint8_t byte = static_cast<uint8_t>(written & 0xFF);
            if (rb.write(&byte, 1) == 1) ++written;
        }
    });
    size_t read = 0;
    while (read < N) {
        uint8_t byte;
        if (rb.read(&byte, 1) == 1) {
            ASSERT_EQ(byte, static_cast<uint8_t>(read & 0xFF));
            ++read;
        }
    }
    producer.join();
    EXPECT_EQ(read, N);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `msbuild WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: FAIL — `RingBuffer.h` not found.

- [ ] **Step 3: Create `src/core/RingBuffer.h`**

```cpp
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wa {

// Single-producer / single-consumer lock-free byte ring buffer.
// write() is called from exactly one (audio) thread; read() from exactly one
// (engine) thread. Partial write increments overruns; partial read increments
// underruns. These counters feed the later latency/glitch phase.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacityBytes);

    size_t write(const void* data, size_t bytes);
    size_t read(void* out, size_t bytes);

    size_t capacity() const { return buf_.size(); }
    size_t availableRead() const;
    size_t availableWrite() const;

    uint64_t overruns() const { return overruns_.load(std::memory_order_relaxed); }
    uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

    void reset();

private:
    std::vector<uint8_t>  buf_;
    std::atomic<size_t>   head_{0};   // monotonic total bytes written
    std::atomic<size_t>   tail_{0};   // monotonic total bytes read
    std::atomic<uint64_t> overruns_{0};
    std::atomic<uint64_t> underruns_{0};
};

} // namespace wa
```

- [ ] **Step 4: Create `src/core/RingBuffer.cpp`**

```cpp
#include "RingBuffer.h"
#include <algorithm>
#include <cstring>

namespace wa {

RingBuffer::RingBuffer(size_t capacityBytes)
    : buf_(capacityBytes == 0 ? 1 : capacityBytes) {}

size_t RingBuffer::availableRead() const {
    return head_.load(std::memory_order_acquire) -
           tail_.load(std::memory_order_acquire);
}

size_t RingBuffer::availableWrite() const {
    return buf_.size() - availableRead();
}

size_t RingBuffer::write(const void* data, size_t bytes) {
    const size_t h = head_.load(std::memory_order_relaxed);
    const size_t t = tail_.load(std::memory_order_acquire);
    const size_t freeBytes = buf_.size() - (h - t);
    const size_t n = std::min(bytes, freeBytes);
    if (n < bytes) overruns_.fetch_add(1, std::memory_order_relaxed);

    const size_t cap = buf_.size();
    const size_t off = h % cap;
    const size_t first = std::min(n, cap - off);
    std::memcpy(buf_.data() + off, data, first);
    if (n > first)
        std::memcpy(buf_.data(), static_cast<const uint8_t*>(data) + first, n - first);

    head_.store(h + n, std::memory_order_release);
    return n;
}

size_t RingBuffer::read(void* out, size_t bytes) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    const size_t h = head_.load(std::memory_order_acquire);
    const size_t avail = h - t;
    const size_t n = std::min(bytes, avail);
    if (n < bytes) underruns_.fetch_add(1, std::memory_order_relaxed);

    const size_t cap = buf_.size();
    const size_t off = t % cap;
    const size_t first = std::min(n, cap - off);
    std::memcpy(out, buf_.data() + off, first);
    if (n > first)
        std::memcpy(static_cast<uint8_t*>(out) + first, buf_.data(), n - first);

    tail_.store(t + n, std::memory_order_release);
    return n;
}

void RingBuffer::reset() {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    overruns_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
}

} // namespace wa
```

- [ ] **Step 5: Add files to projects**

Add `RingBuffer.cpp`/`.h` to `WinAudioCore.vcxproj`; add `test_ringbuffer.cpp` to `WinAudioTests.vcxproj`.

- [ ] **Step 6: Build and run tests**

Run:
```powershell
msbuild WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m
x64\Debug\WinAudioTests.exe --gtest_filter=RingBuffer.*
```
Expected: PASS (5 tests). The `SpscStress` test confirms lock-free correctness across threads.

- [ ] **Step 7: Commit**

```powershell
git add -A
git commit -m "feat(core): lock-free SPSC RingBuffer with xrun counters"
```

---

## Task 4: WAV file reader/writer (RIFF/PCM)

**Files:**
- Create: `src/core/WavFile.h`
- Create: `src/core/WavFile.cpp`
- Test: `src/tests/test_wavfile.cpp`
- Modify: `src/core/WinAudioCore.vcxproj`, `src/tests/WinAudioTests.vcxproj`

**Interfaces:**
- Consumes: `wa::AudioFormat`, `wa::Result`.
- Produces:
  - `class wa::WavWriter { Result open(const std::wstring& path, const AudioFormat&); size_t write(const void* data, size_t bytes); Result close(); ~WavWriter(); }`
  - `class wa::WavReader { Result open(const std::wstring& path); const AudioFormat& format() const; size_t read(void* out, size_t bytes); bool eof() const; Result close(); ~WavReader(); }`
  - Writer patches RIFF/data chunk sizes on `close()`.

- [ ] **Step 1: Write the failing test**

`src/tests/test_wavfile.cpp`:
```cpp
#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include "WavFile.h"
#include "AudioFormat.h"

using wa::WavWriter;
using wa::WavReader;
using wa::AudioFormat;

static std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

TEST(WavFile, RoundTrip16BitStereo) {
    AudioFormat fmt{48000, 2, 16, false};
    std::vector<int16_t> samples(480 * 2);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(i - 100);

    std::wstring path = TempPath(L"wa_roundtrip.wav");
    {
        WavWriter w;
        ASSERT_TRUE(w.open(path, fmt));
        ASSERT_EQ(w.write(samples.data(), samples.size() * 2),
                  samples.size() * 2);
        ASSERT_TRUE(w.close());
    }
    {
        WavReader r;
        ASSERT_TRUE(r.open(path));
        EXPECT_TRUE(r.format() == fmt);
        std::vector<int16_t> back(samples.size());
        EXPECT_EQ(r.read(back.data(), back.size() * 2), back.size() * 2);
        EXPECT_EQ(samples, back);
        uint8_t extra;
        EXPECT_EQ(r.read(&extra, 1), 0u);
        EXPECT_TRUE(r.eof());
    }
    _wremove(path.c_str());
}

TEST(WavFile, OpenMissingFileFails) {
    WavReader r;
    EXPECT_FALSE(r.open(L"Z:\\does\\not\\exist_xyz.wav"));
}

TEST(WavFile, RejectsTruncatedHeader) {
    std::wstring path = TempPath(L"wa_trunc.wav");
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    ASSERT_NE(f, nullptr);
    const char junk[8] = {'R','I','F','F', 1, 0, 0, 0};
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);

    WavReader r;
    EXPECT_FALSE(r.open(path));
    _wremove(path.c_str());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `msbuild WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: FAIL — `WavFile.h` not found.

- [ ] **Step 3: Create `src/core/WavFile.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include "AudioFormat.h"
#include "Result.h"

namespace wa {

class WavWriter {
public:
    ~WavWriter();
    Result open(const std::wstring& path, const AudioFormat& fmt);
    size_t write(const void* data, size_t bytes); // returns bytes written
    Result close();                               // patches sizes
private:
    FILE*  file_ = nullptr;
    uint32_t dataBytes_ = 0;
    AudioFormat fmt_{};
};

class WavReader {
public:
    ~WavReader();
    Result open(const std::wstring& path);
    const AudioFormat& format() const { return fmt_; }
    size_t read(void* out, size_t bytes);  // returns bytes read (0 at EOF)
    bool   eof() const { return remaining_ == 0; }
    Result close();
private:
    FILE*  file_ = nullptr;
    uint32_t remaining_ = 0;   // bytes left in data chunk
    AudioFormat fmt_{};
};

} // namespace wa
```

- [ ] **Step 4: Create `src/core/WavFile.cpp`**

```cpp
#include "WavFile.h"
#include <cstring>

namespace wa {

namespace {
struct FourCC { char c[4]; };
bool eq(const char a[4], const char* b) { return std::memcmp(a, b, 4) == 0; }
template <typename T> bool readPod(FILE* f, T& v) {
    return std::fread(&v, sizeof(T), 1, f) == 1;
}
}

WavWriter::~WavWriter() { close(); }

Result WavWriter::open(const std::wstring& path, const AudioFormat& fmt) {
    if (file_) close();
    fmt_ = fmt;
    dataBytes_ = 0;
    if (_wfopen_s(&file_, path.c_str(), L"wb") != 0 || !file_)
        return Result::Fail(-1, "WavWriter: cannot open file for write");

    // Reserve header; sizes patched on close().
    const uint32_t zero = 0;
    const uint16_t fmtTag = fmt.isFloat ? 3 /*IEEE_FLOAT*/ : 1 /*PCM*/;
    const uint16_t ch = fmt.channels;
    const uint32_t sr = fmt.sampleRate;
    const uint16_t bps = fmt.bitsPerSample;
    const uint16_t blockAlign = static_cast<uint16_t>(fmt.blockAlign());
    const uint32_t avg = fmt.avgBytesPerSec();
    const uint32_t fmtChunkSize = 16;

    std::fwrite("RIFF", 1, 4, file_);
    std::fwrite(&zero, 4, 1, file_);          // RIFF size (patched)
    std::fwrite("WAVE", 1, 4, file_);
    std::fwrite("fmt ", 1, 4, file_);
    std::fwrite(&fmtChunkSize, 4, 1, file_);
    std::fwrite(&fmtTag, 2, 1, file_);
    std::fwrite(&ch, 2, 1, file_);
    std::fwrite(&sr, 4, 1, file_);
    std::fwrite(&avg, 4, 1, file_);
    std::fwrite(&blockAlign, 2, 1, file_);
    std::fwrite(&bps, 2, 1, file_);
    std::fwrite("data", 1, 4, file_);
    std::fwrite(&zero, 4, 1, file_);          // data size (patched)
    return Result::Ok();
}

size_t WavWriter::write(const void* data, size_t bytes) {
    if (!file_) return 0;
    size_t n = std::fwrite(data, 1, bytes, file_);
    dataBytes_ += static_cast<uint32_t>(n);
    return n;
}

Result WavWriter::close() {
    if (!file_) return Result::Ok();
    std::fflush(file_);
    const uint32_t riffSize = 36 + dataBytes_;
    std::fseek(file_, 4, SEEK_SET);  std::fwrite(&riffSize, 4, 1, file_);
    std::fseek(file_, 40, SEEK_SET); std::fwrite(&dataBytes_, 4, 1, file_);
    std::fclose(file_);
    file_ = nullptr;
    return Result::Ok();
}

WavReader::~WavReader() { close(); }

Result WavReader::open(const std::wstring& path) {
    if (file_) close();
    if (_wfopen_s(&file_, path.c_str(), L"rb") != 0 || !file_)
        return Result::Fail(-1, "WavReader: cannot open file for read");

    char tag[4]; uint32_t riffSize;
    if (std::fread(tag, 1, 4, file_) != 4 || !eq(tag, "RIFF") ||
        !readPod(file_, riffSize) ||
        std::fread(tag, 1, 4, file_) != 4 || !eq(tag, "WAVE")) {
        close();
        return Result::Fail(-1, "WavReader: not a RIFF/WAVE file");
    }

    bool haveFmt = false, haveData = false;
    while (!haveData) {
        char id[4]; uint32_t sz;
        if (std::fread(id, 1, 4, file_) != 4 || !readPod(file_, sz)) {
            close();
            return Result::Fail(-1, "WavReader: truncated or missing data chunk");
        }
        if (eq(id, "fmt ")) {
            uint16_t fmtTag, ch, blockAlign, bps; uint32_t sr, avg;
            if (!readPod(file_, fmtTag) || !readPod(file_, ch) ||
                !readPod(file_, sr) || !readPod(file_, avg) ||
                !readPod(file_, blockAlign) || !readPod(file_, bps)) {
                close();
                return Result::Fail(-1, "WavReader: bad fmt chunk");
            }
            fmt_.channels = ch;
            fmt_.sampleRate = sr;
            fmt_.bitsPerSample = bps;
            fmt_.isFloat = (fmtTag == 3);
            // skip any extra fmt bytes
            if (sz > 16) std::fseek(file_, static_cast<long>(sz - 16), SEEK_CUR);
            haveFmt = true;
        } else if (eq(id, "data")) {
            remaining_ = sz;
            haveData = true;
        } else {
            std::fseek(file_, static_cast<long>(sz + (sz & 1)), SEEK_CUR); // word-align
        }
    }
    if (!haveFmt) { close(); return Result::Fail(-1, "WavReader: missing fmt chunk"); }
    return Result::Ok();
}

size_t WavReader::read(void* out, size_t bytes) {
    if (!file_ || remaining_ == 0) return 0;
    size_t want = bytes < remaining_ ? bytes : remaining_;
    size_t n = std::fread(out, 1, want, file_);
    remaining_ -= static_cast<uint32_t>(n);
    return n;
}

Result WavReader::close() {
    if (file_) { std::fclose(file_); file_ = nullptr; }
    remaining_ = 0;
    return Result::Ok();
}

} // namespace wa
```

- [ ] **Step 5: Add files to projects**

Add `WavFile.cpp`/`.h` to Core; add `test_wavfile.cpp` to Tests. Ensure `windows.h` is available for `GetTempPathW` in the test (include `<windows.h>` at the top of the test file).

- [ ] **Step 6: Build and run tests**

Run:
```powershell
msbuild WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m
x64\Debug\WinAudioTests.exe --gtest_filter=WavFile.*
```
Expected: PASS (3 tests).

- [ ] **Step 7: Commit**

```powershell
git add -A
git commit -m "feat(core): WAV reader/writer with round-trip + truncation handling"
```

---

## Task 5: COM utilities + DeviceEnumerator

**Files:**
- Create: `src/core/ComUtil.h`
- Create: `src/core/IAudioBackend.h`
- Create: `src/core/DeviceEnumerator.h`
- Create: `src/core/DeviceEnumerator.cpp`
- Modify: `src/core/WinAudioCore.vcxproj`, `src/cli/...` (CLI added in Task 8)

**Interfaces:**
- Consumes: `wa::AudioFormat`, `wa::Result`.
- Produces:
  - `src/core/ComUtil.h`: `using Microsoft::WRL::ComPtr;` alias `template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;` in namespace `wa`; `struct ComInitGuard { ComInitGuard(); ~ComInitGuard(); bool ok() const; };` (calls `CoInitializeEx(nullptr, COINIT_MULTITHREADED)`); `Result HrToResult(long hr, const char* where);`
  - `src/core/IAudioBackend.h`: `enum class DataFlow { Capture, Render };` `using DeviceId = std::wstring;` (empty = default endpoint); `struct DeviceInfo { DeviceId id; std::wstring name; DataFlow flow; bool isDefault; AudioFormat mixFormat; };` `struct BackendStats { AudioFormat actualFormat; uint32_t bufferFrames; uint64_t overruns; uint64_t underruns; };` `class IAudioBackend { virtual ~IAudioBackend(); virtual Result open(const DeviceId&, const AudioFormat&, RingBuffer*) = 0; virtual Result start() = 0; virtual void stop() = 0; virtual void close() = 0; virtual BackendStats stats() const = 0; };`
  - `class wa::DeviceEnumerator { Result enumerate(DataFlow, std::vector<DeviceInfo>& out); Result defaultDevice(DataFlow, DeviceInfo& out); Result mixFormat(const DeviceId&, AudioFormat& out); }`

- [ ] **Step 1: Create `src/core/ComUtil.h`**

```cpp
#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <string>
#include "Result.h"

namespace wa {

template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

struct ComInitGuard {
    HRESULT hr;
    ComInitGuard() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInitGuard() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

inline Result HrToResult(HRESULT hr, const char* where) {
    if (SUCCEEDED(hr)) return Result::Ok();
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s failed: hr=0x%08lX", where,
                static_cast<unsigned long>(hr));
    return Result::Fail(static_cast<long>(hr), buf);
}

} // namespace wa
```

- [ ] **Step 2: Create `src/core/IAudioBackend.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "AudioFormat.h"
#include "Result.h"

namespace wa {

class RingBuffer;

enum class DataFlow { Capture, Render };
using DeviceId = std::wstring;     // MMDevice id; empty string = default endpoint

struct DeviceInfo {
    DeviceId     id;
    std::wstring name;
    DataFlow     flow = DataFlow::Render;
    bool         isDefault = false;
    AudioFormat  mixFormat{};       // shared-mode mix format
};

struct BackendStats {
    AudioFormat actualFormat{};
    uint32_t    bufferFrames = 0;
    uint64_t    overruns = 0;
    uint64_t    underruns = 0;
};

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    virtual Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring) = 0;
    virtual Result start() = 0;
    virtual void   stop() = 0;
    virtual void   close() = 0;
    virtual BackendStats stats() const = 0;
};

} // namespace wa
```

- [ ] **Step 3: Create `src/core/DeviceEnumerator.h`**

```cpp
#pragma once
#include <vector>
#include "IAudioBackend.h"
#include "Result.h"

namespace wa {

class DeviceEnumerator {
public:
    Result enumerate(DataFlow flow, std::vector<DeviceInfo>& out);
    Result defaultDevice(DataFlow flow, DeviceInfo& out);
    Result mixFormat(const DeviceId& id, AudioFormat& out); // empty id = default render
};

} // namespace wa
```

- [ ] **Step 4: Create `src/core/DeviceEnumerator.cpp`**

```cpp
#include "DeviceEnumerator.h"
#include "ComUtil.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

namespace wa {

namespace {
EDataFlow toEDataFlow(DataFlow f) { return f == DataFlow::Capture ? eCapture : eRender; }

Result readInfo(IMMDevice* dev, DataFlow flow, bool isDefault, DeviceInfo& info) {
    LPWSTR idStr = nullptr;
    HRESULT hr = dev->GetId(&idStr);
    if (FAILED(hr)) return HrToResult(hr, "IMMDevice::GetId");
    info.id = idStr;
    CoTaskMemFree(idStr);
    info.flow = flow;
    info.isDefault = isDefault;

    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT name; PropVariantInit(&name);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR)
            info.name = name.pwszVal;
        PropVariantClear(&name);
    }

    ComPtr<IAudioClient> client;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client.GetAddressOf())))) {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
            info.mixFormat = AudioFormat::fromWaveFormat(mix);
            CoTaskMemFree(mix);
        }
    }
    return Result::Ok();
}
} // namespace

Result DeviceEnumerator::enumerate(DataFlow flow, std::vector<DeviceInfo>& out) {
    out.clear();
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "CoCreateInstance(MMDeviceEnumerator)");

    DeviceId defaultId;
    {
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(e->GetDefaultAudioEndpoint(toEDataFlow(flow), eConsole, &def))) {
            LPWSTR s = nullptr;
            if (SUCCEEDED(def->GetId(&s))) { defaultId = s; CoTaskMemFree(s); }
        }
    }

    ComPtr<IMMDeviceCollection> coll;
    hr = e->EnumAudioEndpoints(toEDataFlow(flow), DEVICE_STATE_ACTIVE, &coll);
    if (FAILED(hr)) return HrToResult(hr, "EnumAudioEndpoints");

    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;
        DeviceInfo info{};
        if (readInfo(dev.Get(), flow, false, info)) {
            info.isDefault = (info.id == defaultId);
            out.push_back(std::move(info));
        }
    }
    return Result::Ok();
}

Result DeviceEnumerator::defaultDevice(DataFlow flow, DeviceInfo& out) {
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "CoCreateInstance(MMDeviceEnumerator)");
    ComPtr<IMMDevice> dev;
    hr = e->GetDefaultAudioEndpoint(toEDataFlow(flow), eConsole, &dev);
    if (FAILED(hr)) return HrToResult(hr, "GetDefaultAudioEndpoint");
    return readInfo(dev.Get(), flow, true, out);
}

Result DeviceEnumerator::mixFormat(const DeviceId& id, AudioFormat& out) {
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "CoCreateInstance(MMDeviceEnumerator)");
    ComPtr<IMMDevice> dev;
    if (id.empty())
        hr = e->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    else
        hr = e->GetDevice(id.c_str(), &dev);
    if (FAILED(hr)) return HrToResult(hr, "GetDevice");

    ComPtr<IAudioClient> client;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                       reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "IMMDevice::Activate");
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) return HrToResult(hr, "GetMixFormat");
    out = AudioFormat::fromWaveFormat(mix);
    CoTaskMemFree(mix);
    return Result::Ok();
}

} // namespace wa
```

- [ ] **Step 5: Add files to Core project**

Add `ComUtil.h`, `IAudioBackend.h`, `DeviceEnumerator.h/.cpp` to `WinAudioCore.vcxproj`. Add `ole32.lib`, `mmdevapi.lib` to the eventual exe linkers (Core is a static lib; the linker libs are added on the Cli/Gui projects). Note: no unit test here — enumeration needs real devices; it is verified manually via the CLI in Task 8.

- [ ] **Step 6: Build to confirm it compiles**

Run: `msbuild src/core/WinAudioCore.vcxproj /p:Configuration=Debug /p:Platform=x64 /m`
Expected: builds clean.

- [ ] **Step 7: Commit**

```powershell
git add -A
git commit -m "feat(core): COM utils, backend interface, MMDevice enumerator"
```

---

## Task 6: WASAPI-Shared capture backend (own thread → ring)

**Files:**
- Create: `src/core/WasapiShared.h`
- Create: `src/core/WasapiShared.cpp` (capture portion; render added in Task 7)
- Modify: `src/core/WinAudioCore.vcxproj`

**Interfaces:**
- Consumes: `IAudioBackend`, `RingBuffer`, `AudioFormat`, `ComUtil`, `DeviceId`.
- Produces: `class wa::WasapiSharedCapture : public IAudioBackend` — event-driven, runs its own thread, writes captured PCM bytes into the supplied `RingBuffer`. `actualFormat` in `stats()` is the device mix format (shared mode ignores the requested format and uses the mix format).

- [ ] **Step 1: Create `src/core/WasapiShared.h`**

```cpp
#pragma once
#include <atomic>
#include <thread>
#include "IAudioBackend.h"
#include "ComUtil.h"

struct IAudioClient;
struct IAudioCaptureClient;
struct IAudioRenderClient;

namespace wa {

class RingBuffer;

class WasapiSharedCapture : public IAudioBackend {
public:
    ~WasapiSharedCapture() override;
    Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring) override;
    Result start() override;
    void   stop() override;
    void   close() override;
    BackendStats stats() const override;
private:
    void threadMain();

    RingBuffer* ring_ = nullptr;
    AudioFormat actualFormat_{};
    uint32_t    bufferFrames_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    DeviceId    deviceId_;
    void*       hEvent_ = nullptr;       // HANDLE
    ComPtr<IAudioClient>        client_;
    ComPtr<IAudioCaptureClient> capture_;
};

} // namespace wa
```

- [ ] **Step 2: Create `src/core/WasapiShared.cpp` (capture)**

```cpp
#include "WasapiShared.h"
#include "RingBuffer.h"
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace wa {

WasapiSharedCapture::~WasapiSharedCapture() { close(); }

Result WasapiSharedCapture::open(const DeviceId& id, const AudioFormat& /*fmt*/,
                                 RingBuffer* ring) {
    deviceId_ = id;
    ring_ = ring;
    return Result::Ok(); // real activation happens on the worker thread (its own COM apt)
}

Result WasapiSharedCapture::start() {
    if (running_.exchange(true)) return Result::Ok();
    hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread(&WasapiSharedCapture::threadMain, this);
    return Result::Ok();
}

void WasapiSharedCapture::stop() {
    running_.store(false);
    if (hEvent_) SetEvent(static_cast<HANDLE>(hEvent_));
    if (thread_.joinable()) thread_.join();
}

void WasapiSharedCapture::close() {
    stop();
    if (hEvent_) { CloseHandle(static_cast<HANDLE>(hEvent_)); hEvent_ = nullptr; }
    capture_.Reset();
    client_.Reset();
}

BackendStats WasapiSharedCapture::stats() const {
    BackendStats s{};
    s.actualFormat = actualFormat_;
    s.bufferFrames = bufferFrames_;
    if (ring_) { s.overruns = ring_->overruns(); s.underruns = ring_->underruns(); }
    return s;
}

void WasapiSharedCapture::threadMain() {
    ComInitGuard com; // this thread's own MTA apartment

    ComPtr<IMMDeviceEnumerator> e;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(e.GetAddressOf())))) { running_ = false; return; }

    ComPtr<IMMDevice> dev;
    HRESULT hr = deviceId_.empty()
        ? e->GetDefaultAudioEndpoint(eCapture, eConsole, &dev)
        : e->GetDevice(deviceId_.c_str(), &dev);
    if (FAILED(hr)) { running_ = false; return; }

    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client_.GetAddressOf())))) { running_ = false; return; }

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client_->GetMixFormat(&mix)) || !mix) { running_ = false; return; }
    actualFormat_ = AudioFormat::fromWaveFormat(mix);
    const uint32_t frameBytes = actualFormat_.blockAlign();

    REFERENCE_TIME dur = 10'000'000 / 10; // 100 ms buffer
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) { running_ = false; return; }

    client_->GetBufferSize(&bufferFrames_);
    client_->SetEventHandle(static_cast<HANDLE>(hEvent_));
    if (FAILED(client_->GetService(__uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(capture_.GetAddressOf())))) { running_ = false; return; }

    client_->Start();
    while (running_.load()) {
        WaitForSingleObject(static_cast<HANDLE>(hEvent_), 200);
        UINT32 packet = 0;
        while (SUCCEEDED(capture_->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                break;
            const size_t bytes = static_cast<size_t>(frames) * frameBytes;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                static thread_local std::vector<uint8_t> zeros;
                zeros.assign(bytes, 0);
                ring_->write(zeros.data(), bytes);
            } else if (data) {
                ring_->write(data, bytes);
            }
            capture_->ReleaseBuffer(frames);
        }
    }
    client_->Stop();
}

} // namespace wa
```

- [ ] **Step 3: Add files to Core project**

Add `WasapiShared.h/.cpp` to `WinAudioCore.vcxproj`. Add `#include <vector>` to the .cpp.

- [ ] **Step 4: Build to confirm it compiles**

Run: `msbuild src/core/WinAudioCore.vcxproj /p:Configuration=Debug /p:Platform=x64 /m`
Expected: builds clean. (Functional verification happens through the Engine + CLI in Task 8.)

- [ ] **Step 5: Commit**

```powershell
git add -A
git commit -m "feat(core): WASAPI-Shared capture backend (event-driven, own thread)"
```

---

## Task 7: WASAPI-Shared render backend (ring → device)

**Files:**
- Modify: `src/core/WasapiShared.h` (add `WasapiSharedRender`)
- Modify: `src/core/WasapiShared.cpp` (add render implementation)

**Interfaces:**
- Consumes: same as Task 6.
- Produces: `class wa::WasapiSharedRender : public IAudioBackend` — event-driven render; pulls PCM bytes from the supplied `RingBuffer` and writes them to the device; writes silence on underrun. `actualFormat` is the mix format.

- [ ] **Step 1: Add `WasapiSharedRender` to `WasapiShared.h`**

```cpp
class WasapiSharedRender : public IAudioBackend {
public:
    ~WasapiSharedRender() override;
    Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring) override;
    Result start() override;
    void   stop() override;
    void   close() override;
    BackendStats stats() const override;
private:
    void threadMain();
    RingBuffer* ring_ = nullptr;
    AudioFormat actualFormat_{};
    uint32_t    bufferFrames_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    DeviceId    deviceId_;
    void*       hEvent_ = nullptr;
    ComPtr<IAudioClient>       client_;
    ComPtr<IAudioRenderClient> render_;
};
```

- [ ] **Step 2: Add render implementation to `WasapiShared.cpp`**

```cpp
WasapiSharedRender::~WasapiSharedRender() { close(); }

Result WasapiSharedRender::open(const DeviceId& id, const AudioFormat& /*fmt*/,
                                RingBuffer* ring) {
    deviceId_ = id; ring_ = ring; return Result::Ok();
}

Result WasapiSharedRender::start() {
    if (running_.exchange(true)) return Result::Ok();
    hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread(&WasapiSharedRender::threadMain, this);
    return Result::Ok();
}

void WasapiSharedRender::stop() {
    running_.store(false);
    if (hEvent_) SetEvent(static_cast<HANDLE>(hEvent_));
    if (thread_.joinable()) thread_.join();
}

void WasapiSharedRender::close() {
    stop();
    if (hEvent_) { CloseHandle(static_cast<HANDLE>(hEvent_)); hEvent_ = nullptr; }
    render_.Reset(); client_.Reset();
}

BackendStats WasapiSharedRender::stats() const {
    BackendStats s{};
    s.actualFormat = actualFormat_;
    s.bufferFrames = bufferFrames_;
    if (ring_) { s.overruns = ring_->overruns(); s.underruns = ring_->underruns(); }
    return s;
}

void WasapiSharedRender::threadMain() {
    ComInitGuard com;
    ComPtr<IMMDeviceEnumerator> e;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(e.GetAddressOf())))) { running_ = false; return; }
    ComPtr<IMMDevice> dev;
    HRESULT hr = deviceId_.empty()
        ? e->GetDefaultAudioEndpoint(eRender, eConsole, &dev)
        : e->GetDevice(deviceId_.c_str(), &dev);
    if (FAILED(hr)) { running_ = false; return; }
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client_.GetAddressOf())))) { running_ = false; return; }

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client_->GetMixFormat(&mix)) || !mix) { running_ = false; return; }
    actualFormat_ = AudioFormat::fromWaveFormat(mix);
    const uint32_t frameBytes = actualFormat_.blockAlign();

    REFERENCE_TIME dur = 10'000'000 / 10;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) { running_ = false; return; }

    client_->GetBufferSize(&bufferFrames_);
    client_->SetEventHandle(static_cast<HANDLE>(hEvent_));
    if (FAILED(client_->GetService(__uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(render_.GetAddressOf())))) { running_ = false; return; }

    // Pre-roll one buffer of silence so the stream starts cleanly.
    BYTE* buf = nullptr;
    if (SUCCEEDED(render_->GetBuffer(bufferFrames_, &buf)))
        render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);

    client_->Start();
    std::vector<uint8_t> scratch;
    while (running_.load()) {
        WaitForSingleObject(static_cast<HANDLE>(hEvent_), 200);
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) break;
        UINT32 frames = bufferFrames_ - padding;
        if (frames == 0) continue;
        if (FAILED(render_->GetBuffer(frames, &buf))) break;
        const size_t want = static_cast<size_t>(frames) * frameBytes;
        scratch.resize(want);
        size_t got = ring_->read(scratch.data(), want);
        std::memcpy(buf, scratch.data(), got);
        if (got < want) std::memset(buf + got, 0, want - got); // underrun -> silence
        render_->ReleaseBuffer(frames, 0);
    }
    client_->Stop();
}
```

- [ ] **Step 3: Ensure `<cstring>` is included in `WasapiShared.cpp`**

Add `#include <cstring>` near the top if not present (for `memcpy`/`memset`).

- [ ] **Step 4: Build to confirm it compiles**

Run: `msbuild src/core/WinAudioCore.vcxproj /p:Configuration=Debug /p:Platform=x64 /m`
Expected: builds clean.

- [ ] **Step 5: Commit**

```powershell
git add -A
git commit -m "feat(core): WASAPI-Shared render backend (ring -> device)"
```

---

## Task 8: Engine + minimal CLI (first end-to-end verification)

**Files:**
- Create: `src/core/Engine.h`
- Create: `src/core/Engine.cpp`
- Create: `src/cli/main.cpp`
- Create: `src/cli/WinAudioCli.vcxproj`
- Modify: `WinAudio.sln` (add Cli project)

**Interfaces:**
- Consumes: `DeviceEnumerator`, `WasapiSharedCapture`, `WasapiSharedRender`, `WavWriter`, `WavReader`, `RingBuffer`, `IAudioBackend`.
- Produces:
  - `enum class wa::EngineState { Idle, Capturing, Playing, Error };`
  - `enum class wa::BackendKind { WasapiShared };` (only kind in MVP)
  - `struct wa::EngineStatus { EngineState state; float levelL; float levelR; uint64_t overruns; uint64_t underruns; AudioFormat actualFormat; uint32_t elapsedMs; std::string message; };`
  - `class wa::Engine { std::vector<DeviceInfo> enumerate(DataFlow); Result startCapture(BackendKind, const DeviceId&, const std::wstring& wavPath); Result startPlayback(BackendKind, const DeviceId&, const std::wstring& wavPath); void stop(); EngineStatus poll(); }`

- [ ] **Step 1: Create `src/core/Engine.h`**

```cpp
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "IAudioBackend.h"
#include "RingBuffer.h"

namespace wa {

enum class EngineState { Idle, Capturing, Playing, Error };
enum class BackendKind { WasapiShared };

struct EngineStatus {
    EngineState state = EngineState::Idle;
    float       levelL = 0.f;
    float       levelR = 0.f;
    uint64_t    overruns = 0;
    uint64_t    underruns = 0;
    AudioFormat actualFormat{};
    uint32_t    elapsedMs = 0;
    std::string message;
};

class WavWriter;
class WavReader;

class Engine {
public:
    Engine();
    ~Engine();

    std::vector<DeviceInfo> enumerate(DataFlow flow);
    Result startCapture(BackendKind kind, const DeviceId& id, const std::wstring& wavPath);
    Result startPlayback(BackendKind kind, const DeviceId& id, const std::wstring& wavPath);
    void   stop();
    EngineStatus poll();

private:
    void captureLoop(std::wstring wavPath);
    void playbackLoop(std::wstring wavPath);

    std::unique_ptr<IAudioBackend> backend_;
    std::unique_ptr<RingBuffer>    ring_;
    std::thread                    pump_;
    std::atomic<bool>              running_{false};

    std::mutex   mtx_;
    EngineStatus status_;
    unsigned long long startTick_ = 0;
};

} // namespace wa
```

- [ ] **Step 2: Create `src/core/Engine.cpp`**

```cpp
#include "Engine.h"
#include "WasapiShared.h"
#include "WavFile.h"
#include "DeviceEnumerator.h"
#include "ComUtil.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace wa {

namespace {
constexpr size_t kRingBytes = 1 << 20; // 1 MiB

// Compute peak level (0..1) for L/R from interleaved float32 or int16 PCM.
void computeLevels(const uint8_t* data, size_t bytes, const AudioFormat& fmt,
                   float& l, float& r) {
    l = r = 0.f;
    if (fmt.channels == 0) return;
    const uint16_t ch = fmt.channels;
    if (fmt.isFloat && fmt.bitsPerSample == 32) {
        const float* s = reinterpret_cast<const float*>(data);
        size_t n = bytes / 4;
        for (size_t i = 0; i + ch <= n; i += ch) {
            l = std::max(l, std::fabs(s[i]));
            r = std::max(r, std::fabs(s[i + (ch > 1 ? 1 : 0)]));
        }
    } else if (!fmt.isFloat && fmt.bitsPerSample == 16) {
        const int16_t* s = reinterpret_cast<const int16_t*>(data);
        size_t n = bytes / 2;
        for (size_t i = 0; i + ch <= n; i += ch) {
            l = std::max(l, std::fabs(s[i] / 32768.f));
            r = std::max(r, std::fabs(s[i + (ch > 1 ? 1 : 0)] / 32768.f));
        }
    }
}
} // namespace

Engine::Engine() = default;
Engine::~Engine() { stop(); }

std::vector<DeviceInfo> Engine::enumerate(DataFlow flow) {
    ComInitGuard com;
    DeviceEnumerator de;
    std::vector<DeviceInfo> out;
    de.enumerate(flow, out);
    return out;
}

Result Engine::startCapture(BackendKind, const DeviceId& id, const std::wstring& wavPath) {
    stop();
    ring_ = std::make_unique<RingBuffer>(kRingBytes);
    backend_ = std::make_unique<WasapiSharedCapture>();
    Result r = backend_->open(id, AudioFormat{}, ring_.get());
    if (!r) return r;
    r = backend_->start();
    if (!r) return r;
    running_.store(true);
    startTick_ = GetTickCount64();
    { std::lock_guard<std::mutex> lk(mtx_); status_ = {}; status_.state = EngineState::Capturing; }
    pump_ = std::thread(&Engine::captureLoop, this, wavPath);
    return Result::Ok();
}

Result Engine::startPlayback(BackendKind, const DeviceId& id, const std::wstring& wavPath) {
    stop();
    ring_ = std::make_unique<RingBuffer>(kRingBytes);
    backend_ = std::make_unique<WasapiSharedRender>();
    Result r = backend_->open(id, AudioFormat{}, ring_.get());
    if (!r) return r;
    running_.store(true);
    startTick_ = GetTickCount64();
    { std::lock_guard<std::mutex> lk(mtx_); status_ = {}; status_.state = EngineState::Playing; }
    pump_ = std::thread(&Engine::playbackLoop, this, wavPath);
    return Result::Ok();
}

void Engine::stop() {
    running_.store(false);
    if (pump_.joinable()) pump_.join();
    if (backend_) backend_->stop();
    backend_.reset();
    ring_.reset();
    std::lock_guard<std::mutex> lk(mtx_);
    if (status_.state != EngineState::Error) status_.state = EngineState::Idle;
}

void Engine::captureLoop(std::wstring wavPath) {
    // Backend already started; its actualFormat is known after start.
    AudioFormat fmt = backend_->stats().actualFormat;
    WavWriter writer;
    if (!writer.open(wavPath, fmt)) {
        std::lock_guard<std::mutex> lk(mtx_);
        status_.state = EngineState::Error;
        status_.message = "cannot open output wav";
        running_.store(false);
        return;
    }
    std::vector<uint8_t> buf(16384);
    while (running_.load()) {
        size_t got = ring_->read(buf.data(), buf.size());
        if (got == 0) { Sleep(5); }
        else {
            writer.write(buf.data(), got);
            float l, r; computeLevels(buf.data(), got, fmt, l, r);
            std::lock_guard<std::mutex> lk(mtx_);
            status_.levelL = l; status_.levelR = r;
            status_.actualFormat = fmt;
            status_.overruns = ring_->overruns();
            status_.underruns = ring_->underruns();
            status_.elapsedMs = static_cast<uint32_t>(GetTickCount64() - startTick_);
        }
    }
    writer.close();
}

void Engine::playbackLoop(std::wstring wavPath) {
    WavReader reader;
    if (!reader.open(wavPath)) {
        std::lock_guard<std::mutex> lk(mtx_);
        status_.state = EngineState::Error;
        status_.message = "cannot open input wav";
        running_.store(false);
        return;
    }
    AudioFormat fmt = reader.format();
    backend_->start(); // render thread uses device mix format; feed best-effort
    std::vector<uint8_t> buf(16384);
    bool fileDone = false;
    while (running_.load()) {
        if (!fileDone) {
            // keep the ring topped up
            while (ring_->availableWrite() >= buf.size()) {
                size_t got = reader.read(buf.data(), buf.size());
                if (got == 0) { fileDone = true; break; }
                ring_->write(buf.data(), got);
                float l, r; computeLevels(buf.data(), got, fmt, l, r);
                std::lock_guard<std::mutex> lk(mtx_);
                status_.levelL = l; status_.levelR = r;
            }
        } else if (ring_->availableRead() == 0) {
            break; // drained
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            status_.actualFormat = backend_->stats().actualFormat;
            status_.overruns = ring_->overruns();
            status_.underruns = ring_->underruns();
            status_.elapsedMs = static_cast<uint32_t>(GetTickCount64() - startTick_);
        }
        Sleep(5);
    }
    running_.store(false);
}

EngineStatus Engine::poll() {
    std::lock_guard<std::mutex> lk(mtx_);
    return status_;
}

} // namespace wa
```

> Note on playback/format: MVP plays a `.wav` whose sample format may differ from the device mix format. For the MVP we require the input wav to already match the device mix format (commonly float32/48k from a prior capture). A resampler/format-converter is explicitly out of scope (later phase). The CLI/GUI surface a clear message if `reader.format()` != `backend stats actualFormat`.

- [ ] **Step 3: Create `src/cli/main.cpp`**

```cpp
#include <cstdio>
#include <string>
#include <vector>
#include "Engine.h"
#include "DeviceEnumerator.h"
#include "ComUtil.h"

using namespace wa;

static void usage() {
    std::printf(
        "WinAudioCli list [--render|--capture]\n"
        "WinAudioCli capture --out <file.wav> [--device <id>] [--seconds N]\n"
        "WinAudioCli play    --in  <file.wav> [--device <id>]\n");
}

static std::wstring arg(int argc, wchar_t** argv, const wchar_t* key) {
    for (int i = 1; i < argc - 1; ++i)
        if (std::wcscmp(argv[i], key) == 0) return argv[i + 1];
    return L"";
}
static bool has(int argc, wchar_t** argv, const wchar_t* key) {
    for (int i = 1; i < argc; ++i) if (std::wcscmp(argv[i], key) == 0) return true;
    return false;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { usage(); return 1; }
    ComInitGuard com;
    std::wstring cmd = argv[1];

    if (cmd == L"list") {
        DataFlow flow = has(argc, argv, L"--capture") ? DataFlow::Capture : DataFlow::Render;
        DeviceEnumerator de;
        std::vector<DeviceInfo> devs;
        Result r = de.enumerate(flow, devs);
        if (!r) { std::printf("enumerate failed: %s\n", r.message.c_str()); return 2; }
        for (auto& d : devs) {
            std::wprintf(L"%s %s  [%u Hz %u ch %s]\n",
                d.isDefault ? L"*" : L" ", d.name.c_str(),
                d.mixFormat.sampleRate, d.mixFormat.channels,
                d.mixFormat.isFloat ? L"float" : L"pcm");
            std::wprintf(L"     id=%s\n", d.id.c_str());
        }
        return 0;
    }

    Engine eng;
    if (cmd == L"capture") {
        std::wstring out = arg(argc, argv, L"--out");
        if (out.empty()) { usage(); return 1; }
        std::wstring id = arg(argc, argv, L"--device");
        std::wstring secStr = arg(argc, argv, L"--seconds");
        int seconds = secStr.empty() ? 5 : _wtoi(secStr.c_str());
        Result r = eng.startCapture(BackendKind::WasapiShared, id, out);
        if (!r) { std::printf("capture start failed: %s\n", r.message.c_str()); return 2; }
        for (int i = 0; i < seconds * 10; ++i) {
            Sleep(100);
            EngineStatus s = eng.poll();
            std::printf("\rL=%.2f R=%.2f over=%llu under=%llu  ",
                s.levelL, s.levelR,
                (unsigned long long)s.overruns, (unsigned long long)s.underruns);
        }
        eng.stop();
        std::printf("\nwrote %ls\n", out.c_str());
        return 0;
    }

    if (cmd == L"play") {
        std::wstring in = arg(argc, argv, L"--in");
        if (in.empty()) { usage(); return 1; }
        std::wstring id = arg(argc, argv, L"--device");
        Result r = eng.startPlayback(BackendKind::WasapiShared, id, in);
        if (!r) { std::printf("play start failed: %s\n", r.message.c_str()); return 2; }
        for (;;) {
            Sleep(100);
            EngineStatus s = eng.poll();
            if (s.state == EngineState::Idle || s.state == EngineState::Error) break;
            std::printf("\rplaying L=%.2f R=%.2f  ", s.levelL, s.levelR);
        }
        eng.stop();
        std::printf("\ndone\n");
        return 0;
    }

    usage();
    return 1;
}
```

- [ ] **Step 4: Create `src/cli/WinAudioCli.vcxproj`**

Console application, x64 Debug/Release, C++17. Include dir `$(SolutionDir)src/core`. Project reference to `WinAudioCore.vcxproj`. Linker additional dependencies: `ole32.lib`. Add to `WinAudio.sln`.

- [ ] **Step 5: Build everything**

Run: `msbuild WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: builds clean.

- [ ] **Step 6: Manual end-to-end verification (requires audio hardware)**

Run:
```powershell
x64\Debug\WinAudioCli.exe list --capture
x64\Debug\WinAudioCli.exe list --render
x64\Debug\WinAudioCli.exe capture --out cap.wav --seconds 5
x64\Debug\WinAudioCli.exe play --in cap.wav
```
Expected: `list` prints devices with a `*` on the default; `capture` shows moving L/R levels while you make sound, then writes `cap.wav`; `play` plays it back. Confirm `cap.wav` opens in any media player.

- [ ] **Step 7: Commit**

```powershell
git add -A
git commit -m "feat: Engine orchestration + minimal CLI; first end-to-end capture/play"
```

---

## Task 9: WinAudioGui — Dear ImGui + DX11 front-end (primary)

**Files:**
- Create: `third_party/imgui/` (vendored Dear ImGui core + `backends/imgui_impl_win32.*` + `backends/imgui_impl_dx11.*`)
- Create: `src/gui/main.cpp`
- Create: `src/gui/AppUi.h`
- Create: `src/gui/AppUi.cpp`
- Create: `src/gui/WinAudioGui.vcxproj`
- Modify: `WinAudio.sln`

**Interfaces:**
- Consumes: `wa::Engine`, `wa::DeviceInfo`, `wa::EngineStatus`, `wa::BackendKind`, `wa::DataFlow`.
- Produces: `class AppUi { void draw(Engine& eng); }` rendering all controls and binding them to `Engine` calls; `main.cpp` owns the Win32 window + DX11 swapchain + ImGui frame loop and calls `AppUi::draw` each frame.

- [ ] **Step 1: Vendor Dear ImGui**

Copy Dear ImGui (v1.90+ docking or master) sources into `third_party/imgui/`: `imgui*.cpp/.h`, plus `backends/imgui_impl_win32.cpp/.h` and `backends/imgui_impl_dx11.cpp/.h`.

- [ ] **Step 2: Create `src/gui/AppUi.h`**

```cpp
#pragma once
#include <string>
#include <vector>
#include "Engine.h"

class AppUi {
public:
    void draw(wa::Engine& eng);
private:
    void refreshDevices(wa::Engine& eng);

    int  backendIdx_ = 0;          // 0 = WASAPI-Shared (only MVP option)
    int  flowIdx_ = 0;             // 0 = capture, 1 = playback
    int  deviceIdx_ = 0;
    bool devicesLoaded_ = false;
    std::vector<wa::DeviceInfo> devices_;
    char wavPath_[260] = "cap.wav";
    std::vector<std::string> logLines_;
};
```

- [ ] **Step 3: Create `src/gui/AppUi.cpp`**

```cpp
#include "AppUi.h"
#include "imgui.h"
#include <string>

static std::string wtou(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring utow(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

void AppUi::refreshDevices(wa::Engine& eng) {
    wa::DataFlow flow = (flowIdx_ == 0) ? wa::DataFlow::Capture : wa::DataFlow::Render;
    devices_ = eng.enumerate(flow);
    deviceIdx_ = 0;
    devicesLoaded_ = true;
}

void AppUi::draw(wa::Engine& eng) {
    if (!devicesLoaded_) refreshDevices(eng);

    ImGui::Begin("WinAudio");

    const char* backends[] = {"WASAPI-Shared"};
    ImGui::Combo("Backend", &backendIdx_, backends, 1);

    int prevFlow = flowIdx_;
    ImGui::RadioButton("Capture", &flowIdx_, 0); ImGui::SameLine();
    ImGui::RadioButton("Playback", &flowIdx_, 1);
    if (flowIdx_ != prevFlow) refreshDevices(eng);

    if (ImGui::Button("Refresh devices")) refreshDevices(eng);

    if (ImGui::BeginListBox("Devices")) {
        for (int i = 0; i < (int)devices_.size(); ++i) {
            std::string label = (devices_[i].isDefault ? "* " : "  ") + wtou(devices_[i].name);
            if (ImGui::Selectable(label.c_str(), deviceIdx_ == i)) deviceIdx_ = i;
        }
        ImGui::EndListBox();
    }

    ImGui::InputText("WAV file", wavPath_, sizeof(wavPath_));

    wa::EngineStatus st = eng.poll();
    bool busy = (st.state == wa::EngineState::Capturing || st.state == wa::EngineState::Playing);

    if (!busy) {
        if (ImGui::Button("Start")) {
            wa::DeviceId id = devices_.empty() ? L"" : devices_[deviceIdx_].id;
            std::wstring path = utow(wavPath_);
            wa::Result r = (flowIdx_ == 0)
                ? eng.startCapture(wa::BackendKind::WasapiShared, id, path)
                : eng.startPlayback(wa::BackendKind::WasapiShared, id, path);
            logLines_.push_back(r ? "started" : ("error: " + r.message));
        }
    } else {
        if (ImGui::Button("Stop")) { eng.stop(); logLines_.push_back("stopped"); }
    }

    ImGui::SameLine();
    const char* stateStr[] = {"Idle", "Capturing", "Playing", "Error"};
    ImGui::Text("State: %s  %.1fs", stateStr[(int)st.state], st.elapsedMs / 1000.f);

    ImGui::ProgressBar(st.levelL, ImVec2(-1, 0), "L");
    ImGui::ProgressBar(st.levelR, ImVec2(-1, 0), "R");
    ImGui::Text("overrun %llu  underrun %llu  fmt %u/%u/%u",
        (unsigned long long)st.overruns, (unsigned long long)st.underruns,
        st.actualFormat.sampleRate, st.actualFormat.bitsPerSample, st.actualFormat.channels);

    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 120), true);
    for (auto& l : logLines_) ImGui::TextUnformatted(l.c_str());
    ImGui::EndChild();

    ImGui::End();
}
```

- [ ] **Step 4: Create `src/gui/main.cpp`**

Use the standard Dear ImGui Win32+DX11 example skeleton (`examples/example_win32_directx11/main.cpp`), adapted: create the device + swapchain, init `ImGui_ImplWin32_Init`/`ImGui_ImplDX11_Init`, then in the frame loop call:
```cpp
// inside the render loop, after NewFrame():
static AppUi ui;
static wa::Engine engine;
ui.draw(engine);
```
On `WM_DESTROY`, call `engine.stop()` before teardown. Keep the window title "WinAudio". (Copy the example file verbatim and insert the three lines above plus the `#include "AppUi.h"` and the `engine.stop()` call.)

- [ ] **Step 5: Create `src/gui/WinAudioGui.vcxproj`**

Windows application (`Application`, `Subsystem=Windows`), x64 Debug/Release, C++17. Include dirs: `$(SolutionDir)src/core`, `$(SolutionDir)src/gui`, `$(SolutionDir)third_party/imgui`, `$(SolutionDir)third_party/imgui/backends`. Compile: `src/gui/*.cpp`, `third_party/imgui/imgui*.cpp`, `third_party/imgui/backends/imgui_impl_win32.cpp`, `third_party/imgui/backends/imgui_impl_dx11.cpp`. Project reference to `WinAudioCore.vcxproj`. Linker deps: `d3d11.lib; dxgi.lib; d3dcompiler.lib; ole32.lib`. Add to `WinAudio.sln`.

- [ ] **Step 6: Build**

Run: `msbuild WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: builds clean; `x64\Debug\WinAudioGui.exe` exists.

- [ ] **Step 7: Manual verification (requires audio hardware + desktop)**

Run: `x64\Debug\WinAudioGui.exe`
Expected: window opens; device list populates; selecting Capture + a device + Start shows moving L/R meters and writes the wav; switching to Playback + Start plays the wav back; Stop returns to Idle. Closing the window does not hang (engine.stop() joins threads).

- [ ] **Step 8: Commit**

```powershell
git add -A
git commit -m "feat(gui): Dear ImGui + DX11 front-end bound to Engine"
```

---

## Task 10: CLAUDE.md fix-up + Release build sanity

**Files:**
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: the now-real project layout.
- Produces: corrected build/run commands and architecture notes matching the actual files.

- [ ] **Step 1: Update CLAUDE.md build/run section**

Replace the "目标约定" placeholders with the real commands:
```powershell
msbuild WinAudio.sln /p:Configuration=Debug   /p:Platform=x64 /m
msbuild WinAudio.sln /p:Configuration=Release /p:Platform=x64 /m
x64\Debug\WinAudioTests.exe            # run unit tests
x64\Debug\WinAudioCli.exe list --render
x64\Debug\WinAudioGui.exe              # GUI
```
And update the "项目状态" section to note the MVP is implemented (Core + WASAPI-Shared + CLI + GUI), remove the "仓库为空" wording.

- [ ] **Step 2: Verify Release build**

Run: `msbuild WinAudio.sln /p:Configuration=Release /p:Platform=x64 /m`
Expected: all four projects build clean in Release.

- [ ] **Step 3: Run full test suite**

Run: `x64\Release\WinAudioTests.exe`
Expected: all tests PASS.

- [ ] **Step 4: Commit**

```powershell
git add -A
git commit -m "docs: update CLAUDE.md to match implemented MVP"
```

---

## Self-Review

**Spec coverage:**
- Layered architecture (Core lib + Cli + Gui) — Tasks 1, 8, 9. ✅
- AudioFormat + WAVEFORMATEX conversion — Task 2. ✅
- SPSC RingBuffer + xrun counters — Task 3. ✅
- WAV reader/writer round-trip + bad-header handling — Task 4. ✅
- DeviceEnumerator (IMMDevice) — Task 5. ✅
- IAudioBackend interface — Task 5. ✅
- WASAPI-Shared capture + render (own thread, ring decoupling, scheme C) — Tasks 6, 7. ✅
- Engine poll() snapshot, levels, xrun — Task 8. ✅
- Minimal CLI (list/capture/play) — Task 8. ✅
- ImGui+DX11 GUI as primary front-end, bound to Engine — Task 9. ✅
- Error handling via Result, HRESULT normalization, COM RAII/threading — Tasks 1, 5, 6, 7. ✅
- Testing via gtest (RingBuffer/AudioFormat/Wav) — Tasks 1–4. ✅
- gtest only in Tests; ImGui only in Gui; Core/Cli zero-dep — Tasks 1, 6, 9. ✅
- Out of scope (waveIn/waveOut, WASAPI-Exclusive, format conversion/resampling, latency measurement) — documented as later phases. ✅

**Deferred-to-later-phase (intentionally not in MVP, per spec phasing):** format/capability probe panel (IsFormatSupported UI), latency/glitch measurement, signal-quality loopback compare, waveIn/waveOut + Exclusive backends. The backend interface and ring xrun counters are already shaped to accept them without rework.

**Known MVP limitation (made explicit):** playback assumes the input wav format matches the device mix format (no resampler). Capture writes the device mix format (commonly float32). This is called out in Task 8 Step 2's note and surfaced to the user; a converter is a later phase.

**Type consistency:** `AudioFormat`, `Result`, `RingBuffer`, `IAudioBackend`/`DeviceInfo`/`BackendStats`, `Engine`/`EngineStatus`/`EngineState`/`BackendKind` names and signatures are used identically across Tasks 2–9. `enumerate`, `startCapture`, `startPlayback`, `stop`, `poll` match between Engine.h (Task 8), CLI (Task 8), and GUI (Task 9). ✅
