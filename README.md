# WinAudio

WinAudio 是一个面向 Windows 的音频链路验证与诊断项目，提供 GUI 程序和 CLI 工具，用于枚举音频设备、启动采集/渲染会话、执行快速探测，以及批量验证多组音频配置组合。

当前仓库包含两个主要可执行文件：
- `winaudio`：Win32 GUI 程序，适合交互式配置、运行会话和观察诊断信息。
- `winaudio_probe`：命令行工具，适合脚本化探测、设备检查和集成验证。

## 主要能力

- 支持 `WASAPI` 和 `Wave API` 两类后端。
- 支持 `Microphone`、`System Loopback`、`Application Loopback` 三种采集来源。
- 支持 `Quick Probe`，用于单次链路探测和结果摘要输出。
- 支持 `Probe Matrix`，用于批量覆盖多组后端、格式、延迟、缓冲区和共享模式组合。
- 支持设备枚举，包括普通采集/渲染设备和 loopback 视角下的渲染端点。
- 支持监控播放开关、render auto-align、dump 到 `WAV`/`PCM` 等配置项。
- GUI 中提供会话摘要、运行诊断、能力说明、波形可视化和最近日志。

## 项目结构

- `src/audio`：音频后端、格式处理、会话控制、重采样、数据管线。
- `src/app`：应用模型、CLI 解析、GUI 文本、设备通知与应用入口。
- `src/ui`：GUI 波形绘制。
- `tests`：单元测试、文本语义测试、CLI 解析测试、控制器测试。
- `tools`：构建包装脚本、收敛检查、CLI 集成验证、GUI smoke、硬件验证。
- `docs`：收敛审计、GUI 手工核对清单、问题与积压说明。

## 环境要求

- Windows 10/11
- CMake 3.24 或更高版本
- Visual Studio 2022 或等价的 MSVC C++20 构建环境
- Windows SDK
- PowerShell

这是一个 Windows 专用项目。仓库当前直接链接 `ole32`、`mmdevapi`、`winmm`、`Mfplat`、`Mf`、`Mfcore` 等 Windows 音频/媒体组件。

## 项目治理约束

WinAudio 采用正式项目宪章治理，核心要求如下：

- Windows 音频语义优先：设计、实现与验证必须忠实反映 WASAPI/Wave API、设备枚举、loopback/app-loopback、共享/驱动模式与失败语义。
- GUI/CLI 契约一致：`winaudio` 与 `winaudio_probe` 对共享概念必须保持一致术语、状态名、设备标签、失败阶段与提示语义。
- 测试先于实现：凡影响核心音频逻辑、CLI 解析、GUI 状态语义、诊断文本或设备发现行为的改动，必须先更新测试、脚本断言或人工清单，再进入实现验收。
- 分层验证明确：feature plan 必须声明影响的是 baseline `CTest`、CLI integration、GUI smoke、hardware validation 还是 manual checklist；默认基线不得混入真实硬件依赖。
- 文档与验证材料同步：任何用户可见语义改动都不能只改代码，必须同步更新 README、脚本、文本测试、手工清单或相关验证文档。

宪章全文位于 `.specify/memory/constitution.md`。

## 构建

标准构建方式：

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

如果本机存在 `PATH/Path` 环境变量冲突，或者偶发 `LNK1168` 导致输出文件被占用，优先使用仓库自带的安全构建包装脚本：

```powershell
powershell -ExecutionPolicy Bypass -File tools\invoke_msbuild_safe.ps1 -PrintEnvironmentSummary cmake --build build --config Debug --target winaudio winaudio_probe
```

默认生成的主要产物位于：

- `build\Debug\winaudio.exe`
- `build\Debug\winaudio_probe.exe`

## 运行

启动 GUI：

```powershell
.\build\Debug\winaudio.exe
```

查看 CLI 帮助：

```powershell
.\build\Debug\winaudio_probe.exe --help
```

### 常用 CLI 示例

列出设备：

```powershell
.\build\Debug\winaudio_probe.exe devices
```

列出 loopback 采集视角设备：

```powershell
.\build\Debug\winaudio_probe.exe devices --source=loopback
```

以原生设备名输出：

```powershell
.\build\Debug\winaudio_probe.exe devices --device-name-format=native
```

执行一次快速探测：

```powershell
.\build\Debug\winaudio_probe.exe quick
```

执行 loopback 快速探测：

```powershell
.\build\Debug\winaudio_probe.exe quick --source=loopback --capture-device-id="<loopback-device-id>"
```

关闭监控播放并执行快速探测：

```powershell
.\build\Debug\winaudio_probe.exe quick --monitor=off
```

执行矩阵探测：

```powershell
.\build\Debug\winaudio_probe.exe matrix --matrix-source=loopback
```

收敛矩阵范围：

```powershell
.\build\Debug\winaudio_probe.exe matrix --matrix-source=loopback --matrix-render-backend=wave --matrix-align=on --matrix-delay=0ms
```

### CLI 模式概览

- `quick`：执行单次探测，输出请求格式、协商格式、设备选择、波形状态、失败阶段等信息。
- `matrix`：执行组合探测，输出按来源、后端、profile、delay、buffer、align 等维度汇总的结果。
- `devices`：枚举设备并输出稳定的设备行，适合后续脚本复用设备 id。

### 源模式说明

- `--source=mic`：普通麦克风/采集设备模式。
- `--source=loopback`：系统回环采集模式，loopback capture device id 来自 `devices --source=loopback`。
- `--source=app-loopback`：面向目标进程树的应用级回环采集，需要配合 `--app-loopback-process=<name-or-pid>` 使用。

`Application Loopback` 的真实可用性依赖当前 Windows 版本与运行环境；仓库已有针对其失败语义和提示文本的测试覆盖，但并不意味着所有机器都能直接成功采集。

## GUI 概览

GUI 程序支持以下典型操作：

- 配置 capture/render backend
- 选择 source mode、capture/render device
- 调整采样率、声道数、样本类型
- 配置 WASAPI share mode 和 drive mode
- 打开或关闭 monitor playback
- 启用 follow default devices
- 启用 render auto-align
- 配置 dump 类型与输出路径
- 启动/停止会话
- 运行 `Run Quick Probe`
- 运行 `Run Probe Matrix`

GUI 还会展示：

- 当前会话状态与设备计数
- 配置摘要
- 诊断文本
- 能力说明
- probe 输出
- capture/render waveform

## 测试与验证

### 默认测试

配置并构建后，可以直接运行默认测试集：

```powershell
ctest --test-dir build -C Debug --output-on-failure
```

默认测试集包含：

- `core_pipeline_test`
- `session_controller_test`
- `wave_format_utils_test`
- `app_model_text_test`
- `probe_cli_test`
- `probe_ui_text_test`
- `build_environment_tools_test`
- `convergence_helpers_test`
- `cli_integration_test`

默认测试基线强调轻量、可复现、无真实硬件强依赖。真实设备结果允许随机器、
驱动和系统状态变化，但输出语义、失败阶段与诊断字段必须保持稳定、可审计。

### GitHub Hosted PR 基线

仓库提供一个面向 GitHub Hosted Windows runner 的稳定 PR 检查：

- workflow 名称：`PR Check`
- 触发时机：`pull_request`
- 运行环境：`windows-2022`
- 目标：只验证 hosted-stable 基线，不把真实设备依赖或桌面交互依赖塞进必跑 CI

该 hosted-stable 基线当前覆盖：

- `core_pipeline_test`
- `session_controller_test`
- `wave_format_utils_test`
- `app_model_text_test`
- `probe_cli_test`
- `probe_ui_text_test`
- `build_environment_tools_test`
- `convergence_helpers_test`

本地复现该基线可使用：

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_hosted_stable_ctest.ps1 -Config Release -BuildDir build-ci
```

以下验证层当前不纳入 GitHub Hosted PR 必跑范围：

- `cli_integration_test`
- `gui_smoke_test`
- `hardware_validation_test`

### GitHub Hosted Build 产物

仓库还提供一个独立的手动构建 workflow，用来主动触发编译产物生成：

- workflow 名称：`Build`
- 触发时机：`workflow_dispatch`
- 运行环境：`windows-2022`
- 目标：手动生成可下载的 Windows 编译产物，不创建正式 GitHub Release

该 workflow 会：

- 配置并构建 `Release`
- 运行与 PR 相同的 hosted-stable 基线
- 打包并上传以下 artifact：
  - `WinAudio-build-<run_number>-windows-x64.zip`
  - `WinAudio-build-<run_number>-windows-x64-symbols.zip`

其主包与符号包内容与 `Release` workflow 保持一致：

- 主包：`winaudio.exe`、`winaudio_probe.exe`、`README.md`
- 符号包：`winaudio.pdb`、`winaudio_probe.pdb`

### CLI 集成验证

```powershell
powershell -ExecutionPolicy Bypass -File tools\test_cli_integration.ps1 -Config Debug -BuildDir build
```

或通过已注册的 `CTest` 项运行：

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_cli_integration_ctest.ps1 -Config Debug -BuildDir build
```

CLI 集成验证会覆盖设备发现、显式 device id 复用、loopback 设备路径、monitor off 语义、失败恢复提示和部分矩阵筛选能力。
它依赖机器上存在可枚举的音频设备与 loopback 路径，因此仍视为本地或特定环境验证层，而不是 GitHub Hosted PR 基线。

### GUI Smoke 验证

直接运行脚本：

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_gui_smoke.ps1 -Config Debug -BuildDir build
```

如果希望以正式 `CTest` 条目注册 GUI smoke：

```powershell
cmake -S . -B build-gui -DWINAUDIO_ENABLE_GUI_SMOKE_TESTS=ON
cmake --build build-gui --config Debug
ctest --test-dir build-gui -C Debug -R gui_smoke_test --output-on-failure
```

或使用辅助脚本：

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_gui_smoke_ctest.ps1 -Config Debug -BuildDir build-gui
```

GUI smoke 是真实桌面交互验证，当前不纳入 GitHub Hosted PR 必跑范围。

### 硬件验证

真实硬件相关验证默认不纳入轻量基线，需要在具备可用音频设备的环境中运行：

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_hardware_validation.ps1 -Config Debug -BuildDir build
```

如需注册到 `CTest`：

```powershell
cmake -S . -B build-hwtest -DWINAUDIO_ENABLE_HARDWARE_VALIDATION_TESTS=ON
cmake --build build-hwtest --config Debug
ctest --test-dir build-hwtest -C Debug -R hardware_validation_test --output-on-failure
```

或使用辅助脚本：

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_hardware_validation_ctest.ps1 -Config Debug -BuildDir build-hwtest
```

硬件验证依赖真实设备环境，当前不纳入 GitHub Hosted PR 必跑范围。

### Release 发布

仓库提供一个正式发布 workflow：

- workflow 名称：`Release`
- 触发时机：推送匹配 `v*` 的 tag，例如 `v0.1.0`
- 运行环境：`windows-2022`
- 发布前会重新执行一遍与 PR 相同的 hosted-stable 基线

发布成功后会创建正式 GitHub Release，并上传以下资产：

- `WinAudio-<tag>-windows-x64.zip`
- `WinAudio-<tag>-windows-x64-symbols.zip`

主包包含：

- `winaudio.exe`
- `winaudio_probe.exe`
- `README.md`

符号包包含：

- `winaudio.pdb`
- `winaudio_probe.pdb`

如果任一发布必需文件缺失，release workflow 会直接失败，不会创建不完整资产。

### 一站式收敛检查

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_convergence_check.ps1 -Config Debug -BuildDir build
```

该入口会串联执行构建、CLI 帮助、设备检查、quick probe、matrix probe、CLI integration、GUI smoke 和 `CTest` 基线。

PowerShell 脚本是本项目的一等验证入口。新增构建或验证入口时，应优先对齐
`CMake`、`CTest` 与 `tools/*.ps1`，并明确其适用场景、默认基线归属与失败排查路径。

## 已知边界与注意事项

- 真实音频设备协商结果会随机器、驱动、默认设备、系统设置而变化，测试更强调语义稳定性，而非所有环境输出完全一致。
- GUI 已经具备 smoke 自动化和手工核对清单，但并不是完整的全量 UI 自动化覆盖。
- `Application Loopback` 属于环境相关能力，某些系统上可能只能验证错误提示、诊断文本和失败语义。
- `monitor=off` 时，render pipeline 会被禁用，`--render-device-id` 会被忽略。
- loopback 模式下，capture device id 应来自 `devices --source=loopback` 的输出，而不是普通麦克风设备列表。
- 仓库中的脚本与测试大量依赖 PowerShell 和 Windows 桌面交互环境，建议在本地桌面会话中执行 GUI 相关验证。

## 参考文档

- `docs\convergence-audit-2026-05-29.md`
- `docs\gui-manual-verification-checklist-2026-05-29.md`
- `docs\convergence-must-fix-2026-05-29.md`
- `docs\convergence-backlog-2026-05-29.md`

## 当前状态摘要

从当前仓库内容看，项目已经具备：

- GUI 与 CLI 双入口
- 默认可运行的 CMake/CTest 基线
- 脚本化 CLI 集成验证
- 可选 GUI smoke 正式测试项
- 可选真实硬件验证测试项
- 较完整的收敛审计和人工核对资料

如果你是第一次接手这个仓库，推荐的最短上手路径是：

1. `cmake -S . -B build`
2. `cmake --build build --config Debug`
3. `ctest --test-dir build -C Debug --output-on-failure`
4. 运行 `.\build\Debug\winaudio_probe.exe --help`
5. 运行 `.\build\Debug\winaudio.exe`
