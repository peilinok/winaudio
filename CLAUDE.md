# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目状态

WinAudio 是一个小巧的 Windows 音频测试工具，用于对系统音频设备做采集、播放等测试。

**Phase 3 已完成。** 当前代码库包含 4 个项目：
- **WinAudioCore**（`src/core`）：纯 C++ 静态库，零外部依赖（仅 Win32 + STL）；提供后端抽象（`IAudioBackend`）、设备枚举（`DeviceEnumerator`）、WASAPI-Shared/Exclusive 采集/播放、WAV 读写、环形缓冲区（RingBuffer）、引擎（Engine）、双流延迟监听（MonitorEngine、DelayFifo）、FFT 分析（Fft、SampleConvert、ScopeBuffer、Analysis）。
- **WinAudioCli**（`src/cli`）：最小化命令行前端，支持 `list` / `capture` / `play` / `probe` / `monitor` 子命令。
- **WinAudioGui**（`src/gui`）：Dear ImGui + DX11 GUI 前端，为首选交互方式；**恒监听**（monitor-only），两列布局（左列：设备/控制/状态/日志；右列：波形+频谱+声谱图，可拖拽重排），"同步播放" checkbox 实时控制 render 流。
- **WinAudioTests**（`src/tests`）：gtest 单元测试套件，覆盖核心模块（RingBuffer、AudioFormat、WAV、FormatSpec、WasapiStream 辅助函数、Fft、SampleConvert、ScopeBuffer、DelayFifo、Analysis、MonitorEngine、Spectrogram）。

Phase 1 实现了 **WASAPI-Shared 采集/播放**；Phase 2 新增了 **WASAPI-Exclusive（独占模式，低延迟）**；Phase 3 新增了**双流延迟监听直通（MonitorEngine）与实时 GUI 可视化（波形 + 频谱 + 声谱图）**。waveIn/waveOut 与格式转换/重采样留作后续阶段。

**构建系统已迁移到 CMake（已完成）**：从手写 MSBuild（`.sln`/`.vcxproj`）迁移到 CMake（Visual Studio 17 2022 生成器，保留 MSVC；`build.bat`/`test.bat` 驱动，产物集中在 `build/`），零 C++ 源码改动。详见「技术选型」「构建与运行」。

## 技术选型

- 语言/构建：**C++ + CMake**（Visual Studio 17 2022 生成器，保留 MSVC v143）；工程文件（`.sln`/`.vcxproj`）由 CMake 生成到 `build/`，命令行/批处理（`build.bat`）驱动，不依赖 VS IDE。
- 形态：**CLI + GUI**；ImGui + DX11 为主交互，CLI 为轻量辅助。
- 能力范围（已实现）：
  - **WASAPI-Shared**：`IAudioClient` 采集与播放，共享模式（多应用可并行使用设备，格式由设备混音格式决定）。
  - **WASAPI-Exclusive**：独占模式（低延迟），通过 `IsFormatSupported` 探测可用格式；采集时支持 `--format` 指定目标格式；播放时格式从 WAV 文件头自动读取。
  - **格式探测**：`Engine::probeFormat` + CLI `probe` 子命令，可检测特定格式是否受设备支持。
  - **设备枚举**：`IMMDeviceEnumerator` 枚举、查询默认设备与端点属性。
  - **WAV 读写**：采集落盘 / 播放读取，支持标准 RIFF WAVE 格式。
  - **环形缓冲区**：SPSC 设计，线程安全，带 xrun 计数。
  - **双流延迟监听直通**：`MonitorEngine`（capture → `DelayFifo` 漂移控制 → render，两路 scope tap）；CLI `monitor` 子命令打印 cap/ren 状态、sr、fifo ms、drift、xrun。
  - **FFT 实时分析**：手写 radix-2 FFT（`Fft`，Hann 窗 + dBFS 标定 + 窗长归一化）；`SampleConvert`（多格式 PCM 解交织）；`ScopeBuffer`（seqlock 快照，无阻塞读取）；`Analysis`（采样计数节拍，驱动分析刷新）。
  - **GUI Monitor 模式**：ImPlot 可视化，采集/播放两路各自的时域波形 + log-X dBFS 频谱曲线 + 滚动 log 频率声谱图（`Spectrogram` 做 log 频率热图重采样）；状态行显示 fifo/drift 以便观察漂移。
  - **错误处理**：`Result` 类型、HRESULT 规范化、COM RAII 与线程安全。
- 能力范围（后续阶段）：
  - waveIn / waveOut（MME API 后端）。
  - 格式转换/重采样（当前 Shared 模式要求输入 WAV 格式与设备混音格式一致；Exclusive 模式要求格式受设备支持；monitor 模式要求采集与渲染采样率一致）。
- **已知限制**：
  - 分析为单声道降混（多声道信号取首声道或均值）。
  - `monitor` 要求采集与渲染设备采样率一致，否则启动报错。
  - 漂移补偿采用单帧丢/插 + crossfade，**无重采样器**；USB 麦 + HDMI 等跨时钟设备漂移率高，同接口 loopback 漂移很小。

## 构建与运行

本项目使用 **CMake**（Visual Studio 17 2022 生成器，保留 MSVC），命令行/批处理驱动，产物集中在 `build/`。

```powershell
# 构建（默认 Release；可传 Debug；--clean 先清后建）
.\build.bat Debug
.\build.bat Release
.\build.bat Release --clean

# 运行测试（ctest，60 个）
.\test.bat Debug          # 或 .\test.bat Release
# 也可直接跑测试 exe：
.\build\bin\Debug\WinAudioTests.exe
.\build\bin\Debug\WinAudioTests.exe --gtest_filter=MonitorEngine.*

# 清理
.\clean.bat

# 也可直接用 CMake / preset：
cmake --preset vs2022
cmake --build build --config Release -j
```

产物：`build\bin\<Config>\{WinAudioCli,WinAudioGui,WinAudioTests}.exe`、`build\lib\<Config>\WinAudioCore.lib`。
若 `cmake` 不在 PATH，用 VS 自带的：`D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`。

```powershell
# ---- CLI 用法 ----

# 枚举设备
.\build\bin\Debug\WinAudioCli.exe list [--render|--capture]

# 采集（Shared 默认用设备混音格式；Exclusive 可用 --format 指定，省略时自动从候选列表协商）
.\build\bin\Debug\WinAudioCli.exe capture --out <file.wav> [--seconds N] [--device <id>] [--backend wasapi-shared|wasapi-exclusive] [--format 48000/16/2]
# 注：--format 仅对 wasapi-exclusive 有效；用于 shared 后端时报错退出。

# 播放（格式从 WAV 文件头读取；不接受 --format）
.\build\bin\Debug\WinAudioCli.exe play --in <file.wav> [--device <id>] [--backend wasapi-shared|wasapi-exclusive]

# 格式探测（检测设备是否支持指定格式）
.\build\bin\Debug\WinAudioCli.exe probe --format 48000/16/2 [--device <id>] [--render|--capture] [--backend wasapi-shared|wasapi-exclusive]
# 注：exclusive probe 精确反映独占可用性；shared probe 反映 WASAPI 共享混音器能否转换该格式，
#     而本工具 shared 采集/播放始终用设备混音格式（不重采样），故 shared 的 SUPPORTED 不代表本工具会按该格式工作。

# 双流延迟监听（capture → delay → render，打印 cap/ren 状态、sr、fifo ms、drift、xrun）
.\build\bin\Debug\WinAudioCli.exe monitor [--cap <id>] [--render <id>] [--delay-ms N] [--seconds N] [--backend wasapi-shared|wasapi-exclusive]
# 注：--cap 与 --render 设备的采样率须一致，否则报错退出。

# ---- GUI（首选） ----
.\build\bin\Debug\WinAudioGui.exe
# GUI 为恒监听（monitor-only），无 Capture/Playback/Monitor 模式切换。
# 两列布局：左列 = 设备 / 控制（含"同步播放" checkbox）/ 状态 / 日志；
#           右列 = 波形 + 频谱 + 声谱图（可拖拽重排）。
# "同步播放" checkbox：勾选 → 启动 render 流实时直通（采样率须与采集设备一致）；
#                     取消勾选 → 停止 render 流并释放设备。
# 设备为下拉框选择,启动时默认选中系统默认设备;"Advanced..." 弹窗可配置高级流参数
# (category / stream option(RAW=绕过 APO)/ offload / ducking / buffer ms),
# 默认全部"跟随系统"(不注入任何覆盖);除 buffer 外仅 WASAPI-Shared 生效。
# 选择采集设备后点击 Start，实时显示时域波形、log-X dBFS 频谱曲线、滚动 log 频率声谱图；
# 状态行实时显示 fifo ms / drift，方便观察跨设备时钟漂移情况。
```

## CI / 发布（GitHub Actions）

- **`.github/workflows/ci.yml`**：push / PR 到 `main` 时，在 `windows-2022` 上以 **Debug + Release 双配置**（`windows-latest` 现已预装 VS 2026，与 `Visual Studio 17 2022` generator 不匹配，故 pin） 跑 `cmake --preset vs2022` → build → `ctest`，并上传 `WinAudioGui.exe` / `WinAudioCli.exe` 产物（artifact 名 `WinAudio-<config>`）。
- **`.github/workflows/release.yml`**：推送 `v*` tag（如 `v0.1.0`）时，构建 Release + `ctest`，打包 `WinAudio-<tag>-win-x64.zip` 并创建 GitHub Release（`generate_release_notes`）；需仓库 Actions 具备写权限。

## 架构（多后端抽象）

分层设计确保"同一上层流程，可切换不同后端做对比测试"：

- **核心库（WinAudioCore）**
  - `IAudioBackend` 接口：统一的采集/播放生命周期（open → start → poll → stop → close）。
  - `AudioFormat`：统一的 PCM 格式（采样率、位深、声道）与 `WAVEFORMATEX` 转换。
  - `DeviceEnumerator`：基于 `IMMDeviceEnumerator`，枚举设备、查询默认端点。
  - `WasapiStream`（基类）：WASAPI 公共脚手架，含模式感知的格式协商（Shared=`GetMixFormat`，Exclusive=`IsFormatSupported` + 对齐重试 `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED`）、事件驱动线程模型、同步启动握手。
    - `WasapiCaptureStream`：继承 `WasapiStream`，实现 `IAudioCaptureClient` 数据拉取循环。
    - `WasapiRenderStream`：继承 `WasapiStream`，实现 `IAudioRenderClient` 数据推送循环与静音预填充。
  - `RingBuffer`：SPSC 设计，原子操作，xrun 计数。
  - `WavReader` / `WavWriter`：标准 RIFF WAVE 格式支持，错误处理（坏头部检测）。
  - `Engine`：高层驱动，汇总设备 + 后端 + 数据源/汇，poll() 生成快照（电平、xrun、状态）。支持 `startCapture`、`startPlayback`、`probeFormat`。
  - `FormatSpec`：`parseFormatSpec`（解析 `R/B/C[f]` 字符串）、`defaultExclusiveCaptureCandidates`（常用独占格式候选列表）、`selectSupportedFormat`（遍历候选调用探测谓词）、`alignedBufferDuration100ns`（缓冲对齐辅助）。
  - `MonitorEngine`：双流监听引擎，capture → `DelayFifo` → render，两路 `ScopeBuffer` tap 供 GUI/分析读取，`Analysis` 驱动 FFT 刷新节拍。
  - `Fft`：手写 radix-2 FFT，Hann 窗，dBFS 标定（窗长归一化），纯 STL 无外部依赖。
  - `SampleConvert`：多格式 PCM（int16/int32/float32）解交织到 float，供 Fft 消费。
  - `ScopeBuffer`：seqlock 快照，GUI 线程无锁读取最新波形帧，音频线程写入不阻塞。
  - `DelayFifo`：固定延迟 FIFO，单帧丢/插 + crossfade 漂移补偿，无重采样。
  - `Analysis`：采样计数节拍，跨线程触发 FFT 计算，解耦音频回调与分析频率。
- **GUI 第三方库**（GUI 专用，不污染核心库）：
  - `third_party/implot`：ImPlot（Dear ImGui 的绘图扩展），提供波形、频谱曲线、声谱图热图渲染。
  - `src/gui/Spectrogram.*`：纯 STL 辅助类，将线性 FFT bin 重采样到 log 频率网格，生成热图列；有独立 gtest。
- **BackendKind**：`WasapiShared` / `WasapiExclusive`，由 CLI `--backend` 参数或 GUI 选择器决定，传入 `Engine`。
- **COM 与线程安全**：`CoInitializeEx` 包装（RAII）；WASAPI 线程模型约定；原子操作与互斥锁。
- **错误处理**：`Result` 类型，HRESULT 到 string 映射，用户友好报错。

## 约定

- 回答默认用中文；代码、命令、API 标识符等保持英文原文。
