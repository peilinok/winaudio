# WinAudio Phase 2 — WASAPI-Exclusive Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a WASAPI-Exclusive backend (low-latency, explicit format + `IsFormatSupported` probing) to WinAudio, by first refactoring the two WASAPI-Shared backend classes into a `WasapiStream` base + Capture/Render subclasses, then adding the exclusive mode path and wiring it through Engine/CLI/GUI.

**Architecture:** `WasapiStream` (abstract) owns the lifecycle, the synchronous `start()` condition-variable handshake, and the `threadMain` scaffold; direction differs via virtual hooks (`dataFlow`/`createService`/`preRoll`/`runLoop`/`resetService`) in `WasapiCaptureStream`/`WasapiRenderStream`; mode (Shared vs Exclusive) is a branch inside the base `prepareClient()` (format negotiation + Initialize, with the exclusive `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` rebuild-retry). Shared behavior must stay byte-for-byte equivalent to Phase 1.

**Tech Stack:** C++17, MSBuild (x64), MSVC v143, Win32 + WASAPI, gtest. Pure Win32+STL in Core/Cli; ImGui only in Gui.

## Global Constraints

- Build system: **MSBuild**, Debug/Release, platform **x64** only. PlatformToolset v143, `/std:c++17`, `/W4`, Core warning-clean.
- **msbuild is NOT on PATH.** Build with the PowerShell tool (Git-Bash mangles `/p:` flags):
  `& "d:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
  Build the **solution** (`WinAudio.sln`), never a single `.vcxproj` standalone (`$(SolutionDir)`-relative include/reference paths break).
- WinAudioCore + WinAudioCli: pure **Win32 + STL, zero third-party**. ImGui only in Gui; gtest only in Tests.
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
  Engine.h / .cpp            MODIFIED — BackendKind::WasapiExclusive, requestedFormat params, probeFormat()
src/cli/main.cpp             MODIFIED — --backend, --format, probe subcommand
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
- Modify: `src/core/Engine.cpp` (include + construct new classes), `src/core/WinAudioCore.vcxproj`

**Interfaces:**
- Consumes: `IAudioBackend`, `RingBuffer`, `AudioFormat`, `ComUtil`.
- Produces (namespace `wa`):
  - `enum class WasapiMode { Shared, Exclusive };`
  - `class WasapiStream : public IAudioBackend` with `WasapiStream(WasapiMode, const AudioFormat* requested)`.
  - `class WasapiCaptureStream : public WasapiStream` with `WasapiCaptureStream(WasapiMode, const AudioFormat* requested)`.
  - `class WasapiRenderStream : public WasapiStream` with `WasapiRenderStream(WasapiMode, const AudioFormat* requested)`.
  - In THIS task the exclusive `prepareClient` branch returns a "not implemented" failure (filled in Task 3). Shared path is fully functional and equivalent to Phase 1.

> No new unit test: this is a behavior-preserving refactor. Verification is the existing 14 tests + clean build + a CLI Shared capture/play smoke confirming runtime parity.

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

- [ ] **Step 3: Update `src/core/Engine.cpp` to use the new classes**

Change the include (line 5) `#include "WasapiShared.h"` → `#include "WasapiStream.h"`. In `startCapture` change `backend_ = std::make_unique<WasapiSharedCapture>();` → `backend_ = std::make_unique<WasapiCaptureStream>(WasapiMode::Shared, nullptr);`. In `startPlayback` change `backend_ = std::make_unique<WasapiSharedRender>();` → `backend_ = std::make_unique<WasapiRenderStream>(WasapiMode::Shared, nullptr);`. (Engine's `BackendKind` param is still ignored in this task — Task 4 wires it.)

- [ ] **Step 4: Update the Core vcxproj and delete WasapiShared**

In `src/core/WinAudioCore.vcxproj`: remove the `<ClInclude Include="WasapiShared.h" />` and `<ClCompile Include="WasapiShared.cpp" />` items; add `<ClInclude Include="WasapiStream.h" />` and `<ClCompile Include="WasapiStream.cpp" />`. Then `git rm src/core/WasapiShared.h src/core/WasapiShared.cpp`.

- [ ] **Step 5: Build the solution**

Run: `& "...MSBuild.exe" WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: all 4 projects build /W4 clean (0 warnings/errors).

- [ ] **Step 6: Regression — existing tests + Shared runtime parity**

Run:
```powershell
& ".\x64\Debug\WinAudioTests.exe"                         # expect 20 PASSED (14 + 6 FormatSpec)
& ".\x64\Debug\WinAudioCli.exe" capture --out cap.wav --seconds 3
& ".\x64\Debug\WinAudioCli.exe" play --in cap.wav
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

## Task 4: Engine — BackendKind::WasapiExclusive, requestedFormat, probeFormat()

**Files:**
- Modify: `src/core/Engine.h`, `src/core/Engine.cpp`

**Interfaces:**
- Consumes: `WasapiStream` (`WasapiMode`, `WasapiCaptureStream`, `WasapiRenderStream`), `ComUtil`.
- Produces:
  - `enum class BackendKind { WasapiShared, WasapiExclusive };`
  - `Result startCapture(BackendKind, const DeviceId&, const std::wstring& wavPath, const AudioFormat* requested = nullptr);`
  - `Result startPlayback(BackendKind, const DeviceId&, const std::wstring& wavPath, const AudioFormat* requested = nullptr);`
  - `Result probeFormat(BackendKind, DataFlow, const DeviceId&, const AudioFormat&);`

- [ ] **Step 1: Edit `src/core/Engine.h`**

Change line 14 to:
```cpp
enum class BackendKind { WasapiShared, WasapiExclusive };
```
Change the two start signatures (lines 36-37) to:
```cpp
    Result startCapture(BackendKind kind, const DeviceId& id, const std::wstring& wavPath,
                        const AudioFormat* requested = nullptr);
    Result startPlayback(BackendKind kind, const DeviceId& id, const std::wstring& wavPath,
                         const AudioFormat* requested = nullptr);
    Result probeFormat(BackendKind kind, DataFlow flow, const DeviceId& id,
                       const AudioFormat& fmt);
```

- [ ] **Step 2: Edit `src/core/Engine.cpp` includes + start construction**

Change the include `#include "WasapiShared.h"` (already changed to `WasapiStream.h` in Task 2 — keep it). Add `#include <mmdeviceapi.h>` and `#include <audioclient.h>` after `#include "ComUtil.h"`.

In `startCapture`, replace the signature and the backend construction:
```cpp
Result Engine::startCapture(BackendKind kind, const DeviceId& id, const std::wstring& wavPath,
                            const AudioFormat* requested) {
    stop();
    try {
        ring_ = std::make_unique<RingBuffer>(kRingBytes);
        WasapiMode mode = (kind == BackendKind::WasapiExclusive) ? WasapiMode::Exclusive
                                                                 : WasapiMode::Shared;
        backend_ = std::make_unique<WasapiCaptureStream>(mode, requested);
        Result r = backend_->open(id, AudioFormat{}, ring_.get());
        if (!r) return r;
        r = backend_->start();
        if (!r) return r;
        running_.store(true);
        startTick_ = GetTickCount64();
        { std::lock_guard<std::mutex> lk(mtx_); status_ = {}; status_.state = EngineState::Capturing; }
        pump_ = std::thread(&Engine::captureLoop, this, wavPath);
        return Result::Ok();
    } catch (const std::exception& e) {
        stop();
        std::lock_guard<std::mutex> lk(mtx_);
        status_.state = EngineState::Error;
        status_.message = e.what();
        return Result::Fail(-1, e.what());
    }
}
```

In `startPlayback`, the same treatment:
```cpp
Result Engine::startPlayback(BackendKind kind, const DeviceId& id, const std::wstring& wavPath,
                             const AudioFormat* requested) {
    stop();
    try {
        ring_ = std::make_unique<RingBuffer>(kRingBytes);
        WasapiMode mode = (kind == BackendKind::WasapiExclusive) ? WasapiMode::Exclusive
                                                                 : WasapiMode::Shared;
        backend_ = std::make_unique<WasapiRenderStream>(mode, requested);
        Result r = backend_->open(id, AudioFormat{}, ring_.get());
        if (!r) return r;
        running_.store(true);
        startTick_ = GetTickCount64();
        { std::lock_guard<std::mutex> lk(mtx_); status_ = {}; status_.state = EngineState::Playing; }
        pump_ = std::thread(&Engine::playbackLoop, this, wavPath);
        return Result::Ok();
    } catch (const std::exception& e) {
        stop();
        std::lock_guard<std::mutex> lk(mtx_);
        status_.state = EngineState::Error;
        status_.message = e.what();
        return Result::Fail(-1, e.what());
    }
}
```

- [ ] **Step 3: Add `Engine::probeFormat` to `src/core/Engine.cpp`**

Add this method (e.g. right after `enumerate`):
```cpp
Result Engine::probeFormat(BackendKind kind, DataFlow flow, const DeviceId& id,
                           const AudioFormat& fmt) {
    ComInitGuard com;
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(e.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "probeFormat: CoCreateInstance");
    ComPtr<IMMDevice> dev;
    EDataFlow ef = (flow == DataFlow::Capture) ? eCapture : eRender;
    hr = id.empty() ? e->GetDefaultAudioEndpoint(ef, eConsole, &dev)
                    : e->GetDevice(id.c_str(), &dev);
    if (FAILED(hr)) return HrToResult(hr, "probeFormat: GetDevice");
    ComPtr<IAudioClient> client;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "probeFormat: Activate");
    AUDCLNT_SHAREMODE sm = (kind == BackendKind::WasapiExclusive)
                               ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
    WAVEFORMATEXTENSIBLE wfx = fmt.toWaveFormatExtensible();
    WAVEFORMATEX* closest = nullptr;
    hr = client->IsFormatSupported(sm, reinterpret_cast<WAVEFORMATEX*>(&wfx), &closest);
    if (closest) CoTaskMemFree(closest);
    if (hr == S_OK) return Result::Ok();
    if (hr == S_FALSE) return Result::Fail(1, "format not supported exactly (closest available)");
    return HrToResult(hr, "probeFormat: not supported");
}
```

- [ ] **Step 4: Build + regress**

Run: `& "...MSBuild.exe" WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m` then `& ".\x64\Debug\WinAudioTests.exe"`
Expected: /W4 clean; 20 tests PASS. (CLI/GUI still pass `BackendKind::WasapiShared` with the default `requested=nullptr`, so they compile unchanged.)

- [ ] **Step 5: Commit**

```powershell
git add src/core/Engine.h src/core/Engine.cpp
git commit -m "feat(core): Engine exclusive backend kind, requested format, probeFormat()"
```

---

## Task 5: CLI — --backend, --format, probe subcommand

**Files:**
- Modify: `src/cli/main.cpp`

**Interfaces:**
- Consumes: `Engine::startCapture/startPlayback(..., const AudioFormat*)`, `Engine::probeFormat`, `parseFormatSpec`.
- Produces: CLI accepts `--backend wasapi-shared|wasapi-exclusive` (default shared), `--format R/B/C[f]`, and a `probe` subcommand.

- [ ] **Step 1: Add helpers + includes to `src/cli/main.cpp`**

After the existing includes add `#include "FormatSpec.h"` and `#include <string>`. Add these helpers after `has(...)`:
```cpp
static std::string narrow(const std::wstring& w) {
    return std::string(w.begin(), w.end()); // format spec is ASCII
}
static BackendKind backendArg(int argc, wchar_t** argv) {
    std::wstring b = arg(argc, argv, L"--backend");
    return (b == L"wasapi-exclusive") ? BackendKind::WasapiExclusive
                                      : BackendKind::WasapiShared;
}
// Returns true and fills `fmt` if --format present & valid; false if absent; exits(2) if invalid.
static bool formatArg(int argc, wchar_t** argv, AudioFormat& fmt) {
    std::wstring f = arg(argc, argv, L"--format");
    if (f.empty()) return false;
    if (!parseFormatSpec(narrow(f), fmt)) {
        std::printf("invalid --format (want R/B/C[f], e.g. 48000/16/2)\n");
        std::exit(2);
    }
    return true;
}
```

- [ ] **Step 2: Update `usage()`**

```cpp
static void usage() {
    std::printf(
        "WinAudioCli list  [--render|--capture]\n"
        "WinAudioCli capture --out <file.wav> [--device <id>] [--seconds N]\n"
        "                    [--backend wasapi-shared|wasapi-exclusive] [--format 48000/16/2]\n"
        "WinAudioCli play    --in  <file.wav> [--device <id>]\n"
        "                    [--backend wasapi-shared|wasapi-exclusive]\n"
        "WinAudioCli probe   --format 48000/16/2 [--device <id>] [--render|--capture]\n"
        "                    [--backend wasapi-shared|wasapi-exclusive]\n");
}
```

- [ ] **Step 3: Wire backend/format into `capture` and `play`**

In the `capture` block, replace the `startCapture` call:
```cpp
        AudioFormat fmt{};
        bool haveFmt = formatArg(argc, argv, fmt);
        Result r = eng.startCapture(backendArg(argc, argv), id, out,
                                    haveFmt ? &fmt : nullptr);
```
In the `play` block, replace the `startPlayback` call:
```cpp
        AudioFormat fmt{};
        bool haveFmt = formatArg(argc, argv, fmt);
        Result r = eng.startPlayback(backendArg(argc, argv), id, in,
                                     haveFmt ? &fmt : nullptr);
```

- [ ] **Step 4: Add the `probe` subcommand**

Before the final `usage(); return 1;`, add:
```cpp
    if (cmd == L"probe") {
        AudioFormat fmt{};
        if (!formatArg(argc, argv, fmt)) { usage(); return 1; }
        std::wstring id = arg(argc, argv, L"--device");
        DataFlow flow = has(argc, argv, L"--capture") ? DataFlow::Capture : DataFlow::Render;
        Result r = eng.probeFormat(backendArg(argc, argv), flow, id, fmt);
        std::printf("%s: %s\n", r ? "SUPPORTED" : "NOT SUPPORTED", r.message.c_str());
        return r ? 0 : 1;
    }
```
(`eng` is already declared above the `capture` block; `probe` uses the same `Engine eng;`.)

- [ ] **Step 5: Build**

Run: `& "...MSBuild.exe" WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: /W4 clean.

- [ ] **Step 6: Manual CLI verification (real hardware)**

Run:
```powershell
& ".\x64\Debug\WinAudioCli.exe" probe --capture --format 48000/16/2 --backend wasapi-exclusive
& ".\x64\Debug\WinAudioCli.exe" probe --capture --format 99999/16/2 --backend wasapi-exclusive
& ".\x64\Debug\WinAudioCli.exe" capture --out exc.wav --seconds 3 --backend wasapi-exclusive --format 48000/16/2
& ".\x64\Debug\WinAudioCli.exe" play --in exc.wav --backend wasapi-exclusive --format 48000/16/2
& ".\x64\Debug\WinAudioCli.exe" capture --out sh.wav --seconds 2   # Shared still works
```
Expected: first probe SUPPORTED (or a sensible NOT SUPPORTED if the device lacks 48k/16/2 exclusive — try 44100/16/2); the bogus rate NOT SUPPORTED; exclusive capture writes `exc.wav` with a **16-bit/48000** header (the requested exclusive format, not the mix format); exclusive play completes; Shared capture unchanged. Do not commit the `.wav` files. If exclusive capture fails with `AUDCLNT_E_DEVICE_IN_USE`, close other apps using the device and retry; report the outcome.

- [ ] **Step 7: Commit**

```powershell
git add src/cli/main.cpp
git commit -m "feat(cli): --backend, --format, and probe subcommand"
```

---

## Task 6: GUI — backend combo, format controls, probe button

**Files:**
- Modify: `src/gui/AppUi.h`, `src/gui/AppUi.cpp`

**Interfaces:**
- Consumes: `Engine::startCapture/startPlayback(..., const AudioFormat*)`, `Engine::probeFormat`, `BackendKind`.
- Produces: GUI backend combo has `WASAPI-Shared` + `WASAPI-Exclusive`; when Exclusive is selected, format controls + a "Probe format" button are active and the chosen format is passed to start/probe.

- [ ] **Step 1: Add state fields to `src/gui/AppUi.h`**

Add these private members (alongside the existing ones):
```cpp
    int  rateIdx_ = 0;   // index into kRates
    int  bitsIdx_ = 0;   // index into kBits
    int  chIdx_ = 1;     // index into kChannels (default 2ch)
    bool isFloat_ = false;
```

- [ ] **Step 2: Build the requested-format + backend logic in `src/gui/AppUi.cpp`**

At file scope (after the `utow` helper) add:
```cpp
static const int   kRates[] = {44100, 48000, 96000};
static const char* kRatesS[] = {"44100", "48000", "96000"};
static const int   kBits[]  = {16, 24, 32};
static const char* kBitsS[] = {"16", "24", "32"};
static const int   kChans[] = {1, 2};
static const char* kChansS[]= {"1", "2"};
```

- [ ] **Step 3: Render backend combo + format controls in `AppUi::draw`**

Replace the existing backend combo block:
```cpp
    const char* backends[] = {"WASAPI-Shared"};
    ImGui::Combo("Backend", &backendIdx_, backends, 1);
```
with:
```cpp
    const char* backends[] = {"WASAPI-Shared", "WASAPI-Exclusive"};
    ImGui::Combo("Backend", &backendIdx_, backends, 2);
    const bool exclusive = (backendIdx_ == 1);
```
Then, right BEFORE the `ImGui::InputText("WAV file", ...)` line, add the format controls + probe button (disabled unless exclusive):
```cpp
    if (!exclusive) ImGui::BeginDisabled();
    ImGui::Combo("Rate", &rateIdx_, kRatesS, IM_ARRAYSIZE(kRatesS)); ImGui::SameLine();
    ImGui::Combo("Bits", &bitsIdx_, kBitsS, IM_ARRAYSIZE(kBitsS)); ImGui::SameLine();
    ImGui::Combo("Ch", &chIdx_, kChansS, IM_ARRAYSIZE(kChansS)); ImGui::SameLine();
    ImGui::Checkbox("float", &isFloat_);
    if (ImGui::Button("Probe format")) {
        wa::AudioFormat f{};
        f.sampleRate = kRates[rateIdx_]; f.bitsPerSample = (uint16_t)kBits[bitsIdx_];
        f.channels = (uint16_t)kChans[chIdx_]; f.isFloat = isFloat_;
        wa::DeviceId id = devices_.empty() ? L"" : devices_[deviceIdx_].id;
        wa::DataFlow flow = (flowIdx_ == 0) ? wa::DataFlow::Capture : wa::DataFlow::Render;
        wa::Result pr = eng.probeFormat(wa::BackendKind::WasapiExclusive, flow, id, f);
        logLines_.push_back(std::string("probe ") + (pr ? "SUPPORTED" : "NOT SUPPORTED: " + pr.message));
    }
    if (!exclusive) ImGui::EndDisabled();
```

- [ ] **Step 4: Pass backend + format to Start**

Replace the Start handler body:
```cpp
        if (ImGui::Button("Start")) {
            wa::DeviceId id = devices_.empty() ? L"" : devices_[deviceIdx_].id;
            std::wstring path = utow(wavPath_);
            wa::BackendKind kind = exclusive ? wa::BackendKind::WasapiExclusive
                                             : wa::BackendKind::WasapiShared;
            wa::AudioFormat f{};
            f.sampleRate = kRates[rateIdx_]; f.bitsPerSample = (uint16_t)kBits[bitsIdx_];
            f.channels = (uint16_t)kChans[chIdx_]; f.isFloat = isFloat_;
            const wa::AudioFormat* req = exclusive ? &f : nullptr;
            wa::Result r = (flowIdx_ == 0)
                ? eng.startCapture(kind, id, path, req)
                : eng.startPlayback(kind, id, path, req);
            logLines_.push_back(r ? "started" : ("error: " + r.message));
        }
```

- [ ] **Step 5: Build**

Run: `& "...MSBuild.exe" WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m`
Expected: clean build; `src/gui/*.cpp` warning-clean at /W4 (ImGui TUs remain suppressed).

- [ ] **Step 6: GUI liveness check**

Run:
```powershell
$p = Start-Process .\x64\Debug\WinAudioGui.exe -PassThru; Start-Sleep 3
if(!$p.HasExited){ "started OK"; Stop-Process $p } else { "EXITED code=$($p.ExitCode)" }
```
Expected: "started OK". (Full visual check — selecting Exclusive enables Rate/Bits/Ch/float + Probe; Probe logs SUPPORTED/NOT — is part of Task 7's visual acceptance.)

- [ ] **Step 7: Commit**

```powershell
git add src/gui/AppUi.h src/gui/AppUi.cpp
git commit -m "feat(gui): backend selector, exclusive format controls, probe button"
```

---

## Task 7: Docs + full hardware smoke + Release

**Files:**
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: the now-complete Shared + Exclusive backends.
- Produces: CLAUDE.md reflects two WASAPI modes; Release build + tests verified; Shared/Exclusive hardware smoke recorded.

- [ ] **Step 1: Update `CLAUDE.md`**

In the capability list, change the WASAPI line to note **both** modes: "WASAPI-Shared（共享）与 WASAPI-Exclusive（独占，需 IsFormatSupported 格式探测）". Update the "后续阶段" list to remove WASAPI-Exclusive (now done) — leave waveIn/waveOut and resampling as future. Add the new CLI forms to the run section: `probe`, and `capture/play` with `--backend`/`--format`.

- [ ] **Step 2: Release build (all 4 projects)**

Run: `& "...MSBuild.exe" WinAudio.sln /p:Configuration=Release /p:Platform=x64 /m`
Expected: 0 errors across all 4 projects.

- [ ] **Step 3: Full test suite (Release)**

Run: `& ".\x64\Release\WinAudioTests.exe"`
Expected: 20 tests PASS.

- [ ] **Step 4: Hardware smoke — Shared regression + Exclusive**

Run:
```powershell
& ".\x64\Release\WinAudioCli.exe" capture --out sh.wav --seconds 2                                  # Shared
& ".\x64\Release\WinAudioCli.exe" play   --in  sh.wav
& ".\x64\Release\WinAudioCli.exe" probe  --capture --format 48000/16/2 --backend wasapi-exclusive
& ".\x64\Release\WinAudioCli.exe" capture --out exc.wav --seconds 2 --backend wasapi-exclusive --format 48000/16/2
& ".\x64\Release\WinAudioCli.exe" play   --in  exc.wav --backend wasapi-exclusive --format 48000/16/2
```
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
- Engine BackendKind::WasapiExclusive + requestedFormat + probeFormat — Task 4. ✅
- CLI --backend/--format/probe — Task 5. ✅
- GUI backend combo + format controls + probe button — Task 6. ✅
- Tests: aligned duration, format fallback selection, --format parse — Task 1. ✅
- Shared regression (14 → 20 gtest + CLI/GUI smoke) — Tasks 2, 4, 7. ✅
- Docs — Task 7. ✅

**Out of scope (later phases, per spec §7):** waveIn/waveOut; latency/glitch numeric measurement; loopback signal compare; full format-probe UI panel (only the probe button entry here); resampling.

**Type consistency:** `WasapiMode`, `WasapiStream`/`WasapiCaptureStream`/`WasapiRenderStream` ctor `(WasapiMode, const AudioFormat*)`; `BackendKind::{WasapiShared,WasapiExclusive}`; `Engine::startCapture/startPlayback(..., const AudioFormat* requested=nullptr)`; `Engine::probeFormat(BackendKind, DataFlow, const DeviceId&, const AudioFormat&)`; `parseFormatSpec(const std::string&, AudioFormat&)`; `alignedBufferDuration100ns(uint32_t,uint32_t)`; `selectSupportedFormat(vector, function)`; `defaultExclusiveCaptureCandidates()` — used consistently across Tasks 1–6. `Result::Fail(long,std::string)` everywhere (no `Result::Error`). Protected members (`ring_`, `actualFormat_`, `bufferFrames_`, `frameBytes_`, `running_`, `hEvent_`, `client_`) are accessed by subclass `runLoop`/`preRoll`/`createService`. ✅

**Equivalence risk (Shared):** Task 2 keeps the 100ms buffer, event flags, the synchronous-start handshake, and the HRESULT guards verbatim in the base scaffold; regression Step 6 (20 tests + Shared CLI capture/play with correct mix-format header) is the gate before exclusive code lands.
