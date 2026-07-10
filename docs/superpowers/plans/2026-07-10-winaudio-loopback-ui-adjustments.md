# WinAudio Loopback UI Adjustments Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现已确认的 Loopback UI 调整：两个 tab 底部全宽日志，以及默认开启、可关闭、可观察的 System Loopback silent render helper。

**Architecture:** 用一个小的 `LoopbackOptions` 结构贯穿 CLI、GUI、`Engine` 和 `MonitorEngine`。新增专用 `WasapiSilentRenderStream` 负责提交 `AUDCLNT_BUFFERFLAGS_SILENT`，避免复用普通 playback render 路径导致语义混淆。`MonitorEngine` 增加可测试的 silent render factory seam；`Engine` 走真实 WASAPI helper，用日志暴露状态。

**Tech Stack:** C++17、WASAPI、Dear ImGui、CMake、GoogleTest、PowerShell。

---

## 文件结构

- 修改 `src/core/IAudioBackend.h`：新增 `LoopbackOptions`，不放 GUI 或 monitor 状态字段。
- 修改 `src/core/WasapiStream.h` / `src/core/WasapiStream.cpp`：新增 `WasapiSilentRenderStream`，专门用于 shared-mode silent render。
- 修改 `src/core/Engine.h` / `src/core/Engine.cpp`：`capture --loopback` 路径按 `LoopbackOptions` 启停 silent render helper。
- 修改 `src/core/MonitorEngine.h` / `src/core/MonitorEngine.cpp`：`monitor --loopback` 和 GUI Loopback 页按 `LoopbackOptions` 启停 silent render helper，并通过 `MonitorStatus` 暴露状态。
- 新增 `src/cli/CliOptions.h`：只放可单测的 CLI loopback options 解析。
- 修改 `src/cli/main.cpp`：usage 文案和 `capture` / `monitor` 参数传递。
- 修改 `src/gui/AppUi.h` / `src/gui/AppUi.cpp`：Loopback checkbox、状态显示、两个 tab 底部全宽日志布局。
- 修改 `src/tests/test_loopback.cpp`：`LoopbackOptions` 和 `WasapiSilentRenderStream` 基础测试。
- 修改 `src/tests/test_monitorengine.cpp`：fake seam 覆盖 silent render 默认开启、关闭、失败非 fatal。
- 新增 `src/tests/test_cli_options.cpp`：覆盖 `--no-silent-render` 解析。
- 修改 `src/tests/CMakeLists.txt`：注册 `test_cli_options.cpp`，并让 tests 能 include `src/cli`。
- 不修改 `docs/superpowers/specs/2026-07-10-winaudio-loopback-ui-adjustments-design.md`，除非实现中发现 spec 与现实冲突。

---

### Task 1: LoopbackOptions 与 CLI 解析测试

**Files:**
- Modify: `src/core/IAudioBackend.h`
- Create: `src/cli/CliOptions.h`
- Create: `src/tests/test_cli_options.cpp`
- Modify: `src/tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `test_cli_options.cpp`**

创建 `src/tests/test_cli_options.cpp`：

```cpp
#include <gtest/gtest.h>
#include "CliOptions.h"

TEST(CliOptions, LoopbackSilentRenderDefaultsEnabled) {
    const wchar_t* argv[] = {L"WinAudioCli", L"capture", L"--loopback"};

    wa::LoopbackOptions opts =
        wa::cli::parseLoopbackOptions(3, const_cast<wchar_t**>(argv));

    EXPECT_TRUE(opts.silentRender);
}

TEST(CliOptions, NoSilentRenderDisablesHelper) {
    const wchar_t* argv[] = {
        L"WinAudioCli", L"monitor", L"--loopback", L"--no-silent-render"
    };

    wa::LoopbackOptions opts =
        wa::cli::parseLoopbackOptions(4, const_cast<wchar_t**>(argv));

    EXPECT_FALSE(opts.silentRender);
}
```

- [ ] **Step 2: 注册测试并运行，确认失败**

修改 `src/tests/CMakeLists.txt`：

```cmake
add_executable(WinAudioTests
    test_delayfifo.cpp
    test_streamparams.cpp
    test_smoke.cpp
    test_audioformat.cpp
    test_fft.cpp
    test_ringbuffer.cpp
    test_sampleconvert.cpp
    test_wavfile.cpp
    test_formatspec.cpp
    test_scopebuffer.cpp
    test_analysis.cpp
    test_monitorengine.cpp
    test_spectrogram.cpp
    test_capabilities.cpp
    test_log.cpp
    test_loopback.cpp
    test_cli_options.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/Spectrogram.cpp
)
target_include_directories(WinAudioTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/gui
    ${CMAKE_SOURCE_DIR}/src/cli
)
```

运行：

```powershell
cmake --build build --config Debug --target WinAudioTests -j
```

Expected: 编译失败，错误包含 `Cannot open include file: 'CliOptions.h'` 或 `parseLoopbackOptions` 未声明。

- [ ] **Step 3: 实现 `LoopbackOptions`**

在 `src/core/IAudioBackend.h` 的 `CaptureSource` 后添加：

```cpp
struct LoopbackOptions {
    bool silentRender = true;
};
```

- [ ] **Step 4: 实现 `src/cli/CliOptions.h`**

创建 `src/cli/CliOptions.h`：

```cpp
#pragma once
#include <cwchar>
#include "IAudioBackend.h"

namespace wa::cli {

inline bool hasArg(int argc, wchar_t** argv, const wchar_t* key) {
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], key) == 0) return true;
    }
    return false;
}

inline LoopbackOptions parseLoopbackOptions(int argc, wchar_t** argv) {
    LoopbackOptions opts{};
    opts.silentRender = !hasArg(argc, argv, L"--no-silent-render");
    return opts;
}

} // namespace wa::cli
```

- [ ] **Step 5: 运行 focused 测试，确认通过**

```powershell
cmake --build build --config Debug --target WinAudioTests -j
.\build\bin\Debug\WinAudioTests.exe --gtest_filter=CliOptions.*
```

Expected: `2 tests` 通过。

- [ ] **Step 6: 提交 Task 1**

```powershell
git add src/core/IAudioBackend.h src/cli/CliOptions.h src/tests/test_cli_options.cpp src/tests/CMakeLists.txt
git commit -m "feat(cli): add loopback silent render option parsing"
```

---

### Task 2: 新增 WasapiSilentRenderStream

**Files:**
- Modify: `src/core/WasapiStream.h`
- Modify: `src/core/WasapiStream.cpp`
- Modify: `src/tests/test_loopback.cpp`

- [ ] **Step 1: 写失败测试**

在 `src/tests/test_loopback.cpp` 末尾添加：

```cpp
TEST(WasapiSilentRenderStream, RejectsExclusiveInOpen) {
    WasapiSilentRenderStream stream(WasapiMode::Exclusive, nullptr);

    Result r = stream.open(L"", AudioFormat{}, nullptr, StreamParams{});

    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_NE(r.message.find("silent render"), std::string::npos);
    EXPECT_NE(r.message.find("Shared"), std::string::npos);
}
```

- [ ] **Step 2: 运行测试，确认失败**

```powershell
cmake --build build --config Debug --target WinAudioTests -j
```

Expected: 编译失败，错误包含 `WasapiSilentRenderStream` 未声明。

- [ ] **Step 3: 声明 `WasapiSilentRenderStream`**

在 `src/core/WasapiStream.h` 的 `WasapiRenderStream` 后添加：

```cpp
class WasapiSilentRenderStream : public WasapiStream {
public:
    WasapiSilentRenderStream(WasapiMode mode, const AudioFormat* requested);
    ~WasapiSilentRenderStream() override;
    Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring,
                const StreamParams& params) override;
protected:
    EDataFlow dataFlow() const override { return eRender; }
    Result createService() override;
    void   preRoll() override;
    void   runLoop() override;
    void   resetService() override { render_.Reset(); }
private:
    ComPtr<IAudioRenderClient> render_;
};
```

- [ ] **Step 4: 实现构造、析构、open 和 createService**

在 `src/core/WasapiStream.cpp` 的 `WasapiRenderStream` 实现后添加：

```cpp
WasapiSilentRenderStream::WasapiSilentRenderStream(WasapiMode mode,
                                                   const AudioFormat* requested)
    : WasapiStream(mode, requested) {}

WasapiSilentRenderStream::~WasapiSilentRenderStream() { close(); }

Result WasapiSilentRenderStream::open(const DeviceId& id, const AudioFormat& fmt,
                                      RingBuffer* ring, const StreamParams& params) {
    if (isExclusive()) {
        return Result::Fail(-1, "WasapiSilentRenderStream: silent render requires WASAPI-Shared");
    }
    return WasapiStream::open(id, fmt, ring, params);
}

Result WasapiSilentRenderStream::createService() {
    HRESULT hr = client_->GetService(__uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(render_.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "WasapiSilentRenderStream",
           "GetService(IAudioRenderClient)", "", wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "WasapiSilentRenderStream",
               "GetService(IAudioRenderClient)", "", wa::log::hrName(hr));
        return HrToResult(hr, "WasapiSilentRenderStream: GetService");
    }
    return Result::Ok();
}
```

- [ ] **Step 5: 实现 silent pre-roll 和 run loop**

在同一实现块继续添加：

```cpp
void WasapiSilentRenderStream::preRoll() {
    BYTE* buf = nullptr;
    HRESULT hrPre = render_->GetBuffer(bufferFrames_, &buf);
    WA_LOG(wa::log::Level::Debug, "WasapiSilentRenderStream", "GetBuffer(preRoll)",
           "frames=" + std::to_string(bufferFrames_), wa::log::hrName(hrPre));
    if (SUCCEEDED(hrPre)) {
        HRESULT hrRel = render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
        WA_LOG(wa::log::Level::Debug, "WasapiSilentRenderStream",
               "ReleaseBuffer(preRoll)", "", wa::log::hrName(hrRel));
    }
}

void WasapiSilentRenderStream::runLoop() {
    wa::log::setThreadName("silR");
    WA_LOG(wa::log::Level::Info, "WasapiSilentRenderStream", "runLoop",
           "silent render loop started", "");
    while (running_.load()) {
        DWORD waitRc = WaitForSingleObject(static_cast<HANDLE>(hEvent_), 200);
        wa::log::emitTrace("WasapiSilentRenderStream", "WaitForSingleObject", 0, waitRc, 0);
        if (!running_.load()) break;

        UINT32 padding = 0;
        HRESULT hrPad = client_->GetCurrentPadding(&padding);
        wa::log::emitTrace("WasapiSilentRenderStream", "GetCurrentPadding", padding, 0,
                           static_cast<long>(hrPad));
        if (FAILED(hrPad)) break;

        UINT32 frames = bufferFrames_ - padding;
        if (frames == 0) continue;

        BYTE* buf = nullptr;
        HRESULT hrGB = render_->GetBuffer(frames, &buf);
        wa::log::emitTrace("WasapiSilentRenderStream", "GetBuffer", frames, 0,
                           static_cast<long>(hrGB));
        if (FAILED(hrGB)) break;

        HRESULT hrRB = render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
        wa::log::emitTrace("WasapiSilentRenderStream", "ReleaseBuffer", frames,
                           AUDCLNT_BUFFERFLAGS_SILENT, static_cast<long>(hrRB));
        if (FAILED(hrRB)) break;
    }
}
```

- [ ] **Step 6: 运行 loopback focused 测试**

```powershell
cmake --build build --config Debug --target WinAudioTests -j
.\build\bin\Debug\WinAudioTests.exe --gtest_filter=WasapiSystemLoopbackCaptureStream.*:WasapiSilentRenderStream.*
```

Expected: `WasapiSilentRenderStream.RejectsExclusiveInOpen` 和现有 loopback helper 测试通过。

- [ ] **Step 7: 提交 Task 2**

```powershell
git add src/core/WasapiStream.h src/core/WasapiStream.cpp src/tests/test_loopback.cpp
git commit -m "feat(core): add silent render stream"
```

---

### Task 3: MonitorEngine silent render helper

**Files:**
- Modify: `src/core/MonitorEngine.h`
- Modify: `src/core/MonitorEngine.cpp`
- Modify: `src/tests/test_monitorengine.cpp`

- [ ] **Step 1: 写失败测试：默认开启 helper**

在 `FakeRig` 中新增字段和 factory 之前，先添加测试让编译失败：

```cpp
TEST(MonitorEngine, LoopbackStartsSilentRenderByDefault) {
    FakeRig rig;
    MonitorEngine eng(rig.factory(), rig.silentFactory());
    CaptureSource source{CaptureSourceKind::SystemLoopback, L"loopback-render-id"};

    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, source, L"", 50, false));

    EXPECT_EQ(rig.silentOpenCount.load(), 1);
    EXPECT_EQ(eng.poll().silentRenderState, StreamState::Running);
    eng.stop();
}

TEST(MonitorEngine, LoopbackCanDisableSilentRender) {
    FakeRig rig;
    MonitorEngine eng(rig.factory(), rig.silentFactory());
    CaptureSource source{CaptureSourceKind::SystemLoopback, L"loopback-render-id"};
    LoopbackOptions opts{};
    opts.silentRender = false;

    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, source, L"", 50, false,
                          {}, {}, nullptr, opts));

    EXPECT_EQ(rig.silentOpenCount.load(), 0);
    EXPECT_EQ(eng.poll().silentRenderState, StreamState::Idle);
    eng.stop();
}

TEST(MonitorEngine, SilentRenderFailureDoesNotFailLoopbackCapture) {
    FakeRig rig;
    rig.silentFailStart = true;
    MonitorEngine eng(rig.factory(), rig.silentFactory());
    CaptureSource source{CaptureSourceKind::SystemLoopback, L"loopback-render-id"};

    Result r = eng.start(BackendKind::WasapiShared, source, L"", 50, false);

    EXPECT_TRUE(r);
    EXPECT_EQ(rig.capOpenCount.load(), 1);
    EXPECT_EQ(rig.silentOpenCount.load(), 1);
    EXPECT_EQ(eng.poll().overall, StreamState::Running);
    EXPECT_EQ(eng.poll().silentRenderState, StreamState::Error);
    eng.stop();
}
```

- [ ] **Step 2: 扩展 FakeRig 让测试可编译到缺少产品签名处**

在 `FakeRig` 中添加字段：

```cpp
    bool              silentFailStart = false;
    std::atomic<bool> silentStopped{false};
    std::atomic<int>  silentOpenCount{0};
    FakeBackend*      silentPtr = nullptr;
```

在 `FakeRig` 中添加 factory：

```cpp
    MonitorEngine::SilentRenderFactory silentFactory() {
        return [this](const AudioFormat* req) -> std::unique_ptr<IAudioBackend> {
            silentOpenCount.fetch_add(1, std::memory_order_relaxed);
            auto b = std::make_unique<FakeBackend>(renderFmt, silentFailStart, &silentStopped);
            silentPtr = b.get();
            if (req) { b->lastRequested_ = *req; b->sawRequested_ = true; }
            return b;
        };
    }
```

运行：

```powershell
cmake --build build --config Debug --target WinAudioTests -j
```

Expected: 编译失败，错误包含 `SilentRenderFactory`、`silentRenderState` 或新 `start(...)` 参数未声明。

- [ ] **Step 3: 扩展 `MonitorStatus`、factory seam 和 start 签名**

在 `src/core/MonitorEngine.h` 的 `MonitorStatus` 中添加：

```cpp
    StreamState silentRenderState = StreamState::Idle;
```

在 `MonitorEngine` public 区添加 factory alias，并更新构造函数：

```cpp
    using SilentRenderFactory =
        std::function<std::unique_ptr<IAudioBackend>(const AudioFormat*)>;

    explicit MonitorEngine(BackendFactory factory = {},
                           SilentRenderFactory silentFactory = {});
```

更新 `start` 签名：

```cpp
    Result start(BackendKind kind, const CaptureSource& capSource, const DeviceId& renderId,
                 uint32_t delayMs, bool playbackEnabled = true,
                 const StreamParams& capParams = {}, const StreamParams& renderParams = {},
                 const AudioFormat* capFormat = nullptr,
                 const LoopbackOptions& loopbackOptions = {});
```

更新 legacy overload：

```cpp
        return start(kind, CaptureSource{CaptureSourceKind::Endpoint, capId}, renderId,
                     delayMs, playbackEnabled, capParams, renderParams, capFormat, {});
```

在 private 区添加：

```cpp
    std::unique_ptr<IAudioBackend> makeSilentRenderBackend(const AudioFormat* requested);
    Result startSilentRenderIfNeeded();
    void   stopSilentRender();
```

在成员区添加：

```cpp
    SilentRenderFactory silentFactory_;
    LoopbackOptions loopbackOptions_{};
    std::unique_ptr<IAudioBackend> silentRenderBackend_;
    std::atomic<StreamState> silentRenderState_{StreamState::Idle};
```

- [ ] **Step 4: 实现 MonitorEngine helper**

在 `src/core/MonitorEngine.cpp` 更新构造函数：

```cpp
MonitorEngine::MonitorEngine(BackendFactory factory, SilentRenderFactory silentFactory)
    : factory_(std::move(factory)), silentFactory_(std::move(silentFactory)) {}
```

添加 helper：

```cpp
std::unique_ptr<IAudioBackend> MonitorEngine::makeSilentRenderBackend(
    const AudioFormat* requested) {
    if (silentFactory_) return silentFactory_(requested);
    return std::make_unique<WasapiSilentRenderStream>(WasapiMode::Shared, requested);
}

Result MonitorEngine::startSilentRenderIfNeeded() {
    silentRenderState_.store(StreamState::Idle, std::memory_order_relaxed);
    if (capSource_.kind != CaptureSourceKind::SystemLoopback || !loopbackOptions_.silentRender)
        return Result::Ok();

    WA_LOG(wa::log::Level::Info, "MonitorEngine", "silentRender.start",
           "dev=" + wa::narrowAscii(capSource_.deviceId), "requested");

    silentRenderBackend_ = makeSilentRenderBackend(nullptr);
    if (!silentRenderBackend_) {
        silentRenderState_.store(StreamState::Error, std::memory_order_relaxed);
        return Result::Fail(-1, "MonitorEngine: silent render factory null");
    }

    Result r = silentRenderBackend_->open(capSource_.deviceId, AudioFormat{}, nullptr, {});
    if (!r) {
        WA_LOG(wa::log::Level::Err, "MonitorEngine", "silentRender.open", "", r.message);
        silentRenderBackend_.reset();
        silentRenderState_.store(StreamState::Error, std::memory_order_relaxed);
        return r;
    }

    r = silentRenderBackend_->start();
    if (!r) {
        WA_LOG(wa::log::Level::Err, "MonitorEngine", "silentRender.start", "", r.message);
        silentRenderBackend_->stop();
        silentRenderBackend_.reset();
        silentRenderState_.store(StreamState::Error, std::memory_order_relaxed);
        return r;
    }

    silentRenderState_.store(StreamState::Running, std::memory_order_relaxed);
    WA_LOG(wa::log::Level::Info, "MonitorEngine", "silentRender.started", "", "ok");
    return Result::Ok();
}

void MonitorEngine::stopSilentRender() {
    if (silentRenderBackend_) {
        WA_LOG(wa::log::Level::Info, "MonitorEngine", "silentRender.stop", "", "");
        silentRenderBackend_->stop();
        silentRenderBackend_.reset();
    }
    if (silentRenderState_.load(std::memory_order_relaxed) == StreamState::Running)
        silentRenderState_.store(StreamState::Idle, std::memory_order_relaxed);
}
```

- [ ] **Step 5: 接入 start、teardown 和 poll**

在 `start(...)` 开头状态 reset 区添加：

```cpp
    silentRenderState_.store(StreamState::Idle, std::memory_order_relaxed);
```

在 session 参数赋值处改成：

```cpp
    kind_ = kind;
    capSource_ = capSource;
    renderId_ = renderId;
    delayMs_ = delayMs;
    loopbackOptions_ = loopbackOptions;
```

在 capture started、`capFmt_` 已经有效之后，启动 helper；失败只记录不 rollback：

```cpp
    if (Result sr = startSilentRenderIfNeeded(); !sr) {
        WA_LOG(wa::log::Level::Warn, "MonitorEngine", "silentRender.unavailable",
               "", sr.message);
    }
```

在 `teardown()` 中，pump join 后、停止 capture/render 前添加：

```cpp
    stopSilentRender();
```

在 `poll()` 中添加：

```cpp
    s.silentRenderState = silentRenderState_.load(std::memory_order_relaxed);
```

- [ ] **Step 6: 运行 focused 测试**

```powershell
cmake --build build --config Debug --target WinAudioTests -j
.\build\bin\Debug\WinAudioTests.exe --gtest_filter=MonitorEngine.LoopbackStartsSilentRenderByDefault:MonitorEngine.LoopbackCanDisableSilentRender:MonitorEngine.SilentRenderFailureDoesNotFailLoopbackCapture:MonitorEngine.LoopbackCaptureSourceReachesFactory
```

Expected: 4 个测试通过。旧 `LoopbackCaptureSourceReachesFactory` 需要保留 `renderOpenCount == 0`，因为 silent helper 不应复用 playback render factory。

- [ ] **Step 7: 提交 Task 3**

```powershell
git add src/core/MonitorEngine.h src/core/MonitorEngine.cpp src/tests/test_monitorengine.cpp
git commit -m "feat(core): wire silent render into loopback monitor"
```

---

### Task 4: Engine 与 CLI 接入 silent render options

**Files:**
- Modify: `src/core/Engine.h`
- Modify: `src/core/Engine.cpp`
- Modify: `src/cli/main.cpp`

- [ ] **Step 1: 扩展 Engine API**

在 `src/core/Engine.h` 中更新 `startCapture`：

```cpp
    Result startCapture(BackendKind kind, const CaptureSource& source, const std::wstring& wavPath,
                        const AudioFormat* requested = nullptr,
                        const LoopbackOptions& loopbackOptions = {});
    Result startCapture(BackendKind kind, const DeviceId& id, const std::wstring& wavPath,
                        const AudioFormat* requested = nullptr) {
        return startCapture(kind, CaptureSource{CaptureSourceKind::Endpoint, id}, wavPath,
                            requested, {});
    }
```

在 private 区添加：

```cpp
    Result startSilentRenderForLoopback(const CaptureSource& source,
                                        const LoopbackOptions& loopbackOptions);
    void   stopSilentRender();
```

在成员区添加：

```cpp
    std::unique_ptr<IAudioBackend> silentRenderBackend_;
```

- [ ] **Step 2: 实现 Engine helper**

在 `src/core/Engine.cpp` 中更新 `startCapture` 签名，并在 capture backend start 成功后、设置 `running_` 前插入：

```cpp
        if (Result sr = startSilentRenderForLoopback(source, loopbackOptions); !sr) {
            WA_LOG(wa::log::Level::Warn, "Engine", "silentRender.unavailable",
                   "", sr.message);
        }
```

添加 helper：

```cpp
Result Engine::startSilentRenderForLoopback(const CaptureSource& source,
                                            const LoopbackOptions& loopbackOptions) {
    if (source.kind != CaptureSourceKind::SystemLoopback || !loopbackOptions.silentRender)
        return Result::Ok();

    WA_LOG(wa::log::Level::Info, "Engine", "silentRender.start",
           "dev=" + (source.deviceId.empty() ? std::string("(default)")
                                             : wa::narrowAscii(source.deviceId)),
           "requested");

    silentRenderBackend_ = std::make_unique<WasapiSilentRenderStream>(WasapiMode::Shared, nullptr);
    Result r = silentRenderBackend_->open(source.deviceId, AudioFormat{}, nullptr, {});
    if (!r) {
        silentRenderBackend_.reset();
        return r;
    }
    r = silentRenderBackend_->start();
    if (!r) {
        silentRenderBackend_->stop();
        silentRenderBackend_.reset();
        return r;
    }
    WA_LOG(wa::log::Level::Info, "Engine", "silentRender.started", "", "ok");
    return Result::Ok();
}

void Engine::stopSilentRender() {
    if (silentRenderBackend_) {
        WA_LOG(wa::log::Level::Info, "Engine", "silentRender.stop", "", "");
        silentRenderBackend_->stop();
        silentRenderBackend_.reset();
    }
}
```

在 `Engine::stop()` 中，join pump 后、停止 main backend 前添加：

```cpp
    stopSilentRender();
```

- [ ] **Step 3: 接入 CLI usage 与参数传递**

在 `src/cli/main.cpp` 添加 include：

```cpp
#include "CliOptions.h"
```

更新 usage 中 capture/monitor 行：

```cpp
        "WinAudioCli capture --out <file.wav> [--device <id>] [--seconds N] [--loopback]\n"
        "                    [--no-silent-render] [--backend wasapi-shared|wasapi-exclusive]\n"
        "                    [--format 48000/16/2] (both backends)\n"
```

```cpp
        "WinAudioCli monitor [--loopback] [--cap <id>] [--render <id>] [--delay-ms N] [--seconds N]\n"
        "                    [--no-silent-render] [--backend wasapi-shared|wasapi-exclusive]\n"
        "                    [--format R/B/C[f]]\n"
```

在 `capture` 命令中创建 options 并传给 `Engine`：

```cpp
        LoopbackOptions loopbackOptions = wa::cli::parseLoopbackOptions(argc, argv);
        CaptureSource source{loopback ? CaptureSourceKind::SystemLoopback : CaptureSourceKind::Endpoint, id};
        Result r = eng.startCapture(kind, source, out, haveFmt ? &fmt : nullptr,
                                    loopbackOptions);
```

在 `monitor` 命令中传给 `MonitorEngine`：

```cpp
        LoopbackOptions loopbackOptions = wa::cli::parseLoopbackOptions(argc, argv);
        wa::Result r = mon.start(kind, source, renderId, delayMs,
                                 true, {}, {}, haveFmt ? &capFmt : nullptr,
                                 loopbackOptions);
```

- [ ] **Step 4: 构建 CLI**

```powershell
cmake --build build --config Debug --target WinAudioCli -j
.\build\bin\Debug\WinAudioCli.exe
```

Expected: 构建通过；无参数运行打印 usage，usage 包含 `--no-silent-render`。

- [ ] **Step 5: 运行 core focused 测试**

```powershell
cmake --build build --config Debug --target WinAudioTests -j
.\build\bin\Debug\WinAudioTests.exe --gtest_filter=CliOptions.*:WasapiSilentRenderStream.*:MonitorEngine.Loopback*
```

Expected: focused tests 通过。

- [ ] **Step 6: 提交 Task 4**

```powershell
git add src/core/Engine.h src/core/Engine.cpp src/cli/main.cpp
git commit -m "feat(loopback): expose silent render in CLI capture"
```

---

### Task 5: GUI Loopback checkbox 与状态展示

**Files:**
- Modify: `src/gui/AppUi.h`
- Modify: `src/gui/AppUi.cpp`

- [ ] **Step 1: 添加 GUI 状态字段**

在 `src/gui/AppUi.h` 的 loopback state 附近添加：

```cpp
    bool                        loopbackSilentRender_ = true;
```

- [ ] **Step 2: 更新 Loopback Start 调用**

在 `drawLoopbackLeftPanel()` 的 Start 分支中，把 `loopback_.start(...)` 调用替换为：

```cpp
            wa::LoopbackOptions loopbackOptions{};
            loopbackOptions.silentRender = loopbackSilentRender_;
            wa::Result r = loopback_.start(wa::BackendKind::WasapiShared, source, L"", 0,
                                           false, {}, {}, nullptr, loopbackOptions);
```

- [ ] **Step 3: 绘制 checkbox 和状态文本**

在 `drawLoopbackLeftPanel()` 的 `Control` 区 Start/Stop 按钮之后添加：

```cpp
    if (!loopbackStarted_) {
        ImGui::Checkbox("Silent render keepalive", &loopbackSilentRender_);
    } else {
        ImGui::BeginDisabled();
        ImGui::Checkbox("Silent render keepalive", &loopbackSilentRender_);
        ImGui::EndDisabled();
    }
```

在 `Status` 区 `ProgressBar` 前添加：

```cpp
    ImGui::Text("silent render=%s",
        ss[(int)loopbackMs_.silentRenderState]);
```

- [ ] **Step 4: 构建 GUI**

```powershell
cmake --build build --config Debug --target WinAudioGui -j
```

Expected: 构建通过。

- [ ] **Step 5: 提交 Task 5**

```powershell
git add src/gui/AppUi.h src/gui/AppUi.cpp
git commit -m "feat(gui): expose loopback silent render"
```

---

### Task 6: 两个 tab 底部全宽日志布局

**Files:**
- Modify: `src/gui/AppUi.cpp`

- [ ] **Step 1: 修改 `drawMonitorPage()` 布局**

把 `drawMonitorPage()` 替换为：

```cpp
void AppUi::drawMonitorPage() {
    constexpr float kLogHeight = 200.0f;
    const float availY = ImGui::GetContentRegionAvail().y;
    const float topHeight = std::max(120.0f, availY - kLogHeight - ImGui::GetStyle().ItemSpacing.y);

    ImGui::BeginChild("monitorTop", ImVec2(0, topHeight), false);
    ImGui::BeginChild("left", ImVec2(360, 0), true);
    drawLeftPanel();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("charts", ImVec2(0, 0), true);
    drawChartsColumn(monitor_, ms_, monitorViz_);
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::BeginChild("monitorLogRegion", ImVec2(0, kLogHeight), true);
    ImGui::SeparatorText("Log");
    drawLogPanel("log", true);
    ImGui::EndChild();
}
```

- [ ] **Step 2: 修改 `drawLoopbackPage()` 布局**

把 `drawLoopbackPage()` 替换为：

```cpp
void AppUi::drawLoopbackPage() {
    constexpr float kLogHeight = 200.0f;
    const float availY = ImGui::GetContentRegionAvail().y;
    const float topHeight = std::max(120.0f, availY - kLogHeight - ImGui::GetStyle().ItemSpacing.y);

    ImGui::BeginChild("loopbackTop", ImVec2(0, topHeight), false);
    ImGui::BeginChild("loopbackLeft", ImVec2(320, 0), true);
    drawLoopbackLeftPanel();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("loopbackCharts", ImVec2(0, 0), true);
    const uint32_t sr = loopbackMs_.sampleRate;
    const bool overallRunning = (loopbackMs_.overall == wa::StreamState::Running && sr > 0);
    const uint32_t hz = (sr > 0) ? sr : 48000u;
    if (loopbackViz_.xLink1 <= 0.0)
        loopbackViz_.xLink1 = (double)(kSpecCols * kFftHop) / (double)hz;
    if (overallRunning) {
        if (sr != loopbackViz_.waveSr) {
            loopbackViz_.waveSr = sr;
            loopbackViz_.waveN = (int)(kSpecCols * kFftHop);
            loopbackViz_.capWave.assign((size_t)loopbackViz_.waveN, 0.f);
            loopbackViz_.xLink0 = 0.0;
            loopbackViz_.xLink1 = (double)(kSpecCols * kFftHop) / (double)sr;
        }
        if (sr != loopbackViz_.specSr) {
            loopbackViz_.specSr = sr;
            loopbackViz_.workCap.resize(kFftWin);
            loopbackViz_.specWin.resize(kFftWin);
            loopbackViz_.capSpec = std::make_unique<wa::Spectrogram>(kSpecRows, kSpecCols, 20.0, (double)sr / 2.0, sr);
        }
    }
    ImGui::TextUnformatted("System audio waveform + spectrogram");
    drawChartPanel(0, loopback_, loopbackMs_, loopbackViz_);
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::BeginChild("loopbackLogRegion", ImVec2(0, kLogHeight), true);
    ImGui::SeparatorText("Log");
    drawLogPanel("loopbackLog", false);
    ImGui::EndChild();
}
```

- [ ] **Step 3: 从左侧 panel 删除旧日志区**

在 `drawLeftPanel()` 删除：

```cpp
    // --- Log (fills remaining height) ---
    ImGui::SeparatorText("Log");
    drawLogPanel("log", true);
```

在 `drawLoopbackLeftPanel()` 删除：

```cpp
    ImGui::SeparatorText("Log");
    drawLogPanel("loopbackLog", false);
```

- [ ] **Step 4: 构建 GUI**

```powershell
cmake --build build --config Debug --target WinAudioGui -j
```

Expected: 构建通过。

- [ ] **Step 5: 手动 UI smoke**

运行：

```powershell
.\build\bin\Debug\WinAudioGui.exe
```

Expected: `Monitor` 和 `Loopback` tab 都显示底部全宽日志；左侧栏只显示设备、控制、状态；图表仍在右侧。

- [ ] **Step 6: 提交 Task 6**

```powershell
git add src/gui/AppUi.cpp
git commit -m "feat(gui): move logs to tab bottom"
```

---

### Task 7: 完整验证

**Files:**
- Verify only

- [ ] **Step 1: 跑完整 Debug 构建**

```powershell
cmake --build build --config Debug -j
```

Expected: 构建通过，无新增 `/W4` 警告导致失败。

- [ ] **Step 2: 跑完整测试**

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 全部测试通过。

- [ ] **Step 3: 跑 focused 测试复核**

```powershell
.\build\bin\Debug\WinAudioTests.exe --gtest_filter=CliOptions.*:WasapiSilentRenderStream.*:WasapiSystemLoopbackCaptureStream.*:MonitorEngine.Loopback*:MonitorEngine.SilentRenderFailureDoesNotFailLoopbackCapture
```

Expected: focused tests 全部通过。

- [ ] **Step 4: GUI smoke**

如果仓库当前 `tools\run_gui_smoke.ps1` 可用，运行：

```powershell
tools\run_gui_smoke.ps1 -Config Debug -BuildDir build
```

Expected: smoke 脚本完成并输出成功状态。

如果脚本不可用，记录实际错误，并用手动打开 `.\build\bin\Debug\WinAudioGui.exe` 的方式完成视觉确认。

- [ ] **Step 5: 手动 loopback 验证**

在目标 Windows 机器上执行：

```powershell
.\build\bin\Debug\WinAudioCli.exe capture --loopback --out loopback-silent-on.wav --seconds 3 --log-level debug
.\build\bin\Debug\WinAudioCli.exe capture --loopback --out loopback-silent-off.wav --seconds 3 --no-silent-render --log-level debug
```

Expected:

- 第一条日志包含 `silentRender.started`。
- 第二条不启动 silent render。
- 两条命令都能正常退出并写出 WAV 文件。

- [ ] **Step 6: 最终状态检查**

```powershell
git status --short --branch
git log --oneline --decorate -8
```

Expected: 工作区只剩用户已知的未跟踪 `.codegraph/` 和 `AGENTS.md`，实现提交在当前分支顶部。

---

## 自审结果

- Spec 覆盖：底部全宽日志由 Task 6 实现；silent render 默认开启、可关闭、可观察由 Task 1、3、4、5 实现；idle zero-fill 保留由 Task 2 和 Task 7 focused 测试保护；实机验证由 Task 7 覆盖。
- 范围控制：没有拆分日志历史、没有 draggable splitter、没有 application/process loopback、没有改变 Monitor tab playback 语义。
- 类型一致性：`LoopbackOptions` 在 `IAudioBackend.h` 定义；CLI parser、`Engine::startCapture`、`MonitorEngine::start`、GUI Start 调用使用同一类型；`silentRenderState` 使用现有 `StreamState`。
- 风险：`Engine` 没有现成 fake factory seam，Task 4 依靠构建、CLI smoke 和 real WASAPI helper 行为验证；`MonitorEngine` 的 helper 生命周期通过 fake seam 做自动化覆盖。
