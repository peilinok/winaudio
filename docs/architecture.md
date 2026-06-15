# WinAudio Architecture

本文档面向长期维护，描述 WinAudio 当前的主要分层、运行数据流、构建与测试关系，以及后续修改时应遵守的边界。文档内容以当前仓库实现为准，尤其覆盖近期已经落地的 RTC sidecar 解耦。

## 1. 设计目标

- 主音频功能优先：本地 capture、render、resampler、probe、matrix、capture-open、GUI 普通会话必须先独立成立。
- GUI/CLI 共享语义：配置项、状态名、失败阶段、RTC 展示文本尽量走同一套模型和 helper。
- RTC 作为小子集：RTC 是可选 sidecar，不反向控制主音频链路的启动、运行与 probe 成败。
- 测试分层清晰：默认基线不依赖真实硬件和本机 RTC 运行库，环境相关验证单独放到更外层。

## 2. 总体分层

当前核心结构可以概括为：

```text
winaudio.exe / winaudio_probe.exe
            │
            ▼
         AppModel
            │
            ├── probe_ui_text.*
            ├── device_notification_client.*
            └── AudioSessionController
                    │
                    ├── IAudioBackendFactory
                    │   ├── IAudioCaptureAdapter
                    │   └── IAudioRenderAdapter
                    ├── IAudioResampler
                    ├── AudioRingBuffer
                    ├── WavDumpWriter
                    └── RtcSidecar
                         └── AgoraRtcPublisher
                              └── agora_rtc_sdk.dll (optional runtime)
```

从构建目标上看：

```text
CMakeLists.txt
  ├── winaudio_core
  │    ├── src/audio/*
  │    ├── src/app/*
  │    └── src/rtc/*
  ├── winaudio.exe
  ├── winaudio_probe.exe
  └── *_test.exe
```

`winaudio_core` 是共享核心库。GUI、CLI 和测试基本都直接链接它，因此长期维护时应优先把行为边界稳在 core 内，而不是把语义散落到入口层。

## 3. 关键模块职责

| 模块 | 主要文件 | 核心职责 | 不应承担的职责 |
| --- | --- | --- | --- |
| GUI 入口 | `src/app/win_audio_app.cpp` | Win32 控件、事件转发、刷新 UI | 不直接实现音频生命周期，不直接拼接复杂 RTC 语义 |
| CLI 入口 | `src/app/probe_cli_main.cpp` | 参数分发、模式选择、退出码映射 | 不重复实现会话状态机，不直接决定 RTC 展示文本格式 |
| 应用模型 | `src/app/app_model.*` | 共享配置状态、设备刷新、probe 编排、缓存文本快照、桥接控制器与 UI/CLI | 不承担底层音频 I/O 细节，不直接实现 Agora SDK 调用 |
| 文本 helper | `src/app/probe_ui_text.*` | GUI/CLI 共用文案、RTC 状态文本、标签和按钮文案 | 不保存状态，不直接操作控制器 |
| 主会话控制器 | `src/audio/audio_session_controller.*` | 主音频链路启动、停止、Tick、主链路诊断与 waveform/stats 更新 | 不应让 RTC 失败决定主会话成败 |
| RTC sidecar | `src/audio/rtc_sidecar.*` | RTC publisher 生命周期、附加、发布失败后的停用、RTC 专属状态收敛 | 不接管主音频链路，不写主 `last_error_*` |
| RTC publisher 抽象 | `src/rtc/agora_rtc_publisher.*` | 运行时可用性、自定义 factory、真实 SDK 封装 | 不暴露到 GUI/CLI 入口做大面积分支 |
| 后端与管线 | `src/audio/backends/*`、`src/audio/pipeline/*`、`src/audio/resample/*` | 设备枚举、WASAPI/Wave 适配、ring buffer、dump、重采样、分析器 | 不承担 probe/GUI/CLI 的呈现逻辑 |

## 4. 主音频链路数据流

### 4.1 启动链路

普通 GUI 会话、`quick`、`matrix`、`capture-open` 最终都会走到 `AppModel -> AudioSessionController::Start()`。

当前主路径大致如下：

```text
SessionConfiguration
    ▼
AppModel::Start() / Run*Probe()
    ▼
AudioSessionController::Start()
    ├── 创建 capture/render adapter
    ├── 创建 resampler / dump writer / ring buffer
    ├── 解析设备与格式
    ├── 启动 capture
    ├── 按需启动 render monitor
    └── 若 rtc.enabled，则尝试附加 RtcSidecar
            失败时只记 RTC 日志和 RTC 状态，不让 Start() 失败
```

几个边界需要长期保持：

- `Start()` 成败只由主音频链路决定。
- `System Loopback` 仍可在主链路层面强制关闭 monitor playback，以避免 loopback storm。
- RTC 只是在主链路已成立后“尝试附加”的可选能力。

### 4.2 Tick 链路

运行中的主路径大致如下：

```text
AudioSessionController::Tick()
    ├── 从 capture 读取 chunk
    ├── 更新 capture analyzer / stats / waveform
    ├── 按需 dump 原始数据
    ├── 经过 resampler / ring buffer 输送到 render
    ├── 更新 render analyzer / stats / waveform
    └── 把 chunk 送给 RtcSidecar::Publish()
            发布失败时：
            - 只停用 RTC sidecar
            - 只更新 rtc_stats / rtc_text / 日志
            - 主音频 Tick 继续成功
```

这条边界是当前 RTC 解耦的核心：RTC 发布失败不再是主会话失败。

### 4.3 错误归属

当前应当维持以下规则：

- `SessionRuntimeStats.last_error_stage` / `last_error_message`
  - 只表示主音频链路错误
  - 例如设备解析失败、capture start 失败、render write 失败、resampler 主链路失败
- RTC 相关错误只进入：
  - `AgoraRtcStats.last_error_*`
  - `rtc_text()`
  - 日志

如果后续再扩展 sidecar 能力，也应沿用这类“主链路错误”和“附属能力错误”分仓语义。

## 5. RTC Sidecar 约束

RTC 当前不是第三种本地 backend，而是“本地采集结果的可选旁路发布”。

### 5.1 生命周期边界

当前 RTC 分层如下：

```text
AudioSessionController
    └── RtcSidecar
         ├── Initialize()
         ├── Attach()
         ├── Publish()
         ├── Detach()
         └── stats() / runtime_status()
```

维护时应继续遵守：

- `AudioSessionController` 只通过少量方法与 RTC sidecar 交互。
- `RtcSidecar` 统一负责 runtime 可用性、Join/Leave、发布失败后停用、RTC 日志与状态缓存。
- 真实 Agora SDK 的选择和构建态差异，优先收敛在 `AgoraRtcPublisher` 抽象和 factory 注入层。

### 5.2 模式契约

| 入口模式 | RTC 不可用时的预期 | 退出/结果语义 |
| --- | --- | --- |
| GUI 普通 Start | 主音频链路继续运行，RTC 状态显示 disabled/error | 会话可 Running |
| `quick` | probe 继续执行，`rtc_text` 显示不可用原因 | 只按主链路 probe 结果成功或失败 |
| `matrix` | matrix 继续执行 | 不因 RTC 缺失而整体失败 |
| `capture-open` | 继续执行主链路检查 | 不因 RTC 缺失而失败 |
| `winaudio_probe rtc` | 视为 RTC 专用健康检查 | RTC 不可用、Join 失败、运行中 Publish 失败都应非零退出 |

### 5.3 可测试性约束

RTC 相关测试应继续依赖 `AgoraRtcPublisherFactory` 注入，而不是依赖：

- `WINAUDIO_ENABLE_AGORA_SDK` 当前是否打开
- 本机是否存在 `agora_rtc_sdk.dll`
- 当前构建目录是否拷贝了运行库

推荐的 fake/stub 场景：

- runtime unavailable
- runtime available + join success
- runtime available + publish failure

## 6. UI / CLI 共享语义

当前 UI/CLI 的共享语义主要通过两层收敛：

- `AppModel`
  - 持有配置、设备快照、probe 结果、日志、runtime 状态
  - 向 GUI 与 CLI 暴露统一查询接口
- `probe_ui_text`
  - 负责把状态转换为标签、摘要、RTC 状态文本、按钮文案和 capability 描述

维护时优先遵守：

- 不要在 `win_audio_app.cpp` 和 `probe_cli_main.cpp` 里重复拼接 RTC 文本。
- 新增 RTC 展示语义时，优先落到 `probe_ui_text.*`。
- `AppModel` 更适合负责“状态查询和缓存”，而不是“长段文案拼接”。

这套约束的目标是减少 GUI/CLI 对内部字段的直接耦合，避免同一状态在多处被手写成不同文案。

## 7. 构建关系

### 7.1 默认构建

默认构建：

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

默认情况下：

- `winaudio_core`、`winaudio`、`winaudio_probe`、测试目标都会构建
- RTC 抽象层始终存在
- 真实 Agora SDK 集成默认关闭

### 7.2 Agora SDK 可选集成

`WINAUDIO_ENABLE_AGORA_SDK=ON` 时，构建会额外：

- 引入 Agora SDK 头文件
- 定义 `WINAUDIO_ENABLE_AGORA_SDK`
- 在构建后把 Agora runtime DLL 复制到相关目标目录

这层可选构建不应改变主音频功能的基本契约。换句话说，是否开启 Agora SDK 只应影响 RTC sidecar 的真实可用性，不应影响主链路测试的通过语义。

## 8. 测试与验证关系

当前测试层大致如下：

```text
hosted-stable / 默认 CTest
  ├── core_pipeline_test
  ├── session_controller_test
  ├── wave_format_utils_test
  ├── app_model_text_test
  ├── probe_cli_test
  ├── probe_ui_text_test
  ├── build_environment_tools_test
  └── convergence_helpers_test

本地环境相关
  ├── cli_integration_test
  ├── gui_smoke_test (opt-in)
  └── hardware_validation_test (opt-in)
```

### 8.1 默认基线

默认 `CTest` 和 hosted-stable 基线强调：

- 无真实硬件强依赖
- 输出语义稳定
- CLI/GUI 文本契约稳定
- RTC 测试可通过注入 fake publisher 覆盖关键分支

### 8.2 环境相关验证

以下验证依赖更真实的本机环境：

- `cli_integration_test`
  - 依赖可枚举的设备和部分 loopback 能力
- `gui_smoke_test`
  - 依赖桌面交互环境
- `hardware_validation_test`
  - 依赖真实设备和驱动状态

### 8.3 变更到测试的映射

| 改动类型 | 最少应检查的验证层 |
| --- | --- |
| 主链路生命周期、错误边界 | `session_controller_test` |
| CLI 解析与退出码 | `probe_cli_test` |
| GUI/CLI 文本、RTC 文案、capability 文本 | `app_model_text_test`、`probe_ui_text_test` |
| 数据管线、重采样、waveform | `core_pipeline_test` |
| 构建脚本或验证脚本 | `build_environment_tools_test`、`convergence_helpers_test` |
| 真实设备发现或桌面路径 | `cli_integration_test`、`gui_smoke_test`、`hardware_validation_test` |

## 9. 维护约定

后续更新建议遵守以下清单：

1. 改主链路生命周期时，先判断 RTC 是否仍是 sidecar，而不是让它重新进入主启动链路。
2. 改用户可见文案时，同时更新 README、`probe_ui_text` 相关测试和必要的脚本断言。
3. 改 RTC 行为时，优先补 fake publisher 测试，不要把本机 DLL 是否存在写死进单测。
4. 新增模块时，优先先判断它属于入口层、AppModel 层、主链路层还是 sidecar 层，再决定文件放置位置。
5. 如果 `docs/architecture.md` 描述的分层已经失真，应在同一批变更里同步更新本文件。

## 10. 推荐阅读顺序

第一次接手仓库时，建议按以下顺序建立上下文：

1. `README.md`
2. `docs/architecture.md`
3. `CMakeLists.txt`
4. `src/app/app_model.*`
5. `src/audio/audio_session_controller.*`
6. `src/audio/rtc_sidecar.*`
7. `src/app/probe_ui_text.*`
8. `tests/session_controller_test.cpp`
9. `tests/app_model_text_test.cpp`
10. `tests/probe_ui_text_test.cpp`
