# WinAudio Phase 2 设计：WASAPI-Exclusive 后端

- 日期：2026-06-29
- 状态：已评审通过，待生成实现计划
- 依赖：Phase 1 MVP（已合并到 master，commit 19607ee 起）

## 1. 背景与目标

Phase 1 交付了 WASAPI-Shared 采集/播放（经 `IAudioBackend` 抽象 + SPSC ring 解耦，方案 C）。Phase 2 增加 **WASAPI-Exclusive（独占模式）** 后端：低延迟、可指定并探测格式，服务 spec 的「格式/能力探测」与「延迟测量」目标。

本阶段同时**重构现有 WASAPI 后端**为「公共基类 + 采集/渲染子类」，让 Shared 与 Exclusive 共享线程/事件/循环脚手架，差异只落在格式协商与 Initialize。

## 2. WASAPI-Exclusive 与 Shared 的本质差异

1. **格式协商**：独占不走 `GetMixFormat`。必须指定目标格式并用 `IAudioClient::IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, fmt, nullptr)` 探测；不支持返回 `AUDCLNT_E_UNSUPPORTED_FORMAT`。
2. **缓冲对齐重试**：独占 `Initialize` 可能返回 `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED`。标准修复：`GetBufferSize` 取对齐帧数 → 用 `duration = round(10000.0 * 1000 / fmt.sampleRate * alignedFrames) ` 重算 → 释放并**重建 IAudioClient** → 用新 duration 同时作为 periodicity 重新 `Initialize`。最多重试一次（对齐后必成或失败）。
3. **占用冲突**：设备被独占占用 → `AUDCLNT_E_DEVICE_IN_USE`；策略不允许独占 → `AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED`。须归一为明确错误消息。
4. **periodicity**：独占事件驱动模式下 `Initialize` 的 `hnsBufferDuration` 与 `hnsPeriodicity` 必须相等（用设备最小周期或对齐后的周期）。

## 3. 类层次重构

替换现有的 `WasapiSharedCapture` / `WasapiSharedRender` 两个独立类为：

```
WasapiStream (抽象基类, src/core/WasapiStream.h/.cpp)
  拥有: RingBuffer* ring_; AudioFormat actualFormat_; uint32_t bufferFrames_;
        std::atomic<bool> running_; std::thread thread_; void* hEvent_;
        DeviceId deviceId_; ComPtr<IAudioClient> client_;
        就绪握手 readyMtx_/readyCv_/ready_/startResult_;
        WasapiMode mode_;            // Shared | Exclusive
        AudioFormat requestedFormat_; bool hasRequestedFormat_;
  通用: open(id,fmt,ring) / start() / stop() / close() / stats()
        threadMain() 脚手架:
          ComInitGuard → 解析设备(dataFlow()) → negotiateFormat()
          → initializeClient() → SetEventHandle → createService()
          → signalReady(Ok) → preRoll() → runLoop() → client_->Stop()
        （每个 init 失败路径调 signalReady(Fail) 并 return —— 保持 Phase 1 修复的同步 start() 握手语义，无死锁）
  negotiateFormat(): mode 感知
    - Shared:    GetMixFormat → actualFormat_
    - Exclusive: 取 requestedFormat_（无则按回退列表）逐个 IsFormatSupported(EXCLUSIVE)
                 命中即 actualFormat_；全不支持 → Fail(AUDCLNT_E_UNSUPPORTED_FORMAT)
  initializeClient(): mode 感知 sharemode + 事件标志
    - Shared:    Initialize(SHARED, EVENTCALLBACK, dur=100ms, 0, mixFmt)
    - Exclusive: Initialize(EXCLUSIVE, EVENTCALLBACK, dur=per, per, fmt)；
                 捕获 BUFFER_SIZE_NOT_ALIGNED → 重建 client_ + 对齐 duration 重试一次
  纯虚: EDataFlow dataFlow();  void createService();  void preRoll();  void runLoop();

WasapiCaptureStream : WasapiStream
  dataFlow=eCapture; createService=GetService(IAudioCaptureClient);
  preRoll=∅; runLoop=drain(GetBuffer/ReleaseBuffer)→ring_->write（含 SILENT→zeros）
WasapiRenderStream : WasapiStream
  dataFlow=eRender; createService=GetService(IAudioRenderClient);
  preRoll=一个静音缓冲; runLoop=GetCurrentPadding→ring_->read→feed（underrun→memset 0）
```

构造参数 `(WasapiMode mode, const AudioFormat* requestedFormat = nullptr)`：
- `BackendKind::WasapiShared` → `mode=Shared`，忽略 requestedFormat（用 mix）。
- `BackendKind::WasapiExclusive` → `mode=Exclusive` + requestedFormat（采集可为空→回退列表）。

**等价性约束**：`mode=Shared` 路径的运行时行为必须与 Phase 1 的 `WasapiSharedCapture/Render` 逐项等价（同样的 100ms 缓冲、事件驱动、握手、HRESULT 检查、xrun 计数）。Phase 1 的两处修复（CreateEventW/std::thread 守卫；GetBufferSize/SetEventHandle/Start 的 HRESULT 检查）必须保留在基类脚手架中。

## 4. 格式回退列表（Exclusive 采集无显式格式时）

依次探测，取第一个 `IsFormatSupported(EXCLUSIVE)` 通过的：
`48000/16/2 → 44100/16/2 → 48000/24/2 → 48000/32/2(float) → 48000/16/1`
全不通过 → 明确报错（设备不支持任何常见独占格式）。播放（render）必须由 wav 的格式决定，不回退（格式不被独占支持即明确报错）。

## 5. 接入改动

- **Engine**（`src/core/Engine.h/.cpp`）
  - `enum class BackendKind { WasapiShared, WasapiExclusive };`
  - `startCapture(BackendKind, const DeviceId&, const std::wstring& wavPath, const AudioFormat* requested = nullptr)`；`startPlayback` 同加可选 requested（Shared 忽略）。
  - 按 kind+mode 构造 `WasapiCaptureStream`/`WasapiRenderStream`。
  - 新增 `Result probeFormat(BackendKind, DataFlow, const DeviceId&, const AudioFormat&)`：薄封装 `IAudioClient::IsFormatSupported`，供 CLI/GUI 的「探测格式」用（独占模式）。
- **CLI**（`src/cli/main.cpp`）
  - `--backend wasapi-shared|wasapi-exclusive`（默认 shared）。
  - `--format <rate>/<bits>/<ch>[f]`（如 `48000/16/2` 或 `48000/32/2f`）传给 capture/play。
  - 新增 `probe --backend wasapi-exclusive --device <id> --format 48000/16/2 [--render|--capture]` 子命令，打印是否支持。
- **GUI**（`src/gui/AppUi.cpp`）
  - 后端下拉框：`WASAPI-Shared`、`WASAPI-Exclusive`。
  - 选 Exclusive 时启用格式控件（采样率/位深/声道下拉 + float 勾选）与「探测此格式是否支持」按钮（调 `Engine::probeFormat`，结果进日志窗格）。
  - Shared 时格式控件禁用（沿用设备混音格式）。

## 6. 测试策略

- **纯逻辑 gtest**（不碰设备）：
  - 对齐 duration 计算：给定 sampleRate + alignedFrames，验证 `hnsPeriodicity` 公式与取整。
  - 格式回退列表选择：给定一个「支持判定」桩函数，验证按序返回第一个命中 / 全不命中报错。
  - 格式字符串解析（CLI `--format`）：`48000/16/2`、`48000/32/2f`、非法输入。
- **设备/集成（硬件手动）**：
  - `probe` 对默认设备的常见格式应返回支持；对明显非法格式返回不支持。
  - Exclusive 采集写 wav（wav 头格式 == 协商格式）；Exclusive 播放该 wav。
  - **Shared 回归**：现有 14 个 gtest 全过；CLI/GUI 的 Shared 采集/播放冒烟与 Phase 1 行为一致（含正确的 wav 头格式）。

## 7. 范围与非目标

- 本阶段只做 WASAPI-Exclusive + WASAPI 后端重构 + 上述接入。
- 非目标（后续阶段）：waveIn/waveOut（MME）后端；延迟/glitch 数值测量；信号质量 loopback 对比；格式探测的完整 UI 面板（本阶段只放探测按钮入口）；采样率转换/重采样（播放仍要求 wav 格式与协商格式一致）。
