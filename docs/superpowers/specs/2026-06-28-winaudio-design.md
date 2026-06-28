# WinAudio 设计文档

- 日期：2026-06-28
- 状态：已评审通过，待生成实现计划
- 形态：Windows 桌面音频测试工具

## 1. 背景与目标

WinAudio 是一个小巧的 Windows 音频测试工具，用于用多种音频技术
（waveIn/waveOut、WASAPI）对系统音频设备做采集、播放等测试与对比。

四类目标测试场景（全部纳入路线，分阶段交付）：

1. **通路打通验证** —— 某设备 + 某 API + 某格式能否成功采集/播放。
2. **延迟/性能测量** —— 采集/播放延迟、回调间隔抖动、glitch/丢帧、缓冲 xrun。
3. **格式/能力探测** —— 设备支持的采样率/位深/声道，共享 vs 独占差异，
   混音格式与 `IsFormatSupported` 探测。
4. **信号质量对比** —— loopback 采集+播放，对比不同后端的信号完整性。

## 2. 技术选型（已确定）

- 语言/构建：**C++ + MSBuild**，Visual Studio 解决方案（`.sln`）+ 工程
  （`.vcxproj`），MSVC 工具链，**x64**。
- **WinAudioCore / WinAudioCli**：纯 **Win32 + STL，零第三方依赖**
  （WAV 读写、CLI 解析、日志、单测断言全部自写）。
- **WinAudioGui**：破例引入 **Dear ImGui**（渲染后端 `imgui_impl_win32` +
  `imgui_impl_dx11`，DX11 属 Windows SDK）。
- 第三方依赖**仅出现在 GUI 前端（Dear ImGui）与测试工程（gtest）**；
  WinAudioCore 与 WinAudioCli 保持纯 Win32 + STL 零依赖。
- 前端定位：**GUI(Dear ImGui) 为主**，CLI 最小化（调试/脚本入口）。

## 3. 首批范围（MVP）与分阶段

- **MVP**：WinAudioCore + GUI 完成 **设备枚举/查询 + 单后端采集落盘 wav +
  从 wav 播放**，并把多后端抽象层与前端骨架立好。CLI 保留最小调试入口。
- **后续阶段**（在同一 Core 上叠加，不返工）：
  - 阶段 2：补齐全部 4 后端（waveIn/waveOut + WASAPI 共享/独占）采集播放。
  - 阶段 3：延迟/性能测量（基于 RingBuffer 计数 + 时间戳）。
  - 阶段 4：格式/能力探测面板（`IsFormatSupported` 等）。
  - 阶段 5：信号质量对比（loopback）。

## 4. 顶层架构

核心做成 UI 无关的静态库，CLI 与 GUI 是它的两个前端，共享同一套后端与测量逻辑。

```
   WinAudioCore (.lib)   ← Engine + 4后端 + RingBuffer + DeviceEnumerator
        ▲          ▲        + AudioFormat + Wav I/O（纯 Win32+STL，零依赖）
        │ link     │ link
   WinAudioCli.exe   WinAudioGui.exe (Dear ImGui + DX11，仅此前端含第三方)
```

分层（方案 C：后端自持线程 + 无锁 SPSC 环形缓冲解耦）：

```
   CLI 层 / GUI 层      参数解析或 ImGui 界面 / 日志或界面刷新
        │ 组装并驱动（仅通过 Engine 接口，不进音频线程）
   编排层 Engine        采集: backend → ring → WavWriter
                       播放: WavReader → ring → backend
        │
   后端层(各自持线程)   WasapiCapture/Render  WaveInCapture/WaveOutRender
        │  ←→ RingBuffer(SPSC 无锁, 带 xrun 计数) ←→ Engine
   设备层               DeviceEnumerator(IMMDevice…)  AudioFormat(WAVEFORMATEX 封装)
```

### 选型理由（为什么是方案 C）

waveIn/waveOut（回调驱动，MM 线程）与 WASAPI（事件驱动，自有 I/O 线程）的
线程与缓冲模型差异极大。让每个后端内部按各自原生模型跑、对外只经一个 SPSC
ring buffer 交换 PCM + 时间戳，可彻底屏蔽差异；ring buffer 自带 over/underrun
计数，天然支撑阶段 3 的 glitch/延迟/下溢观测；各后端互相隔离、可独立测试。
唯一成本是一个可单测的无锁 SPSC ring buffer。

（已否决方案：A 回调式——上层逻辑被迫在受限的后端回调线程跑、抽象会漏；
B 阻塞读写式——把"推"硬包成阻塞读写，等于藏了个 ring buffer 却没暴露控制点。）

## 5. 模块职责

每个单元单一职责、可独立理解与测试。

- **AudioFormat**：统一 PCM 格式描述，与 `WAVEFORMATEX`/`WAVEFORMATEXTENSIBLE`
  互转。被所有层共用，不依赖任何后端。
- **RingBuffer**：单生产者单消费者无锁环形字节缓冲，含 over/underrun 计数。
  后端与 Engine 之间唯一数据通道。纯逻辑、不碰音频 API。
- **DeviceEnumerator**：基于 `IMMDeviceEnumerator` 枚举设备、查默认端点、
  查混音格式/端点属性。仅查询，不持流。
- **后端实现（4 个，互不依赖，各自内部持 I/O 线程）**：
  `WasapiCapture/Render`（event-driven，构造参数区分 shared/exclusive）、
  `WaveInCapture/WaveOutRender`（回调 + 多缓冲队列）。统一接口
  `open/start/stop/close` + 一个 RingBuffer 端点 + `stats()`。
- **WavReader/WavWriter**：`.wav`（RIFF/PCM）读写，被采集落盘与播放读取复用，
  不依赖后端。
- **Engine**：编排——按选择实例化设备/后端/数据源汇，连上 ring buffer，跑
  采集或播放流程，收集计数与电平。**不 print、不碰 UI。**
- **CLI / GUI**：把同一套 Engine 调用分别包成命令行 + 定时 `poll()` 打印 /
  ImGui 界面 + 每帧 `poll()` 刷新。两前端不实现彼此没有的逻辑，保证行为一致。

## 6. 核心接口

```cpp
struct AudioFormat {
    uint32_t sampleRate;     // 48000…
    uint16_t channels;       // 1/2…
    uint16_t bitsPerSample;  // 16/24/32
    bool     isFloat;        // PCM int vs IEEE float
    // toWaveFormat() / fromWaveFormat()  与 WAVEFORMATEX/EXTENSIBLE 互转
};

class RingBuffer {                                   // SPSC 无锁, 按 frame 对齐
    size_t   write(const void* data, size_t bytes);  // 生产者(后端采集线程)
    size_t   read(void* out, size_t bytes);          // 消费者(Engine 线程)
    uint64_t overruns()  const;                      // 写满丢弃 → glitch 指标
    uint64_t underruns() const;                      // 读空计数
};

class IAudioBackend {                                // 4 实现各自内部持 I/O 线程
    virtual bool open(const DeviceId&, const AudioFormat&, RingBuffer*) = 0;
    virtual bool start() = 0;   // 启动内部 I/O 线程
    virtual void stop()  = 0;
    virtual void close() = 0;
    virtual BackendStats stats() const = 0;  // 实际格式、缓冲帧数、xrun
};

class Engine {                                       // UI 无关, GUI 与 CLI 共用
    std::vector<DeviceInfo> enumerate(DataFlow);
    bool startCapture(sel, fmt, wavPath);            // backend→ring→WavWriter
    bool startPlayback(sel, wavPath);                // WavReader→ring→backend
    void stop();
    EngineStatus poll() const;   // 状态机 + 实时电平(RMS/峰值) + xrun 计数
};
```

## 7. 数据流

- **采集**：`Backend(采集线程) → RingBuffer → Engine(消费线程) → WavWriter`，
  Engine 顺带算 RMS/峰值供电平条。
- **播放**：`WavReader → Engine(喂数据线程) → RingBuffer → Backend(渲染线程)`。
- **UI 解耦**：UI 线程永不进音频线程，只通过 `Engine::poll()` 读快照。这是
  GUI 实时刷新与音频实时性互不阻塞的关键。

## 8. GUI 布局（Dear ImGui + DX11，单窗口，每帧 poll 刷新）

```
┌─ WinAudio ───────────────────────────────────────────────┐
│ [后端 ▼ WASAPI-Shared | WASAPI-Excl | waveIn/Out ]        │
│ [数据流 ◉ 采集  ○ 播放 ]                                   │
├───────────────────────────────────────────────────────────┤
│ 设备列表 (来自 Engine::enumerate)        [刷新设备]        │
├───────────────────────────────────────────────────────────┤
│ 格式: [48000▼]Hz [16▼]bit [2▼]ch □float                   │
│   独占模式: [探测此格式是否支持] (IsFormatSupported)        │
│ WAV 文件: [ … path … ] [浏览]                             │
├───────────────────────────────────────────────────────────┤
│ [▶ 开始] [■ 停止]   状态: ● Running 00:03                  │
│ 电平 L ▓▓▓▓▓▓░░░░  R ▓▓▓▓▓░░░░░   (RMS/峰值)               │
│ 计数: overrun 0  underrun 0  实际格式 48k/16/2             │
├───────────────────────────────────────────────────────────┤
│ 日志窗格 (复用 Core 状态/错误消息)                         │
└───────────────────────────────────────────────────────────┘
```

- 控件状态映射到一次启动配置（后端+数据流+设备+格式+wav 路径）；「开始」即调
  `startCapture/startPlayback`。
- 电平条、计时、xrun、实际格式全部来自 `Engine::poll()` 快照。
- 「探测格式」按钮调设备层 `IsFormatSupported`（阶段 4 入口）。

## 9. 最小 CLI

```
WinAudioCli list [--render|--capture]
WinAudioCli capture --backend wasapi-shared --device <id> \
                    --format 48000/16/2 --out cap.wav [--seconds N]
WinAudioCli play    --backend wasapi-shared --device <id> --in cap.wav
```

CLI 只把同一套 Engine 调用包成命令行 + 定时 `poll()` 打印，不实现 GUI 没有的逻辑。

## 10. 错误处理

- **错误传递**：Core 不抛异常穿透 UI、不自己 print。操作返回 `bool` +
  结构化 `Result{ ok, code, message }`；UI 层负责呈现（GUI 日志窗格 / CLI stderr）。
- **HRESULT 归一**：WASAPI/COM 的 `HRESULT` 在后端内部转统一错误码 + 可读消息
  （含 `AUDCLNT_E_*` 常见项：设备被独占、格式不支持、
  `AUDCLNT_E_DEVICE_INVALIDATED` 设备拔出）。
- **COM 生命周期**：每个 WASAPI 后端线程自行 `CoInitializeEx(COINIT_MULTITHREADED)`
  并配对 `CoUninitialize`；COM 接口一律 RAII 智能指针包裹，禁止裸 `Release`。
- **设备失效/拔出**：途中失效 → 后端线程检测后置 Engine 状态 `Error` 并停流；
  GUI 弹日志、按钮回到可重启状态，不崩。
- **xrun 不致命**：over/underrun 只计数、继续跑（这是要观测的测试指标）。
- **格式协商失败**：共享模式取设备混音格式；独占模式 `IsFormatSupported` 失败时
  返回明确「该格式不被支持」，不静默改格式。

## 11. 测试策略

- **RingBuffer 单测**：边界（空/满/回绕）、SPSC 并发读写压力、xrun 计数正确性。
- **AudioFormat 单测**：与 `WAVEFORMATEX`/`EXTENSIBLE` 双向互转、float/int、
  24bit 对齐。
- **Wav I/O 单测**：写出再读回 round-trip 比对；坏头/截断文件容错。
- **后端**：loopback 半自动验证（capture wav 后 play wav 人工确认，或采集默认渲染
  端点 loopback 比对）。依赖真实声卡，归手动/集成测试。
- **测试框架**：**gtest**，`WinAudioTests.exe` 工程链接 gtest 并聚合运行。
  gtest 源码 vendored 于 `third_party/googletest/`，**仅 `WinAudioTests` 工程引用**；
  Core/Cli 仍保持纯 Win32+STL 零依赖。

## 12. 工程结构（目标）

```
WinAudio.sln
  WinAudioCore.vcxproj    静态库, 纯 Win32+STL
  WinAudioCli.vcxproj     控制台 exe, 链接 Core
  WinAudioGui.vcxproj     窗口 exe, 链接 Core + Dear ImGui + DX11
  WinAudioTests.vcxproj   控制台 exe, gtest, 链接 Core
  third_party/imgui/      Dear ImGui 源码(仅 GUI 工程引用)
  third_party/googletest/ gtest 源码(仅 Tests 工程引用)
```
（首次搭建后按实际文件名校正 CLAUDE.md 中的命令与产物路径。）
