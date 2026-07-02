# WinAudio GUI 监听重构 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** GUI 恒为监听模式、"同步播放"由 checkbox 实时控制（关时停用并释放渲染设备）、界面两列且右列图表可拖拽重排。

**Architecture:** MonitorEngine 加运行期播放开关（采集恒运行，渲染在 pump 线程按需 engage/disengage）；AppUi 去单流改 monitor-only 两列布局 + 拖拽重排。基于 `docs/superpowers/specs/2026-07-02-winaudio-gui-monitor-redesign-design.md` (v2)。

**Tech Stack:** C++17/MSVC、Dear ImGui + ImPlot、CMake（`build.bat`/`test.bat`）、gtest。

## Global Constraints
- Core 纯 Win32+STL、不跨 API 抛异常、状态用原子、音频路径无 mutex、帧对齐 ring I/O、无重采样器（渲染开启时校验采集==渲染采样率）。/W4 零告警。
- **对象生命周期不变量（防 UAF）**：`captureRing_`/`captureScope_`/**`renderScope_`** 在 `start()` 分配、**仅** `teardown()`（pump join 后）释放；`renderBackend_`/`renderRing_`/`delayFifo_`/渲染刮擦为 **per-engage**（`engageRender` `make_unique`、`disengageRender` reset）。GUI 每帧读 `renderScope_`，故它必须全会话存活；GUI 从不读 `renderRing_`/`delayFifo_`/`renderBackend_`。
- `overall_` 以**采集**为准（采集起即 Running）；`renderState_` 由渲染 engage 决定（Running=渲染设备已开 / Idle=播放关 / Error=engage 采样率不匹配）。
- 渲染后端仅由 (a) `start()` 内 pump 启动前、(b) pump 线程 触碰；GUI 只设原子 `wantPlayback_` + 读 `poll()`。
- 验证：`build.bat Debug`/`Release` /W4 零告警；`test.bat` 全套通过（现 56 + T1 新增 4 = 60）；GUI 存活。构建产物在 `build/bin|lib/<Config>/`。
- 构建工具：`build.bat`/`test.bat` 内部调 `cmake`/`ctest`；若不在 PATH，会话内前置 VS 自带路径 `D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`（`$env:PATH += ";..."`）。VS 生成器自找 MSVC，无需 vcvars。

---

## Task 1: MonitorEngine 运行期播放开关（engage/disengage）

**Files:** Modify `src/core/MonitorEngine.h`, `src/core/MonitorEngine.cpp`, `src/tests/test_monitorengine.cpp`.

**Interfaces produced:** `void setPlaybackEnabled(bool)`; `Result start(kind, capId, renderId, delayMs, bool playbackEnabled=true)`; `renderState` 语义如上。

- [ ] **Step 1: 头文件改动 (`MonitorEngine.h`)**

在 public 区 `poll()` 后加、并改 `start` 签名：
```cpp
    Result start(BackendKind kind, const DeviceId& capId, const DeviceId& renderId,
                 uint32_t delayMs, bool playbackEnabled = true);   // 第5参默认 true = 旧行为
    void   setPlaybackEnabled(bool enabled);                        // 运行期实时开关（GUI 线程）
```
在 private 区 `rollback` 声明附近加：
```cpp
    Result engageRender();      // pump 前(start) 或 pump 线程调用；开渲染+校验+建 FIFO/刮擦；失败原子(全关渲染)
    void   disengageRender();   // 停+关渲染、释放设备、reset renderRing_/delayFifo_；不动 renderScope_
```
成员区加（run-const，供 engage 复用）：
```cpp
    BackendKind kind_{BackendKind::WasapiShared};
    DeviceId    renderId_{};
    uint32_t    delayMs_ = 0;
    std::atomic<bool> wantPlayback_{false};
```
（`prefilled_` 保留，语义改为"当前 engage 的渲染预填已完成"。）

- [ ] **Step 2: `start()` 重构 (`MonitorEngine.cpp`)** — 采集恒开、渲染抽到 `engageRender`、scope 会话存活、overall 以采集为准。

替换现有 `start()`（`MonitorEngine.cpp:64-184`）为：
```cpp
Result MonitorEngine::start(BackendKind kind, const DeviceId& capId, const DeviceId& renderId,
                            uint32_t delayMs, bool playbackEnabled) {
    teardown();
    // Fresh status slate.
    overall_.store(StreamState::Idle, std::memory_order_relaxed);
    capState_.store(StreamState::Idle, std::memory_order_relaxed);
    renderState_.store(StreamState::Idle, std::memory_order_relaxed);
    errorCode_.store(0, std::memory_order_relaxed);
    fifoFillMs_.store(0.f, std::memory_order_relaxed);
    driftFixes_.store(0, std::memory_order_relaxed);
    capXruns_.store(0, std::memory_order_relaxed);
    renderXruns_.store(0, std::memory_order_relaxed);
    renderDropped_.store(0, std::memory_order_relaxed);
    capLevel_.store(0.f, std::memory_order_relaxed);
    renderLevel_.store(0.f, std::memory_order_relaxed);
    prefilled_.store(false, std::memory_order_relaxed);
    sampleRate_.store(0, std::memory_order_relaxed);
    delayMsAtomic_.store(0, std::memory_order_relaxed);
    renderBufMs_.store(0, std::memory_order_relaxed);

    if (delayMs > 10000u)
        return rollback(StreamState::Error, MonitorError::InvalidDelay,
                        static_cast<long>(MonitorError::InvalidDelay),
                        "MonitorEngine: delayMs exceeds maximum (10000 ms)");

    kind_ = kind; renderId_ = renderId; delayMs_ = delayMs;

    // --- Capture (always) ---
    captureRing_ = std::make_unique<RingBuffer>(kRingBytes);
    capBackend_  = makeBackend(DataFlow::Capture, kind, nullptr);
    if (!capBackend_)
        return rollback(StreamState::Idle, MonitorError::Factory, -1, "MonitorEngine: capture factory null");
    if (Result r = capBackend_->open(capId, AudioFormat{}, captureRing_.get()); !r)
        return rollback(StreamState::Idle, MonitorError::CaptureOpen, r.code, r.message);
    if (Result r = capBackend_->start(); !r)
        return rollback(StreamState::Idle, MonitorError::CaptureStart, r.code, r.message);
    capState_.store(StreamState::Running, std::memory_order_relaxed);
    capFmt_ = capBackend_->stats().actualFormat;

    const uint32_t sr = capFmt_.sampleRate;
    capCh_         = capFmt_.channels ? capFmt_.channels : static_cast<uint16_t>(1);
    capFrameBytes_ = capFmt_.blockAlign();
    if (sr == 0 || capFrameBytes_ == 0)
        return rollback(StreamState::Error, MonitorError::CaptureStart, -1, "MonitorEngine: invalid capture format");

    // --- Session-lifetime buffers (allocated once; freed only in teardown) ---
    const size_t scopeCap = std::max<size_t>(static_cast<size_t>(sr) * 2u, 8192u);
    captureScope_ = std::make_unique<ScopeBuffer>(scopeCap);
    renderScope_  = std::make_unique<ScopeBuffer>(scopeCap);  // GUI reads every frame -> MUST stay alive
    maxChunkFrames_ = kMaxChunkFrames;
    capScratch_.assign(maxChunkFrames_ * capFrameBytes_, 0);
    capFloat_.assign(maxChunkFrames_ * capCh_, 0.f);
    capMono_.assign(maxChunkFrames_, 0.f);

    sampleRate_.store(sr, std::memory_order_relaxed);
    delayMsAtomic_.store(delayMs, std::memory_order_relaxed);
    capDataReadyEvent_ = capBackend_->dataReadyEvent();

    // --- Optional render at start (synchronous engage -> full rollback on failure = CLI parity) ---
    wantPlayback_.store(playbackEnabled, std::memory_order_relaxed);
    if (playbackEnabled) {
        if (Result r = engageRender(); !r) {
            long code = r.code;
            std::string msg = r.message;
            MonitorError err = (code == static_cast<long>(MonitorError::RateMismatch))
                                   ? MonitorError::RateMismatch : MonitorError::RenderStart;
            return rollback(StreamState::Error, err, code, std::move(msg));   // stops capture too
        }
    }

    // Capture is up -> engine Running (independent of render prefill).
    overall_.store(StreamState::Running, std::memory_order_relaxed);

    running_.store(true, std::memory_order_release);
    try {
        pump_ = std::thread(&MonitorEngine::pumpLoop, this);
    } catch (const std::exception& e) {
        running_.store(false, std::memory_order_relaxed);
        return rollback(StreamState::Error, MonitorError::PumpLaunch, -1,
                        std::string("MonitorEngine: pump launch failed: ") + e.what());
    }
    return Result::Ok();
}
```

- [ ] **Step 3: `engageRender()` / `disengageRender()`** — 新增（放在 `start()` 之后）。engage 失败必须原子（全关渲染、不留半开）。

```cpp
Result MonitorEngine::engageRender() {
    const uint32_t sr = capFmt_.sampleRate;
    renderRing_    = std::make_unique<RingBuffer>(kRingBytes);            // per-engage
    renderBackend_ = makeBackend(DataFlow::Render, kind_, &capFmt_);
    if (!renderBackend_) { renderRing_.reset(); return Result::Fail(-1, "MonitorEngine: render factory null"); }
    if (Result r = renderBackend_->open(renderId_, AudioFormat{}, renderRing_.get()); !r) {
        renderBackend_.reset(); renderRing_.reset(); return Result::Fail(r.code, r.message);
    }
    if (Result r = renderBackend_->start(); !r) {
        renderBackend_->stop(); renderBackend_.reset(); renderRing_.reset(); return Result::Fail(r.code, r.message);
    }
    renderFmt_ = renderBackend_->stats().actualFormat;
    if (renderFmt_.sampleRate == 0 || renderFmt_.sampleRate != sr) {
        renderBackend_->stop(); renderBackend_.reset(); renderRing_.reset();
        return Result::Fail(static_cast<long>(MonitorError::RateMismatch),
                            "MonitorEngine: capture/render sample-rate mismatch");
    }
    renderCh_         = renderFmt_.channels ? renderFmt_.channels : static_cast<uint16_t>(1);
    renderFrameBytes_ = renderFmt_.blockAlign();
    if (renderFrameBytes_ == 0) {
        renderBackend_->stop(); renderBackend_.reset(); renderRing_.reset();
        return Result::Fail(-1, "MonitorEngine: invalid render frame size");
    }
    const BackendStats rstats = renderBackend_->stats();
    size_t periodFrames = rstats.bufferFrames ? rstats.bufferFrames : (sr / 100u);
    if (periodFrames == 0) periodFrames = 1;
    const size_t delayFrames    = static_cast<size_t>(static_cast<uint64_t>(delayMs_) * sr / 1000u);
    prefillFrames_              = delayFrames + periodFrames;
    const size_t capacityFrames = prefillFrames_ + sr + periodFrames;
    size_t deadbandFrames       = periodFrames < 64 ? 64 : periodFrames;
    delayFifo_ = std::make_unique<DelayFifo>(capCh_, delayFrames, capacityFrames, deadbandFrames);  // per-engage

    popBuf_.assign(maxChunkFrames_ * capCh_, 0.f);
    renderAdapt_.assign(maxChunkFrames_ * renderCh_, 0.f);
    renderMono_.assign(maxChunkFrames_, 0.f);
    renderBytes_.assign(maxChunkFrames_ * renderFrameBytes_, 0);

    renderBufMs_.store(rstats.bufferFrames
                           ? static_cast<uint32_t>(static_cast<uint64_t>(rstats.bufferFrames) * 1000u / sr) : 0u,
                       std::memory_order_relaxed);
    renderDropped_.store(0, std::memory_order_relaxed);
    renderXruns_.store(0, std::memory_order_relaxed);
    renderLevel_.store(0.f, std::memory_order_relaxed);
    prefilled_.store(false, std::memory_order_relaxed);          // this engage's fill not done yet
    renderState_.store(StreamState::Running, std::memory_order_relaxed); // device up (fills, then pops)
    return Result::Ok();
}

void MonitorEngine::disengageRender() {
    if (renderBackend_) renderBackend_->stop();
    renderBackend_.reset();
    renderRing_.reset();
    delayFifo_.reset();
    renderState_.store(StreamState::Idle, std::memory_order_relaxed);
    renderLevel_.store(0.f, std::memory_order_relaxed);
    renderBufMs_.store(0, std::memory_order_relaxed);
    fifoFillMs_.store(0.f, std::memory_order_relaxed);
    prefilled_.store(false, std::memory_order_relaxed);
    // NOTE: renderScope_ is session-lifetime -> NOT touched here (GUI reads it every frame).
}

void MonitorEngine::setPlaybackEnabled(bool enabled) {
    wantPlayback_.store(enabled, std::memory_order_release);   // pump converges to this next iteration
}
```

- [ ] **Step 4: `pumpLoop()` 重构** — 加 COM guard、toggle 处理、渲染路径按 `renderActive` 门控。替换 `pumpLoop()`（`:186-260`）为：
```cpp
void MonitorEngine::pumpLoop() {
    ComInitGuard com;   // pump owns COM (it now does render backend COM lifetime on toggle)
    const uint16_t capCh         = capCh_;
    const uint32_t capFrameBytes = capFrameBytes_;
    const uint32_t sr            = capFmt_.sampleRate;
    const size_t   maxFrames     = maxChunkFrames_;
    HANDLE         evt           = static_cast<HANDLE>(capDataReadyEvent_);

    while (running_.load(std::memory_order_acquire)) {
        if (evt) WaitForSingleObject(evt, kPumpWaitMs);
        else     Sleep(5);
        if (!running_.load(std::memory_order_acquire)) break;

        // --- Converge render lifecycle to wantPlayback_ (pump-thread only) ---
        const bool want   = wantPlayback_.load(std::memory_order_acquire);
        const bool active = (renderBackend_ != nullptr);
        if (want && !active) {
            if (Result r = engageRender(); !r) {
                renderState_.store(StreamState::Error, std::memory_order_relaxed);
                errorCode_.store(static_cast<uint32_t>(
                    r.code == static_cast<long>(MonitorError::RateMismatch)
                        ? MonitorError::RateMismatch : MonitorError::RenderStart),
                    std::memory_order_relaxed);
                wantPlayback_.store(false, std::memory_order_relaxed); // don't retry every wake
                // capture continues untouched
            }
        } else if (!want && active) {
            disengageRender();
        }
        const bool renderActive = (renderBackend_ != nullptr);
        const uint16_t renderCh         = renderCh_;
        const uint32_t renderFrameBytes = renderFrameBytes_;

        // --- Drain capture in whole frames (capture path ALWAYS runs) ---
        for (;;) {
            if (!running_.load(std::memory_order_acquire)) break;
            const size_t frames = readWholeFrames(*captureRing_, capScratch_.data(), capFrameBytes, maxFrames);
            if (frames == 0) break;

            pcmToFloat(capScratch_.data(), frames, capFmt_, capFloat_.data());
            downmixMono(capFloat_.data(), frames, capCh, capMono_.data());
            captureScope_->push(capMono_.data(), frames);
            capLevel_.store(peakLevel(capMono_.data(), frames), std::memory_order_relaxed);

            if (!renderActive) continue;   // capture-only: do not touch FIFO / render

            delayFifo_->pushFrames(capFloat_.data(), frames);
            if (!prefilled_.load(std::memory_order_relaxed)) {
                if (delayFifo_->fillFrames() >= prefillFrames_)
                    prefilled_.store(true, std::memory_order_relaxed);
            } else {
                const size_t popped = delayFifo_->popFrames(popBuf_.data(), frames);
                if (popped > 0) {
                    downmixMono(popBuf_.data(), popped, capCh, renderMono_.data());
                    renderScope_->push(renderMono_.data(), popped);
                    renderLevel_.store(peakLevel(renderMono_.data(), popped), std::memory_order_relaxed);
                    adaptChannels(popBuf_.data(), capCh, renderAdapt_.data(), renderCh, popped);
                    floatToPcm(renderAdapt_.data(), popped, renderFmt_, renderBytes_.data());
                    const size_t wantBytes = static_cast<size_t>(popped) * renderFrameBytes;
                    const size_t freeBytes = renderRing_->availableWrite();
                    const size_t safeBytes = (std::min(wantBytes, freeBytes) / renderFrameBytes) * renderFrameBytes;
                    renderRing_->write(renderBytes_.data(), safeBytes);
                    renderDropped_.fetch_add((wantBytes - safeBytes) / renderFrameBytes, std::memory_order_relaxed);
                }
            }
        }

        // --- Publish status (guard render fields on renderActive) ---
        capXruns_.store(captureRing_->overruns(), std::memory_order_relaxed);
        if (renderActive) {
            fifoFillMs_.store(static_cast<float>(delayFifo_->lowpassFillFrames() * 1000.0 / (double)sr),
                              std::memory_order_relaxed);
            driftFixes_.store(delayFifo_->driftFixes(), std::memory_order_relaxed);
            if (prefilled_.load(std::memory_order_relaxed))
                renderXruns_.store(renderDropped_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    }
}
```
(注：`ComInitGuard` 见 `ComUtil.h`；若默认构造即 MTA 用之，否则按其 API 传 MTA。`#include "ComUtil.h"` 到 MonitorEngine.cpp。)

- [ ] **Step 5: `teardown()` 微调** — 现有 `teardown()`（`:262-282`）已 join-before-free 且释放全部；只需确认 render 为空时 `if (renderBackend_)` 守卫存在（已在）。保持不变即可（它 stop+reset cap 与 render、reset 全部 ring/scope/fifo）。**无需改动**，但确认 `renderScope_.reset()` 只在此发生（不在 disengage）。

- [ ] **Step 6: 测试 (`test_monitorengine.cpp`)** — 加 4 测试 + 调整 2 处。fake backend 需支持"渲染 open/stop 标志"与"可配采样率"（复用现有 FakeRig；若无 stop 标志，加一个原子指针记录，模式同现有 `stoppedOut_`）。
  - `PlaybackStartsDisabled`：`start(...,/*playbackEnabled*/false)` → poll: `capState==Running`、`overall==Running`、`renderState==Idle`；fake 渲染工厂**未被调用**（记数）。
  - `EnablePlaybackEngagesRender`：上后 `setPlaybackEnabled(true)`，fake 采集推数据驱动 pump 数轮 → `renderState==Running`、fake 渲染已 open。
  - `DisablePlaybackStopsRender`：engaged 后 `setPlaybackEnabled(false)` + 驱动 pump → `renderState==Idle`、fake 渲染 stop 标志置位、`overall==Running`（采集续）；随后 `float buf[16]; uint64_t e; monitor.snapshotRender(16,buf,e);` **不崩溃**（renderScope_ 存活，返回 false/空）。
  - `EnablePlaybackRateMismatch`：fake 采集 48000/渲染 44100，`start(...,false)` 后 `setPlaybackEnabled(true)` + 驱动 → `renderState==Error`、`errorCode==RateMismatch`、播放保持关、`capState==Running`。
  - 调整 `PrefillThenRunning`（若存在）：现 `overall` 在 start 后即 Running（采集），断言改为"start 后 overall==Running；渲染预填期 renderXruns 仍 0；预填后 pop 开始"。（依当前测试实际断言微调，保持意图。）
  - `RateMismatchFails`（现有）：4 参 `start`（默认 playbackEnabled=true）→ 采样率不匹配同步 engage 失败 → `start` 返回 Fail；**新增断言 `poll().capState==StreamState::Idle`**（采集已回滚）。

- [ ] **Step 7: 构建 + 测试 + 提交**
```
.\build.bat Debug
.\build\bin\Debug\WinAudioTests.exe --gtest_filter=MonitorEngine.*    # 期望原有 6 + 新 4 = 10 通过
.\build.bat Release
.\test.bat Release                                                    # 双配置全绿（全套 60）
git add src/core/MonitorEngine.h src/core/MonitorEngine.cpp src/tests/test_monitorengine.cpp
git -c commit.gpgsign=false commit -m "feat(core): MonitorEngine runtime playback toggle (engage/disengage)"
```
期望：/W4 零告警；MonitorEngine 10 项全通过；全套 60。（若 `cmake` 不在 PATH，先按 Global Constraints 前置 VS 自带路径。）

---

## Task 2: AppUi 改 monitor-only 两列布局 + 播放 checkbox

**Files:** Modify `src/gui/AppUi.h`, `src/gui/AppUi.cpp`.

**Interfaces consumed:** T1 的 `monitor_.start(kind,capId,renId,delayMs,playbackEnabled)`、`setPlaybackEnabled(bool)`、`poll().renderState`。

- [ ] **Step 1: `AppUi.h`** — 去单流、加 DeviceEnumerator + 播放态。
  - 删 `#include "Engine.h"`；加 `#include "DeviceEnumerator.h"`（`BackendKind` 经 `MonitorEngine.h` 传递仍可用）。
  - 删成员：`wa::Engine engine_`、`modeIdx_/prevMode_`、`deviceIdx_/devices_/devicesLoaded_`、`wavPath_`、`rateIdx_/bitsIdx_/chIdx_/isFloat_`。删方法：`refreshDevices()`、`drawSingleStream(bool)`。
  - 加成员：`wa::DeviceEnumerator enumerator_;`、`bool playbackEnabled_ = false;`、`std::vector<int> chartOrder_ = {0,1,2,3,4,5};`（0=采集波形 1=播放波形 2=采集频谱 3=播放频谱 4=采集声谱图 5=播放声谱图，默认同类相邻；T2 固定序渲染，T3 加拖拽）、`int prevRenderState_ = 0;`（检测 Running→非 以重置播放侧分析）。
  - 加私有方法声明：`void drawLeftPanel();`、`void drawChartsColumn();`、`void drawChartPanel(int id);`、`const char* chartTitle(int id);`。删 `drawMonitor(bool)` 声明（其体拆入上述方法）。
  - 保留：`monitor_`、`backendIdx_`、`capDevices_/renderDevices_/capDevIdx_/renderDevIdx_/monitorDevicesLoaded_`、`delayMs_`、`monitorStarted_`、`logLines_`、所有分析缓冲（`capWave_...capSpec_`）。
  - `draw()` 无参不变（重写见 Step 3）。

- [ ] **Step 2: `AppUi.cpp` 删死代码 + 改 enumerate 源**
  - 删文件顶 `utow()`（保留 `wtou()`）、`kRates/kRatesS/kBits/kBitsS/kChans/kChansS` 数组。删 `drawSingleStream` 整个函数、`refreshDevices()`。
  - `refreshMonitorDevices()` 改用 enumerator（`Engine::enumerate` 已无）：
```cpp
void AppUi::refreshMonitorDevices() {
    capDevices_.clear(); renderDevices_.clear();
    enumerator_.enumerate(wa::DataFlow::Capture, capDevices_);
    enumerator_.enumerate(wa::DataFlow::Render,  renderDevices_);
    capDevIdx_ = 0; renderDevIdx_ = 0; monitorDevicesLoaded_ = true;
}
```
（确认 `DeviceEnumerator::enumerate(DataFlow, std::vector<DeviceInfo>&)` 签名；若返回 vector 则 `capDevices_ = enumerator_.enumerate(...)`。读 `src/core/DeviceEnumerator.h` 对齐。）
  - `stopAll()` 改为只 `monitor_.stop();`。

- [ ] **Step 3: `draw()` 两列布局 + 播放 checkbox + renderState 门控**
  重写 `draw()`（去 mode Combo/单流分支）；把原 `drawMonitor` 的图表体抽为按 id 的小函数 `drawChartPanel(int id)`（内部 switch 到现有的 6 段 ImPlot 代码：0 采集波形/1 播放波形/2 采集频谱/3 播放频谱/4 采集声谱图/5 播放声谱图；宽度用 `-1` 填满右列，高度沿用现值 120/140/160）。draw() 结构：
```cpp
void AppUi::draw() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(800, 400), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::Begin("WinAudio");
    // 左列
    ImGui::BeginChild("left", ImVec2(360, 0), true);
    drawLeftPanel();            // 设备 / 控制(含 checkbox) / 状态 / 日志
    ImGui::EndChild();
    ImGui::SameLine();
    // 右列
    ImGui::BeginChild("charts", ImVec2(0, 0), true);
    drawChartsColumn();         // 按 chartOrder_ 渲染 6 面板（T3 加拖拽；T2 先固定序）
    ImGui::EndChild();
    ImGui::End();
}
```
`drawLeftPanel()`：
  - 设备区：`SeparatorText("Devices")`；Refresh 按钮；采集 listbox（`##capdev`）+ 渲染 listbox（`##rendev`）——**两者始终可交互**（沿用现有 `drawMonitor` 里的 listbox 代码）。
  - 控制区：`SeparatorText("Control")`；Backend combo（`WASAPI-Shared/Exclusive`）；`SliderInt("Delay (ms)", &delayMs_, 0, 500)`；Start/Stop；**播放 checkbox**：
```cpp
    if (!monitorStarted_) ImGui::BeginDisabled();
    if (ImGui::Checkbox("同步播放 (playback)", &playbackEnabled_))
        monitor_.setPlaybackEnabled(playbackEnabled_);   // 运行中实时生效
    if (!monitorStarted_) ImGui::EndDisabled();
```
  - Start 分支：`wa::Result r = monitor_.start(kind, capId, renId, (uint32_t)delayMs_, playbackEnabled_);` 成功后 `monitorStarted_=true; nextCapEnd_=0; nextRenderEnd_=0; specSr_=0; waveSr_=0;`。Stop 分支：`monitor_.stop(); monitorStarted_=false;`。
  - 状态区：`SeparatorText("Status")`；现有 `ms=monitor_.poll()` + overall/cap/ren 文本 + fifo/drift/xrun + cap/ren 电平条（沿用）。
  - 日志区：`SeparatorText("Log")`；`ImGui::BeginChild("log", ImVec2(0, 0), true)`（**填满剩余高度**，不写死 120）遍历 `logLines_`。
  `drawChartsColumn()`（T2 先固定序 `for (int id : chartOrder_) { drawChartPanel(id); }`，`chartOrder_` T3 引入；T2 可暂用 `{0,1,2,3,4,5}` 局部数组）。
  **renderState 门控**：`drawChartPanel` 里播放三图（id 1/3/5）仅在 `renderState==Running` 时画数据线（否则空轴）；且在 `draw()` 顶部用 `prevRenderState_` 比较：若本帧 renderState 由 Running→非 Running，则 `magRender_.clear(); renderSpec_.reset(); nextRenderEnd_=0; std::fill(renderWave_.begin(),renderWave_.end(),0.f);` 使播放图变空而非留旧帧，再更新 `prevRenderState_`。采集三图只要 `overall==Running && sr>0` 持续绘制。

- [ ] **Step 4: 构建 + 存活 + 提交**
```
.\build.bat Debug
$p = Start-Process ".\build\bin\Debug\WinAudioGui.exe" -PassThru; Start-Sleep 3
if ($p.HasExited){"FAIL"}else{Stop-Process $p;"GUI started OK"}
git add src/gui/AppUi.h src/gui/AppUi.cpp
git -c commit.gpgsign=false commit -m "feat(gui): monitor-only two-column layout + realtime playback checkbox"
```
期望：/W4 零告警；GUI 存活；无 mode 选择器；两列。（有硬件桌面时人工验证 checkbox 实时起停播放 + 播放图空/有信号切换。）

---

## Task 3: 右列图表拖拽 splice 重排

**Files:** Modify `src/gui/AppUi.h`, `src/gui/AppUi.cpp`.

- [ ] **Step 1:** `chartOrder_` 成员已在 T2 加入（默认 `{0,1,2,3,4,5}`）；本任务只给 `drawChartsColumn()` 加拖拽逻辑（T2 里它是固定序 `for (int id : chartOrder_) drawChartPanel(id);`）。

- [ ] **Step 2: `drawChartsColumn()` 拖拽 splice**（替换 T2 的固定序版本）
  每个面板顶部一个手柄，作 drag source（payload = 该项在 `chartOrder_` 的**位置**），面板整体作 drop target；接收时对 `chartOrder_` 做**插入式移动**（erase + insert），非 swap：
```cpp
void AppUi::drawChartsColumn() {
    for (int pos = 0; pos < (int)chartOrder_.size(); ++pos) {
        int id = chartOrder_[pos];
        ImGui::PushID(pos);
        ImGui::Button("☰");                                  // drag handle
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload("CHART_POS", &pos, sizeof(int));
            ImGui::TextUnformatted(chartTitle(id));
            ImGui::EndDragDropSource();
        }
        ImGui::SameLine(); ImGui::TextUnformatted(chartTitle(id));
        drawChartPanel(id);                                  // the ImPlot chart
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("CHART_POS")) {
                int from = *(const int*)pl->Data;
                if (from != pos) {                           // splice: move `from` to `pos`
                    int moved = chartOrder_[from];
                    chartOrder_.erase(chartOrder_.begin() + from);
                    chartOrder_.insert(chartOrder_.begin() + pos, moved);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }
}
```
`chartTitle(int id)` 返回 6 个标题字符串。注：ImGui `BeginDragDropTarget` 在 child 内不自动滚动，目标须在可视区（已知限制，不处理）。

- [ ] **Step 3: 构建 + 存活 + 提交**
```
.\build.bat Debug ; GUI 存活检查同上
git add src/gui/AppUi.h src/gui/AppUi.cpp
git -c commit.gpgsign=false commit -m "feat(gui): drag-reorder monitor charts (splice)"
```
期望：/W4 零告警；GUI 存活；（有桌面时验证拖拽把采集/播放波形并到相邻、顺序稳定）。

---

## Task 4: 更新 CLAUDE.md GUI 章节

**Files:** Modify `CLAUDE.md`.

- [ ] **Step 1:** 更新 GUI 相关描述：GUI 现为 **monitor-only**（无 Capture/Playback/Monitor mode 切换，恒监听）；"同步播放" checkbox 实时控制播放（关时释放渲染设备）；两列布局（左 设备/控制/状态/日志，右 波形+频谱+声谱图，**可拖拽重排**）；录制到 WAV / 播放 WAV 文件仅 CLI 提供。同步 `## 项目状态` 里 WinAudioGui 一句与"GUI（首选）"用法段（去掉 mode/格式控件描述）。保持 CLI 段不变。

- [ ] **Step 2: 构建确认 + 提交**
```
.\build.bat Release ; .\test.bat Release      # 文档命令应可用
git add CLAUDE.md
git -c commit.gpgsign=false commit -m "docs: CLAUDE.md GUI monitor-only + realtime playback + reorderable charts"
```

---

## Verification Summary（全计划成功标准）
- `build.bat Debug`/`Release` 均 /W4 零告警；`test.bat` 双配置全绿（MonitorEngine 原有 6 + 新 4）；GUI 存活。
- 无 mode 选择器、启动即监听、采集恒可视化（overall=Running）；两列布局；右列 6 图可拖拽 splice 重排。
- "同步播放" checkbox 实时：勾选→ren=Running+播放图有信号；取消→ren=Idle+释放设备+播放图变空；反复切换不崩溃/不 UAF/不死锁；采样率不匹配勾选→ren=Error+采集续；start-with-playback 不匹配→整体失败 capState=Idle。
- 仅改 MonitorEngine.h/.cpp、test_monitorengine.cpp、AppUi.h/.cpp、CLAUDE.md（+ 若编译需要的 main.cpp 最小调整）。
