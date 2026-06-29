# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目状态

WinAudio 是一个小巧的 Windows 音频测试工具，用于对系统音频设备做采集、播放等测试。

**Phase 2 已完成。** 当前代码库包含 4 个项目：
- **WinAudioCore**（`src/core`）：纯 C++ 静态库，零外部依赖（仅 Win32 + STL）；提供后端抽象（`IAudioBackend`）、设备枚举（`DeviceEnumerator`）、WASAPI-Shared/Exclusive 采集/播放、WAV 读写、环形缓冲区（RingBuffer）、引擎（Engine）。
- **WinAudioCli**（`src/cli`）：最小化命令行前端，支持 `list` / `capture` / `play` / `probe` 子命令。
- **WinAudioGui**（`src/gui`）：Dear ImGui + DX11 GUI 前端，为首选交互方式；含后端选择器（Shared/Exclusive）与格式控件。
- **WinAudioTests**（`src/tests`）：gtest 单元测试套件，覆盖核心模块（RingBuffer、AudioFormat、WAV、FormatSpec、WasapiStream 辅助函数）。

Phase 1 实现了 **WASAPI-Shared 采集/播放**；Phase 2 在此基础上新增了 **WASAPI-Exclusive（独占模式，低延迟，使用 `IsFormatSupported` 进行格式探测）**。waveIn/waveOut 与格式转换/重采样留作后续阶段。

## 技术选型

- 语言/构建：**C++ + MSBuild**，Visual Studio 解决方案（`WinAudio.sln`）+ 工程（`.vcxproj`）。
- 形态：**CLI + GUI**；ImGui + DX11 为主交互，CLI 为轻量辅助。
- 能力范围（已实现）：
  - **WASAPI-Shared**：`IAudioClient` 采集与播放，共享模式（多应用可并行使用设备，格式由设备混音格式决定）。
  - **WASAPI-Exclusive**：独占模式（低延迟），通过 `IsFormatSupported` 探测可用格式；采集时支持 `--format` 指定目标格式；播放时格式从 WAV 文件头自动读取。
  - **格式探测**：`Engine::probeFormat` + CLI `probe` 子命令，可检测特定格式是否受设备支持。
  - **设备枚举**：`IMMDeviceEnumerator` 枚举、查询默认设备与端点属性。
  - **WAV 读写**：采集落盘 / 播放读取，支持标准 RIFF WAVE 格式。
  - **环形缓冲区**：SPSC 设计，线程安全，带 xrun 计数。
  - **错误处理**：`Result` 类型、HRESULT 规范化、COM RAII 与线程安全。
- 能力范围（后续阶段）：
  - waveIn / waveOut（MME API 后端）。
  - 格式转换/重采样（当前 Shared 模式要求输入 WAV 格式与设备混音格式一致；Exclusive 模式要求格式受设备支持）。

## 构建与运行

```powershell
# msbuild 不在 PATH 中；使用 VS 2022 安装路径
$MSBuild = "D:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"

# Debug 构建
& $MSBuild WinAudio.sln /p:Configuration=Debug /p:Platform=x64 /m

# Release 构建
& $MSBuild WinAudio.sln /p:Configuration=Release /p:Platform=x64 /m

# 单个工程重建（例如仅核心库）
& $MSBuild src\core\WinAudioCore.vcxproj /t:Rebuild /p:Configuration=Debug /p:Platform=x64

# 运行单元测试（gtest）
.\x64\Debug\WinAudioTests.exe                          # 全部测试（22 个）
.\x64\Debug\WinAudioTests.exe --gtest_filter=RingBuffer.*  # 筛选指定套件

# ---- CLI 用法 ----

# 枚举设备
.\x64\Debug\WinAudioCli.exe list [--render|--capture]

# 采集（Shared 默认用设备混音格式；Exclusive 可用 --format 指定，省略时自动从候选列表协商）
.\x64\Debug\WinAudioCli.exe capture --out <file.wav> [--seconds N] [--device <id>] [--backend wasapi-shared|wasapi-exclusive] [--format 48000/16/2]
# 注：--format 仅对 wasapi-exclusive 有效；用于 shared 后端时报错退出。

# 播放（格式从 WAV 文件头读取；不接受 --format）
.\x64\Debug\WinAudioCli.exe play --in <file.wav> [--device <id>] [--backend wasapi-shared|wasapi-exclusive]

# 格式探测（检测设备是否支持指定格式）
.\x64\Debug\WinAudioCli.exe probe --format 48000/16/2 [--device <id>] [--render|--capture] [--backend wasapi-shared|wasapi-exclusive]
# 注：exclusive probe 精确反映独占可用性；shared probe 反映 WASAPI 共享混音器能否转换该格式，
#     而本工具 shared 采集/播放始终用设备混音格式（不重采样），故 shared 的 SUPPORTED 不代表本工具会按该格式工作。

# ---- GUI（首选） ----
.\x64\Debug\WinAudioGui.exe
# GUI 含后端选择器（Shared / Exclusive）；选 Exclusive 时，Rate/Bits/Ch/float 控件
# 与"Probe format"按钮会启用（用于 capture 与 probe；playback 自动使用 WAV 格式）。
```

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
- **BackendKind**：`WasapiShared` / `WasapiExclusive`，由 CLI `--backend` 参数或 GUI 选择器决定，传入 `Engine`。
- **COM 与线程安全**：`CoInitializeEx` 包装（RAII）；WASAPI 线程模型约定；原子操作与互斥锁。
- **错误处理**：`Result` 类型，HRESULT 到 string 映射，用户友好报错。

## 约定

- 回答默认用中文；代码、命令、API 标识符等保持英文原文。
