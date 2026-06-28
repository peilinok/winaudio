# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目状态

WinAudio 是一个小巧的 Windows 音频测试工具，用于用多种音频技术（waveIn/waveOut、WASAPI）对系统音频设备做采集、播放等测试与对比。

**当前仓库为空，处于初始搭建阶段。** 本文件记录已确定的技术选型与目标架构，供后续实例据此搭建；当代码落地后，应同步用真实的文件/命令更新本文件，并删除本节中与现状不符的描述。

## 技术选型（已确定）

- 语言/构建：**C++ + MSBuild**，以 Visual Studio 解决方案（`.sln`）+ 工程（`.vcxproj`）组织，使用 MSVC 工具链。
- 形态：**命令行 CLI**——通过子命令/参数选择设备、后端 API、采集或播放模式，输出日志。
- 首批能力范围：
  - **WASAPI**：`IAudioClient` 采集与播放，需同时支持**共享模式**与**独占模式**。
  - **waveIn / waveOut**：经典 MME API，作为兼容性对照后端。
  - **设备枚举/查询**：`IMMDeviceEnumerator` 枚举设备、查询默认设备、混音格式与端点属性。
  - **WAV 文件读写**：采集可存为 `.wav`，也可从 `.wav` 播放，便于离线验证。

## 构建与运行（MSBuild 约定）

```powershell
# 配置/打开开发环境：使用 "Developer PowerShell for VS" 或先调用 vcvarsall.bat 使 msbuild 在 PATH 中

# 构建（解决方案文件落地后）
msbuild WinAudio.sln /p:Configuration=Debug   /p:Platform=x64
msbuild WinAudio.sln /p:Configuration=Release /p:Platform=x64

# 仅重建单个工程
msbuild src\WinAudio.vcxproj /t:Rebuild /p:Configuration=Debug /p:Platform=x64

# 运行（产物路径取决于工程 OutDir，默认形如 x64\Debug\WinAudio.exe）
x64\Debug\WinAudio.exe <subcommand> [options]
```

> 注：上面的 `.sln`/`.vcxproj`/产物路径为目标约定，首次搭建后请按实际文件名校正。优先用 `x64` 平台（WASAPI 项目通常如此）。

## 目标架构（多后端抽象）

工具的核心价值在于"同一套上层流程，可切换不同音频后端做对比测试"。为此推荐如下分层，后续搭建应遵循：

- **后端抽象层**：定义统一的采集/播放接口（如 `IAudioCapture` / `IAudioRender`），约定生命周期（open → start → 数据回调/拉取 → stop → close）与统一的 PCM 格式描述（采样率、位深、声道、`WAVEFORMATEX`/`WAVEFORMATEXTENSIBLE`）。
- **后端实现**：每种技术一个实现，互不依赖——
  - `WasapiCapture` / `WasapiRender`（区分 shared/exclusive，处理事件驱动 vs 定时拉取）。
  - `WaveInCapture` / `WaveOutRender`（基于回调 + 多缓冲队列）。
- **设备层**：基于 `IMMDeviceEnumerator` 的枚举/查询，输出设备 id、友好名、默认端点、支持格式，供 CLI 列举与选择。
- **WAV I/O**：独立的 `.wav` 读写模块，被采集（落盘）与播放（读取）复用，避免与后端耦合。
- **CLI 层**：解析子命令/参数，组装"设备 + 后端 + 模式 + 数据源/汇"并驱动流程，负责日志与错误呈现。

关键耦合点（实现时重点关注）：COM 初始化与线程模型（WASAPI 需 `CoInitializeEx`，独占/事件模式对线程与缓冲时序敏感）；waveIn/waveOut 与 WASAPI 的缓冲模型差异需被抽象层屏蔽；格式协商（共享模式用混音格式，独占模式需 `IsFormatSupported` 探测）。

## 约定

- 回答默认用中文；代码、命令、API 标识符等保持英文原文。
