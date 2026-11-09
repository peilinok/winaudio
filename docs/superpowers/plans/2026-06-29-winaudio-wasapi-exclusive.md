# WinAudio Phase 2 — WASAPI-Exclusive Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a WASAPI-Exclusive backend (low-latency, explicit format + `IsFormatSupported` probing) to WinAudio, by first refactoring the two WASAPI-Shared backend classes into a `WasapiStream` base + Capture/Render subclasses, then adding the exclusive mode path and wiring it through Core/GUI.

**Architecture:** `WasapiStream` (abstract) owns the lifecycle, the synchronous `start()` condition-variable handshake, and the `threadMain` scaffold; direction differs via virtual hooks (`dataFlow`/`createService`/`preRoll`/`runLoop`/`resetService`) in `WasapiCaptureStream`/`WasapiRenderStream`; mode (Shared vs Exclusive) is a branch inside the base `prepareClient()` (format negotiation + Initialize, with the exclusive `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` rebuild-retry). Shared behavior must stay byte-for-byte equivalent to Phase 1.

**Tech Stack:** C++17, MSBuild (x64), MSVC v143, Win32 + WASAPI, gtest. Pure Win32+STL in Core; ImGui only in Gui.

## Global Constraints

- Build system: **MSBuild**, Debug/Release, platform **x64** only. PlatformToolset v143, `/std:c++17`, `/W4`, Core warning-clean.
- **msbuild is NOT on PATH.** Build with the PowerShell tool (Git-Bash mangles `/p:` flags):
  `& "d:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
  Build the **solution** (`WinAudio.sln`), never a single `.vcxproj` standalone (`$(SolutionDir)`-relative include/reference paths break).
- WinAudioCore: pure **Win32 + STL, zero third-party**. ImGui only in Gui; gtest only in Tests.
- All Core symbols in namespace `wa`. Core never `printf`/`std::cout`, never throws across its public API; returns `wa::Result`.
- COM: every WASAPI thread runs its own `ComInitGuard` (`CoInitializeEx(nullptr, COINIT_MULTITHREADED)` paired with `CoUninitialize`); all COM interfaces RAII via `wa::ComPtr`; no raw `Release()`.
- **Equivalence:** the `WasapiMode::Shared` path must behave identically to Phase 1 (100ms buffer, event-driven, the synchronous-start handshake, the `CreateEventW`/`std::thread` guards, and the `GetBufferSize`/`SetEventHandle`/`Start` HRESULT checks). Existing 14 gtest tests must stay green.
- Commit with `git -c commit.gpgsign=false commit -m "..."` (a clang-format pre-commit hook prints a harmless `cannot use -i when reading from stdin` error; the commit still succeeds — verify with `git log --oneline -1`).
- `wa::Result` factory is `Result::Fail(long code, std::string message)` and `Result::Ok()` — there is no `Result::Error`.

---

## File Structure

```
src/core/
  FormatSpec.h / .cpp        NEW — pure helpers: parseFormatSpec, alignedBufferDuration100ns,
                                   selectSupportedFormat, defaultExclusiveCaptureCandidates
  WasapiStream.h / .cpp      NEW — WasapiStream base + WasapiCaptureStream + WasapiRenderStream
  WasapiShared.h / .cpp      DELETED — replaced by WasapiStream
src/gui/AppUi.h / .cpp       MODIFIED — backend combo (2), format controls, probe button
src/tests/
  test_formatspec.cpp        NEW — gtest for FormatSpec pure helpers
```

---

## Task 1: FormatSpec pure helpers (gtest)

**Files:**
- Create: `src/core/FormatSpec.h`, `src/core/FormatSpec.cpp`
- Create: `src/tests/test_formatspec.cpp`
- Modify: `src/core/WinAudioCore.vcxproj` (add FormatSpec.h/.cpp), `src/tests/WinAudioTests.vcxproj` (add test_formatspec.cpp)

**Interfaces:**
- Consumes: `wa::AudioFormat`.
- Produces (namespace `wa`):
  - `bool parseFormatSpec(const std::string& spec, AudioFormat& out);` — `"48000/16/2"` → {48000,2,16,false}; trailing `f` on bits means float (`"48000/32/2f"` → isFloat true). Returns false on malformed input.
  - `long long alignedBufferDuration100ns(uint32_t sampleRate, uint32_t alignedFrames);` — MSDN exclusive realign formula, in 100-ns units (REFERENCE_TIME is `long long`).
  - `int selectSupportedFormat(const std::vector<AudioFormat>& candidates, const std::function<bool(const AudioFormat&)>& isSupported);` — index of first candidate for which `isSupported` is true, else -1.
  - `std::vector<AudioFormat> defaultExclusiveCaptureCandidates();` — the fallback probe list.

- [ ] **Step 1: Write the failing test** — `src/tests/test_formatspec.cpp`:

```cpp
#include <gtest/gtest.h>
#include "FormatSpec.h"
using namespace wa;

TEST(FormatSpec, ParsesPcm) {
    AudioFormat f{};
    ASSERT_TRUE(parseFormatSpec("48000/16/2", f));
    EXPECT_EQ(f.sampleRate, 48000u);
    EXPECT_EQ(f.bitsPerSample, 16);
    EXPECT_EQ(f.channels, 2);
    EXPECT_FALSE(f.isFloat);
}

TEST(FormatSpec, ParsesFloatSuffix) {
    AudioFormat f{};
    ASSERT_TRUE(parseFormatSpec("48000/32/2f", f));
    EXPECT_EQ(f.bitsPerSample, 32);
    EXPECT_TRUE(f.isFloat);
}

TEST(FormatSpec, RejectsMalformed) {
    AudioFormat f{};
    EXPECT_FALSE(parseFormatSpec("48000/16", f));
    EXPECT_FALSE(parseFormatSpec("abc/16/2", f));
    EXPECT_FALSE(parseFormatSpec("", f));
}

TEST(FormatSpec, AlignedDurationFormula) {
    // 10000.0 * 1000 / 48000 * 480 + 0.5 = 100000 (100ms in 100ns units = 1,000,000? )
    // For 48000 Hz and 480 frames (10ms): 10000*1000/48000*480 = 100000 (100ns units) = 10 ms. round.
    long long d = alignedBufferDuration100ns(48000, 480);
    EXPECT_EQ(d, 100000); // 10 ms in 100-ns units
}

TEST(FormatSpec, SelectFirstSupported) {
    std::vector<AudioFormat> c = {
        {48000,2,16,false}, {44100,2,16,false}, {48000,2,32,true}
    };
    int idx = selectSupportedFormat(c, [](const AudioFormat& f){ return f.sampleRate == 44100; });
    EXPECT_EQ(idx, 1);
    int none = selectSupportedFormat(c, [](const AudioFormat&){ return false; });
    EXPECT_EQ(none, -1);
}

TEST(FormatSpec, CaptureCandidatesNonEmpty) {
    auto c = defaultExclusiveCaptureCandidates();
    ASSERT_FALSE(c.empty());
    EXPECT_EQ(c.front().sampleRate, 48000u); // 48k/16/2 first
    EXPECT_EQ(c.front().bitsPerSample, 16);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run (PowerShell): `& "d:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: FAIL — `FormatSpec.h` not found.

- [ ] **Step 3: Create `src/core/FormatSpec.h`**

```cpp
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "AudioFormat.h"

namespace wa {

bool parseFormatSpec(const std::string& spec, AudioFormat& out);
long long alignedBufferDuration100ns(uint32_t sampleRate, uint32_t alignedFrames);
int  selectSupportedFormat(const std::vector<AudioFormat>& candidates,
                           const std::function<bool(const AudioFormat&)>& isSupported);
std::vector<AudioFormat> defaultExclusiveCaptureCandidates();

} // namespace wa
```

- [ ] **Step 4: Create `src/core/FormatSpec.cpp`**

```cpp
#include "FormatSpec.h"
#include <cstdio>
#include <cstdlib>

namespace wa {

bool parseFormatSpec(const std::string& spec, AudioFormat& out) {
    // <rate>/<bits>/<ch> with optional trailing 'f' on bits meaning float.
    unsigned rate = 0, bits = 0, ch = 0;
    char tail = 0;
    // sscanf returns the count of matched fields; require rate/bits/ch.
    int n = std::sscanf(spec.c_str(), "%u/%u/%u%c", &rate, &bits, &ch, &tail);
    if (n < 3) return false;
    bool isFloat = false;
    if (n == 4) {
        if (tail == 'f' || tail == 'F') isFloat = true;
        else return false; // any other trailing char is malformed
    }
    if (rate == 0 || ch == 0 || (bits != 8 && bits != 16 && bits != 24 && bits != 32))
        return false;
    out.sampleRate = rate;
    out.bitsPerSample = static_cast<uint16_t>(bits);
    out.channels = static_cast<uint16_t>(ch);
    out.isFloat = isFloat;
    return true;
}

long long alignedBufferDuration100ns(uint32_t sampleRate, uint32_t alignedFrames) {
    if (sampleRate == 0) return 0;
    // MSDN exclusive-mode realignment: hns = 10000 * 1000 / rate * frames + 0.5
    return static_cast<long long>(10000.0 * 1000.0 / sampleRate * alignedFrames + 0.5);
}

int selectSupportedFormat(const std::vector<AudioFormat>& candidates,
                          const std::function<bool(const AudioFormat&)>& isSupported) {
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
        if (isSupported(candidates[i])) return i;
    return -1;
}

std::vector<AudioFormat> defaultExclusiveCaptureCandidates() {
    return {
        {48000, 2, 16, false},
        {44100, 2, 16, false},
        {48000, 2, 24, false},
        {48000, 2, 32, true},
        {48000, 1, 16, false},
    };
}

} // namespace wa
```

- [ ] **Step 5: Add files to projects** — add `FormatSpec.h`/`.cpp` to `WinAudioCore.vcxproj` (`<ClInclude>`/`<ClCompile>`); add `test_formatspec.cpp` to `WinAudioTests.vcxproj` (`<ClCompile>`).

- [ ] **Step 6: Build and run tests**

Run: `& "...MSBuild.exe" WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m` then `& ".\x64\Debug\WinAudioTests.exe" --gtest_filter=FormatSpec.*`
Expected: 6 FormatSpec tests PASS; total now 20 tests; build /W4 clean.

- [ ] **Step 7: Commit**

```powershell
git add src/core/FormatSpec.h src/core/FormatSpec.cpp src/tests/test_formatspec.cpp src/core/WinAudioCore.vcxproj src/tests/WinAudioTests.vcxproj
git -c commit.gpgsign=false commit -m "feat(core): FormatSpec pure helpers (parse/align/select/candidates)"
```

---

## Task 2: Refactor WASAPI backends into WasapiStream base + Capture/Render subclasses (Shared behavior preserved)

**Files:**
- Create: `src/core/WasapiStream.h`, `src/core/WasapiStream.cpp`
- Delete: `src/core/WasapiShared.h`, `src/core/WasapiShared.cpp`
- Modify: `src/core/WinAudioCore.vcxproj`

**Interfaces:**
- Consumes: `IAudioBackend`, `RingBuffer`, `AudioFormat`, `ComUtil`.
- Produces (namespace `wa`):
  - `enum class WasapiMode { Shared, Exclusive };`
  - `class WasapiStream : public IAudioBackend` with `WasapiStream(WasapiMode, const AudioFormat* requested)`.
  - `class WasapiCaptureStream : public WasapiStream` with `WasapiCaptureStream(WasapiMode, const AudioFormat* requested)`.
  - `class WasapiRenderStream : public WasapiStream` with `WasapiRenderStream(WasapiMode, const AudioFormat* requested)`.
  - In THIS task the exclusive `prepareClient` branch returns a "not implemented" failure (filled in Task 3). Shared path is fully functional and equivalent to Phase 1.

> No new unit test: this is a behavior-preserving refactor. Verification is the existing 14 tests + clean build + a GUI Shared capture/playback smoke confirming runtime parity.

- [ ] **Step 1: Create `src/core/WasapiStream.h`**

```cpp
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include "IAudioBackend.h"
#include "ComUtil.h"
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace wa {

class RingBuffer;

enum class WasapiMode { Shared, Exclusive };

class WasapiStream : public IAudioBackend {
public:
    WasapiStream(WasapiMode mode, const AudioFormat* requested);
    ~WasapiStream() override;
    Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring) override;
    Result start() override;
    void   stop() override;
    void   close() override;
    BackendStats stats() const override;

protected:
    // Direction-specific hooks implemented by Capture/Render subclasses.
    virtual EDataFlow dataFlow() const = 0;
    virtual Result createService() = 0;   // GetService(IAudioCaptureClient/RenderClient)
    virtual void   preRoll() {}           // render: one silent buffer; capture: nothing
    virtual void   runLoop() = 0;         // drain/feed loop; runs while running_
    virtual void   resetService() = 0;    // Reset() the service ComPtr (called from close())

    // Scaffolding state visible to subclasses.
    RingBuffer* ring_ = nullptr;
    AudioFormat actualFormat_{};
    uint32_t    bufferFrames_ = 0;
    uint32_t    frameBytes_ = 0;
    std::atomic<bool> running_{false};
    void*       hEvent_ = nullptr;        // HANDLE
    ComPtr<IAudioClient> client_;

private:
    void   threadMain();
    void   signalReady(Result res);
    Result prepareClient(IMMDevice* dev); // mode-aware: negotiate format + Initialize; sets actualFormat_/frameBytes_

    WasapiMode  mode_;
    AudioFormat requestedFormat_{};
    bool        hasRequested_ = false;
    std::thread thread_;
    DeviceId    deviceId_;

    std::mutex              readyMtx_;
    std::condition_variable readyCv_;
    bool                    ready_ = false;
    Result                  startResult_ = Result::Ok();
};

class WasapiCaptureStream : public WasapiStream {
public:
    WasapiCaptureStream(WasapiMode mode, const AudioFormat* requested);
    ~WasapiCaptureStream() override;
protected:
    EDataFlow dataFlow() const override { return eCapture; }
    Result createService() override;
    void   runLoop() override;
    void   resetService() override { capture_.Reset(); }
private:
    ComPtr<IAudioCaptureClient> capture_;
};

class WasapiRenderStream : public WasapiStream {
public:
    WasapiRenderStream(WasapiMode mode, const AudioFormat* requested);
    ~WasapiRenderStream() override;
protected:
    EDataFlow dataFlow() const override { return eRender; }
    Result createService() override;
    void   preRoll() override;
    void   runLoop() override;
    void   resetService() override { render_.Reset(); }
private:
    ComPtr<IAudioRenderClient> render_;
};

} // namespace wa
```

- [ ] **Step 2: Create `src/core/WasapiStream.cpp`**

```cpp
#include "WasapiStream.h"
#include "RingBuffer.h"
#include <system_error>
#include <cstring>

namespace wa {

WasapiStream::WasapiStream(WasapiMode mode, const AudioFormat* requested)
    : mode_(mode), hasRequested_(requested != nullptr) {
    if (requested) requestedFormat_ = *requested;
}

WasapiStream::~WasapiStream() { stop(); } // subclass dtor already ran close()

Result WasapiStream::open(const DeviceId& id, const AudioFormat& /*fmt*/, RingBuffer* ring) {
    deviceId_ = id;
    ring_ = ring;
    return Result::Ok(); // real activation happens on the worker thread (its own COM apt)
}

void WasapiStream::signalReady(Result res) {
    { std::lock_guard<std::mutex> lk(readyMtx_); startResult_ = res; ready_ = true; }
    if (!res) running_.store(false);
    readyCv_.notify_one();
}

Result WasapiStream::start() {
    if (running_.exchange(true)) return Result::Ok();
    if (hEvent_) { CloseHandle(static_cast<HANDLE>(hEvent_)); hEvent_ = nullptr; }
    hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent_) {
        running_.store(false);
        return Result::Fail(static_cast<long>(GetLastError()),
                            "WasapiStream::start: CreateEventW failed");
    }
    { std::lock_guard<std::mutex> lk(readyMtx_); ready_ = false; startResult_ = Result::Ok(); }
    try {
        thread_ = std::thread(&WasapiStream::threadMain, this);
    } catch (const std::system_error& e) {
        CloseHandle(static_cast<HANDLE>(hEvent_));
        hEvent_ = nullptr;
        running_.store(false);
        return Result::Fail(static_cast<long>(e.code().value()),
                            "WasapiStream::start: failed to launch thread");
    }
    Result r;
    {
        std::unique_lock<std::mutex> lk(readyMtx_);
        readyCv_.wait(lk, [this] { return ready_; });
        r = startResult_;
    }
    if (!r) stop();
    return r;
}

void WasapiStream::stop() {
    running_.store(false);
    if (hEvent_) SetEvent(static_cast<HANDLE>(hEvent_));
    if (thread_.joinable()) thread_.join();
}

void WasapiStream::close() {
    stop();
    if (hEvent_) { CloseHandle(static_cast<HANDLE>(hEvent_)); hEvent_ = nullptr; }
    resetService();
    client_.Reset();
}

BackendStats WasapiStream::stats() const {
    BackendStats s{};
    s.actualFormat = actualFormat_;
    s.bufferFrames = bufferFrames_;
    if (ring_) { s.overruns = ring_->overruns(); s.underruns = ring_->underruns(); }
    return s;
}

Result WasapiStream::prepareClient(IMMDevice* dev) {
    if (mode_ == WasapiMode::Shared) {
        WAVEFORMATEX* mix = nullptr;
        HRESULT hr = client_->GetMixFormat(&mix);
        if (FAILED(hr)) return HrToResult(hr, "WasapiStream: GetMixFormat");
        if (!mix) return Result::Fail(-1, "WasapiStream: GetMixFormat returned null");
        actualFormat_ = AudioFormat::fromWaveFormat(mix);
        frameBytes_ = actualFormat_.blockAlign();
        REFERENCE_TIME dur = 10'000'000 / 10; // 100 ms buffer
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) return HrToResult(hr, "WasapiStream: Initialize(shared)");
        return Result::Ok();
    }
    // Exclusive: implemented in Task 3.
    (void)dev;
    return Result::Fail(-1, "WasapiStream: exclusive mode not implemented");
}

void WasapiStream::threadMain() {
    ComInitGuard com; // this thread's own MTA apartment

    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(e.GetAddressOf()));
    if (FAILED(hr)) { signalReady(HrToResult(hr, "WasapiStream: CoCreateInstance")); return; }

    ComPtr<IMMDevice> dev;
    hr = deviceId_.empty()
        ? e->GetDefaultAudioEndpoint(dataFlow(), eConsole, &dev)
        : e->GetDevice(deviceId_.c_str(), &dev);
    if (FAILED(hr)) { signalReady(HrToResult(hr, "WasapiStream: GetDevice")); return; }

    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client_.GetAddressOf()));
    if (FAILED(hr)) { signalReady(HrToResult(hr, "WasapiStream: Activate")); return; }

    Result pr = prepareClient(dev.Get());
    if (!pr) { signalReady(pr); return; }

    hr = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(hr)) { signalReady(HrToResult(hr, "WasapiStream: GetBufferSize")); return; }
    hr = client_->SetEventHandle(static_cast<HANDLE>(hEvent_));
    if (FAILED(hr)) { signalReady(HrToResult(hr, "WasapiStream: SetEventHandle")); return; }

    Result cs = createService();
    if (!cs) { signalReady(cs); return; }

    preRoll();

    hr = client_->Start();
    if (FAILED(hr)) { signalReady(HrToResult(hr, "WasapiStream: Start")); return; }

    signalReady(Result::Ok()); // device ready; actualFormat_/bufferFrames_ valid

    runLoop();

    client_->Stop();
}

// --------------------------------------------------------------------------
// WasapiCaptureStream
// --------------------------------------------------------------------------

WasapiCaptureStream::WasapiCaptureStream(WasapiMode mode, const AudioFormat* requested)
    : WasapiStream(mode, requested) {}

WasapiCaptureStream::~WasapiCaptureStream() { close(); }

Result WasapiCaptureStream::createService() {
    HRESULT hr = client_->GetService(__uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(capture_.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "WasapiCaptureStream: GetService");
    return Result::Ok();
}

void WasapiCaptureStream::runLoop() {
    while (running_.load()) {
        WaitForSingleObject(static_cast<HANDLE>(hEvent_), 200);
        UINT32 packet = 0;
        while (SUCCEEDED(capture_->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                break;
            const size_t bytes = static_cast<size_t>(frames) * frameBytes_;
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
}

// --------------------------------------------------------------------------
// WasapiRenderStream
// --------------------------------------------------------------------------

WasapiRenderStream::WasapiRenderStream(WasapiMode mode, const AudioFormat* requested)
    : WasapiStream(mode, requested) {}

WasapiRenderStream::~WasapiRenderStream() { close(); }

Result WasapiRenderStream::createService() {
    HRESULT hr = client_->GetService(__uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(render_.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "WasapiRenderStream: GetService");
    return Result::Ok();
}

void WasapiRenderStream::preRoll() {
    BYTE* buf = nullptr;
    if (SUCCEEDED(render_->GetBuffer(bufferFrames_, &buf)))
        render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
}

void WasapiRenderStream::runLoop() {
    std::vector<uint8_t> scratch;
    while (running_.load()) {
        WaitForSingleObject(static_cast<HANDLE>(hEvent_), 200);
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) break;
        UINT32 frames = bufferFrames_ - padding;
        if (frames == 0) continue;
        BYTE* buf = nullptr;
        if (FAILED(render_->GetBuffer(frames, &buf))) break;
        const size_t want = static_cast<size_t>(frames) * frameBytes_;
        scratch.resize(want);
        size_t got = ring_->read(scratch.data(), want);
        std::memcpy(buf, scratch.data(), got);
        if (got < want) std::memset(buf + got, 0, want - got); // underrun -> silence
        render_->ReleaseBuffer(frames, 0);
    }
}

} // namespace wa
```

- [ ] **Step 3: 更新后端构造调用点**

由 GUI 共用的 MonitorEngine 和 Track 后端工厂选择 Shared / Exclusive。

- [ ] **Step 4: Update the Core vcxproj and delete WasapiShared**

In `src/core/WinAudioCore.vcxproj`: remove the `<ClInclude Include="WasapiShared.h" />` and `<ClCompile Include="WasapiShared.cpp" />` items; add `<ClInclude Include="WasapiStream.h" />` and `<ClCompile Include="WasapiStream.cpp" />`. Then `git rm src/core/WasapiShared.h src/core/WasapiShared.cpp`.

- [ ] **Step 5: Build the solution**

Run: `& "...MSBuild.exe" WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: all 4 projects build /W4 clean (0 warnings/errors).

- [ ] **Step 6: Regression — existing tests + Shared runtime parity**

Run:
```powershell
& ".\x64\Debug\WinAudioTests.exe"                         # expect 20 PASSED (14 + 6 FormatSpec)
```
Expected: 20 tests pass; capture writes a non-empty `cap.wav` whose header matches the device mix format (float/48000/32-bit on this machine); play runs to completion and exits cleanly — identical to Phase 1 Shared behavior. Do not commit `cap.wav`.

- [ ] **Step 7: Commit**

```powershell
git add -A
git commit -m "refactor(core): WasapiStream base + Capture/Render subclasses (Shared parity)"
```

---

## Task 3: Exclusive-mode format negotiation + Initialize (with buffer-alignment retry)

**Files:**
- Modify: `src/core/WasapiStream.cpp` (replace the exclusive stub in `prepareClient`), add `#include "FormatSpec.h"`

**Interfaces:**
- Consumes: `FormatSpec` (`alignedBufferDuration100ns`, `selectSupportedFormat`, `defaultExclusiveCaptureCandidates`), `AudioFormat::toWaveFormatExtensible()`.
- Produces: `WasapiStream` constructed with `WasapiMode::Exclusive` now negotiates a format via `IsFormatSupported(EXCLUSIVE)` and Initializes exclusive (with the `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` rebuild-retry). No interface change.

> No unit test (needs a real device); covered by hardware smoke in Task 7 and by Task 1's helper tests. Verification here is a clean build.

- [ ] **Step 1: Add the include**

At the top of `src/core/WasapiStream.cpp`, add `#include "FormatSpec.h"` after `#include "RingBuffer.h"`.

- [ ] **Step 2: Replace the exclusive branch of `prepareClient`**

Replace the two lines
```cpp
    // Exclusive: implemented in Task 3.
    (void)dev;
    return Result::Fail(-1, "WasapiStream: exclusive mode not implemented");
```
with:
```cpp
    // ---- Exclusive ----
    // Candidate formats: the explicitly requested one, else (capture only) a fallback list.
    std::vector<AudioFormat> candidates;
    if (hasRequested_) candidates.push_back(requestedFormat_);
    else if (dataFlow() == eCapture) candidates = defaultExclusiveCaptureCandidates();
    else return Result::Fail(-1, "WasapiStream: exclusive render requires an explicit format");

    int idx = selectSupportedFormat(candidates, [this](const AudioFormat& cand) {
        WAVEFORMATEXTENSIBLE wfx = cand.toWaveFormatExtensible();
        return client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                   reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr) == S_OK;
    });
    if (idx < 0)
        return Result::Fail(static_cast<long>(AUDCLNT_E_UNSUPPORTED_FORMAT),
                            "WasapiStream: no supported exclusive format");

    actualFormat_ = candidates[idx];
    frameBytes_ = actualFormat_.blockAlign();

    REFERENCE_TIME defPer = 0, minPer = 0;
    client_->GetDevicePeriod(&defPer, &minPer);
    REFERENCE_TIME dur = minPer;

    WAVEFORMATEXTENSIBLE wfx = actualFormat_.toWaveFormatExtensible();
    HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur,
                     reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned = 0;
        client_->GetBufferSize(&aligned);
        dur = alignedBufferDuration100ns(actualFormat_.sampleRate, aligned);
        // MSDN: the client must be rebuilt before re-Initializing with the aligned size.
        client_.Reset();
        HRESULT hr2 = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client_.GetAddressOf()));
        if (FAILED(hr2)) return HrToResult(hr2, "WasapiStream: exclusive realign Activate");
        WAVEFORMATEXTENSIBLE wfx2 = actualFormat_.toWaveFormatExtensible();
        hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur,
                 reinterpret_cast<WAVEFORMATEX*>(&wfx2), nullptr);
    }
    if (FAILED(hr)) return HrToResult(hr, "WasapiStream: Initialize(exclusive)");
    return Result::Ok();
```

- [ ] **Step 3: Build the solution**

Run: `& "...MSBuild.exe" WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: /W4 clean. (`AUDCLNT_E_*`, `WAVEFORMATEXTENSIBLE`, `GetDevicePeriod` come from `audioclient.h`/`mmreg.h` already included transitively; if `WAVEFORMATEXTENSIBLE` is unresolved add `#include <mmreg.h>`.)

- [ ] **Step 4: Run existing tests (no regression)**

Run: `& ".\x64\Debug\WinAudioTests.exe"` → expect 20 PASSED (exclusive path isn't exercised by unit tests; this confirms the lib still links).

- [ ] **Step 5: Commit**

```powershell
git add src/core/WasapiStream.cpp
git commit -m "feat(core): WASAPI-Exclusive format negotiation + aligned-buffer Initialize"
```

---

## Task 6: GUI

GUI 格式选择与设备能力弹窗见 [数据格式计划](2026-07-04-winaudio-data-format.md)。

## Task 7: Docs + full hardware smoke + Release

**Files:**
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: the now-complete Shared + Exclusive backends.
- Produces: CLAUDE.md reflects two WASAPI modes; Release build + tests verified; Shared/Exclusive hardware smoke recorded.

- [ ] **Step 1: Update `CLAUDE.md`**

In the capability list, change the WASAPI line to note **both** modes: "WASAPI-Shared（共享）与 WASAPI-Exclusive（独占，需 IsFormatSupported 格式探测）". Update the "后续阶段" list to remove WASAPI-Exclusive (now done) — leave waveIn/waveOut and resampling as future.

- [ ] **Step 2: Release build (all 4 projects)**

Run: `& "...MSBuild.exe" WinAudio.sln /p:Configuration=Release /p:Platform=x64 /m`
Expected: 0 errors across all 4 projects.

- [ ] **Step 3: Full test suite (Release)**

Run: `& ".\x64\Release\WinAudioTests.exe"`
Expected: 20 tests PASS.

- [ ] **Step 4: Hardware smoke — Shared regression + Exclusive**

Run:
Expected: Shared capture writes mix-format wav and plays (Phase 1 parity); probe reports support; exclusive capture writes a **16-bit/48000** wav and exclusive play completes. Record the outcomes (and any `AUDCLNT_E_DEVICE_IN_USE`). Do not commit `.wav` files.

- [ ] **Step 5: Commit**

```powershell
git add CLAUDE.md
git commit -m "docs: CLAUDE.md WASAPI-Exclusive; Phase 2 complete"
```

---

## Self-Review

**Spec coverage:**
- Format negotiation (Shared=GetMixFormat, Exclusive=IsFormatSupported + fallback) — Task 2 (shared) + Task 3 (exclusive). ✅
- Buffer-alignment retry (AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED rebuild) — Task 3. ✅
- Occupied/unsupported errors normalized — Task 3 (HrToResult on Initialize/IsFormatSupported). ✅
- WasapiStream base + Capture/Render subclasses; Shared equivalence — Task 2 (+ regression Step 6). ✅
- Format fallback list (capture, exclusive, no explicit format) — Task 1 `defaultExclusiveCaptureCandidates` + Task 3. ✅
- GUI backend combo + format controls + probe button — Task 6. ✅
- Tests: aligned duration, format fallback selection, format parsing — Task 1. ✅
- Shared regression (14 → 20 gtest + GUI smoke) — Tasks 2, 4, 7. ✅
- Docs — Task 7. ✅

**Out of scope (later phases, per spec §7):** waveIn/waveOut; latency/glitch numeric measurement; loopback signal compare; full format-probe UI panel (only the probe button entry here); resampling.

**Type consistency:** `WasapiMode`, `WasapiStream`/`WasapiCaptureStream`/`WasapiRenderStream` ctor `(WasapiMode, const AudioFormat*)`; `BackendKind::{WasapiShared,WasapiExclusive}`; `parseFormatSpec(const std::string&, AudioFormat&)`; `alignedBufferDuration100ns(uint32_t,uint32_t)`; `selectSupportedFormat(vector, function)`; `defaultExclusiveCaptureCandidates()` — used consistently across Tasks 1–6. `Result::Fail(long,std::string)` everywhere (no `Result::Error`). Protected members (`ring_`, `actualFormat_`, `bufferFrames_`, `frameBytes_`, `running_`, `hEvent_`, `client_`) are accessed by subclass `runLoop`/`preRoll`/`createService`. ✅

**Equivalence risk (Shared):** Task 2 keeps the 100ms buffer, event flags, the synchronous-start handshake, and the HRESULT guards verbatim in the base scaffold; regression Step 6 (20 tests + Shared GUI capture/playback) is the gate before exclusive code lands.
