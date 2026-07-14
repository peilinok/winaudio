# WinAudio

[![CI](https://github.com/peilinok/winaudio/actions/workflows/ci.yml/badge.svg)](https://github.com/peilinok/winaudio/actions/workflows/ci.yml)

小巧的 Windows 音频测试工具：对系统音频设备做采集、播放、回环（loopback）与延迟监听测试，带实时波形 / 声谱图可视化。GUI（Dear ImGui + DX11）为首选交互方式，另有轻量 CLI。

## 功能

- **WASAPI 双后端**：Shared（共享模式）与 Exclusive（独占模式、低延迟），同一流程可切换后端做对比测试。
- **采集 / 播放**：采集落盘 WAV，播放标准 RIFF WAV；可用 `--format` 指定目标格式（Shared 经 WASAPI 引擎 `AUTOCONVERTPCM` 转换；Exclusive 要求硬件原生支持）。
- **系统 loopback**：以 render endpoint 为采集源回采系统输出，可选静音 render keepalive 保持时钟推进。
- **Application Loopback**：按 PID 回采指定进程及其子进程树的音频（Shared-only；需要 Windows 10 build 20348 及以上）。
- **双流延迟监听**：capture → 固定延迟 FIFO（漂移补偿）→ render 实时直通，显示 fifo 水位 / drift / xrun。
- **设备能力查询**：Mix / Device / OEM 三来源格式 + 候选格式在 Shared/Exclusive 下的支持矩阵。
- **格式探测**：检测设备是否支持指定格式（shared 反映 WASAPI 混音器转换能力，exclusive 反映硬件独占可用性）。
- **实时分析**：dB 时域波形 + 滚动 log 频率声谱图（自带 radix-2 FFT，Hann 窗，dBFS 标定）。
- **详细接口调用日志**：所有 WASAPI/COM/Win32 调用的参数与返回值可观测（基于 spdlog，级别运行时可调）。

## 下载与运行

- 从 [Releases](https://github.com/peilinok/winaudio/releases) 下载 `WinAudio-<版本>-win-x64.zip`，解压即用。
- exe 使用静态 CRT，自包含，**无需安装 VC++ Redistributable**。
- 系统要求：Windows 10/11 x64；GUI 需要 Direct3D 11；Application Loopback 功能需要 Windows 10 build 20348 及以上（Windows 11 / Windows Server 2022+）。

## 从源码构建

依赖：Visual Studio 2022（v143 工具集 + Windows SDK）、CMake ≥ 3.21（VS 自带的即可）。

```powershell
git clone --recursive https://github.com/peilinok/winaudio.git   # spdlog 为 git submodule
cd winaudio

.\build.bat Release        # 默认 Release；可传 Debug；--clean 先清后建
.\test.bat Release         # 运行测试套件（ctest / gtest）
```

产物在 `build\bin\<Config>\`：`WinAudioGui.exe`、`WinAudioCli.exe`、`WinAudioTests.exe`。

- 若 `cmake` / `ctest` 不在 PATH，可用 VS 自带的（`<VS 安装目录>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`）。
- 也可直接用 CMake preset：`cmake --preset vs2022`，然后 `cmake --build build --config Release -j`。
- Windows SDK 低于 10.0.20348 时仍可构建，但 Application Loopback 功能会被编译为不可用（运行时报错提示）。

## GUI 使用（首选）

运行 `WinAudioGui.exe`，包含 **Monitor** / **Loopback** / **Application Loopback** 三个 tab。日志默认写运行目录下的 `winaudio.log`（双击启动即 exe 目录），左栏日志面板可运行时切换级别、清空、自动滚动。

### Monitor（双流延迟监听）

两列布局：左列为设备 / 控制 / 状态 / 日志，右列为采集、播放两路各一个「波形 + 声谱图」合并单元（共享时间轴，中间分割线可上下调节，单元可拖拽重排）。

- **Devices**：选择采集设备（默认系统默认设备）与后端（Shared / Exclusive）；「Capture caps…」弹窗显示该设备的三来源格式与 Shared/Exclusive 能力矩阵。
- **Format**：显示当前将用格式（如 `48000 Hz, 16-bit, 2-ch`）；候选下拉第一项为 System default，随后是当前后端支持的候选格式，末项 Custom 可手动输入；切换设备或后端时自动重算默认值。
- **Control**：「Options」弹窗配置高级流参数——capture/render 各自的 `SetClientProperties` 总开关、category、offload、stream option（None / Raw / MatchFormat / Ambisonics / Post-volume loopback）、ducking（仅 render）、buffer ms；弹窗仅 Start 前可改。「同步播放」checkbox 实时启停 render 直通流（Exclusive 模式要求渲染设备支持采集格式）。
- **Status**：实时显示 fifo ms / drift，方便观察跨设备时钟漂移。

### Loopback（系统回采）

选择一个 render device 作为回采源，Start 后实时显示系统输出的波形 + 声谱图。「Silent render keepalive」默认开启（用静音 render 保持 loopback 时钟推进），运行中不可改。

### Application Loopback（按进程回采）

打开 tab 后自动枚举当前有 Audio Session 的进程（进程名 + PID，按进程名排序），「Refresh」重新枚举；点击列表行会把 PID 填入下方输入框，也可手动输入任意非零 PID（不要求出现在列表中）。Start 后以 WASAPI process loopback（Shared）采集该进程及其子进程的音频。

## CLI 使用

`WinAudioCli.exe <子命令> [参数]`，子命令：`list` / `caps` / `probe` / `capture` / `play` / `monitor`。

### list — 枚举设备

```powershell
WinAudioCli list [--render|--capture]
```

默认列出 render 设备。`*` 标记系统默认设备，同时打印混音格式与设备 id（供 `--device` / `--cap` 使用）。

### caps — 设备能力

```powershell
WinAudioCli caps [--device <id>] [--render|--capture]
```

显示 Mix / Device / OEM 三来源格式，以及每个候选格式在 Shared / Exclusive 下的支持情况（`yes` / `-`）。

### probe — 格式探测

```powershell
WinAudioCli probe --format 48000/16/2 [--device <id>] [--render|--capture] [--backend wasapi-shared|wasapi-exclusive]
```

输出 `SUPPORTED` / `NOT SUPPORTED`（退出码 0 / 1）。exclusive 精确反映硬件独占可用性；shared 反映 WASAPI 混音器能否转换该格式（即本工具实际可用性）。

### capture — 采集

```powershell
WinAudioCli capture --out <file.wav> [--seconds N] [--device <id>] [--loopback] [--no-silent-render]
                    [--backend wasapi-shared|wasapi-exclusive] [--format 48000/16/2]
```

- 默认采集 5 秒，实时打印 L/R 电平与 overrun/underrun 计数。
- `--format` 两个后端均可用：Shared 经 WASAPI 引擎转换（无本工具重采样）；Exclusive 要求硬件原生支持。
- `--loopback` 回采系统输出：此时 `--device` 填 render 设备 id；仅支持 Shared；默认启动静音 render keepalive，`--no-silent-render` 关闭。

### play — 播放

```powershell
WinAudioCli play --in <file.wav> [--device <id>] [--backend wasapi-shared|wasapi-exclusive]
```

格式从 WAV 文件头读取，不接受 `--format`。Shared 下由 WASAPI 引擎转换到设备混音格式；Exclusive 要求设备原生支持 WAV 的格式。

### monitor — 双流延迟监听

```powershell
WinAudioCli monitor [--cap <id>] [--render <id>] [--delay-ms N] [--seconds N] [--loopback]
                    [--no-silent-render] [--format 48000/16/2] [--backend wasapi-shared|wasapi-exclusive]
```

- capture → 固定延迟（默认 100 ms）→ render 直通，默认运行 5 秒；状态行实时打印 cap/ren 状态、采样率、fifo ms、drift、xrun。
- `--format` 仅作用于采集端。
- Shared：WASAPI 引擎在渲染侧桥接采样率，采集/渲染可用不同采样率的设备；Exclusive：渲染设备必须支持采集格式，否则 render 启动失败。
- `--loopback` 以 render endpoint 为采集源（`--cap` 填 render 设备 id），仅支持 Shared；注意不要把同一个 render 设备既作 loopback 输入又作监听输出。

### 通用参数

- `--log-level <trace|debug|info|warn|err>`（默认 `info`）与 `--log-file <path>`：日志走 stderr，状态行走 stdout（互不干扰，方便重定向）；`--log-file` 额外写入文件。
- `--format` 语法为 `采样率/位深/声道[f]`，如 `48000/16/2`、`48000/32/2f`（`f` 表示 float）。
- 设备 id 通过 `list` 查询。

## 日志

- 级别：`Info`（默认，流生命周期：open/close/start/stop、选中设备、最终格式）；`Debug`（所有控制路径调用的参数 + 返回值，HRESULT 译成符号名，如 `AUDCLNT_E_DEVICE_INVALIDATED`）；`Trace`（音频热路径逐帧，诊断用，开启时可能对被测音频有轻微扰动）。
- GUI：左栏日志面板（运行时切级别）+ 运行目录 `winaudio.log`（轮转文件）。
- CLI：stderr 输出 + 可选 `--log-file`。

## 已知限制

- **无重采样器**：Shared 依赖 WASAPI 引擎转换格式；Exclusive 要求硬件原生支持；monitor 的 Exclusive 模式要求渲染设备支持采集格式（不匹配则 render 启动失败）。
- **漂移补偿为单帧丢/插 + crossfade**：跨时钟域设备组合（如 USB 麦克风 + HDMI 输出）漂移率高；同接口 loopback 漂移很小。
- 波形/频谱分析为单声道降混（多声道取首声道或均值）。
- 系统 loopback 与 Application Loopback 仅支持 Shared 后端。
- waveIn / waveOut（MME 后端）未实现。

## 开发

- 开发者指南（构建细节、架构、编码约束）见 [CLAUDE.md](CLAUDE.md)；历次功能的设计文档在 `docs/superpowers/specs/`，实施计划在 `docs/superpowers/plans/`。
- CI：push / PR 到 `main` 自动以 Debug + Release 双配置构建并跑测试；推送 `v*` tag 自动发布 Release；`Release` workflow 也支持在 GitHub Actions 页面手动触发，手动运行会构建 Release、跑测试并上传 zip artifact，不创建正式 GitHub Release。
