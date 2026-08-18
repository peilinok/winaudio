# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 这是什么项目

WinAudio：Windows 音频测试工具（WASAPI 采集/播放、系统与应用 loopback、双流延迟监听、波形/声谱图可视化）。C++17 + CMake（MSVC v143；默认 x64，也支持 x86 构建），4 个 target：`WinAudioCore` 静态库（`src/core`）、`WinAudioCli`（`src/cli`）、`WinAudioGui`（ImGui + DX11，`src/gui`，首选前端）、`WinAudioTests`（gtest，`src/tests`）。

用户向文档（功能介绍、下载、CLI/GUI 完整用法、已知限制）见 `README.md`；历次功能的设计文档与实施计划在 `docs/superpowers/specs/`、`docs/superpowers/plans/`，大改某个子系统前先看对应 spec。

## 常用命令

```powershell
git submodule update --init          # 首次 clone 后必须（third_party/spdlog）

.\build.bat Debug                    # 构建（默认 Release；--clean 先删 build\ 再重建）
.\build.bat Release x86              # 构建 x86 到 build-x86\
.\test.bat Debug                     # ctest 全量（默认 Release）
.\build\bin\Debug\WinAudioTests.exe --gtest_filter=MonitorEngine.*   # 单跑一组测试
.\clean.bat

# 等价的原生 CMake 方式：
cmake --preset vs2022
cmake --build build --config Debug -j
cmake --preset vs2022-x86
cmake --build build-x86 --config Release -j
```

- `cmake` / `ctest` 若不在 PATH，用 VS 自带的：`D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`。
- 产物：`build\bin\<Config>\{WinAudioCli,WinAudioGui,WinAudioTests}.exe`、`build\lib\<Config>\WinAudioCore.lib`。
- 手工冒烟：CLI 子命令 `list` / `caps` / `capture` / `play` / `probe` / `monitor`（参数解析与 usage 在 `src/cli/main.cpp`；`list` 不开音频流，最适合快速验证）；GUI 有 Monitor / Loopback / Application Loopback 三个 tab。日志调试：CLI 用 `--log-level trace --log-file <path>`（日志走 stderr，与 stdout 状态行分离）；GUI 默认写 `winaudio.log` 并带左栏日志面板。

## 架构

数据流主线（多数改动落在这条链上）：

```
capture source（endpoint | system loopback | application loopback(PID + ProcessLoopbackMode)）
  ├─ Engine：单流。采集→WAV / WAV→render 播放 / probeFormat 格式探测
  └─ MonitorEngine：双流直通。capture → DelayFifo（固定延迟+漂移补偿）→ render
                    ├─ ScopeBuffer tap ×2（cap 保留实际 channel 快照，ren 为 mono tap）
                    └─ Analysis（采样计数节拍）→ Fft → GUI 波形/Spectrogram
```

- **分层**：前端把 `BackendKind`（`WasapiShared` / `WasapiExclusive`）传给 Engine/MonitorEngine，同一上层流程可切后端做对比测试；流生命周期统一为 open → start → poll → stop → close（`IAudioBackend.h`）。
- **`src/core/WasapiStream.{h,cpp}` 是 WASAPI 心脏**：基类做模式感知格式协商（Shared = `GetMixFormat` + `AUTOCONVERTPCM`；Exclusive = `IsFormatSupported` + `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` 对齐重试）、事件驱动线程模型、同步启动握手；派生出 capture / render / system-loopback / silent-render 各流。按 PID 的 process loopback 在 `ApplicationLoopbackCapture.*`（Shared-only）：`CaptureSource::processLoopbackMode` 为 `IncludeTree`（默认）或 `ExcludeTree`，激活时映射 Win32 `PROCESS_LOOPBACK_MODE_*_TARGET_PROCESS_TREE`；GUI Application Loopback tab 用 Exclude checkbox 选择（运行中不可改）。
- **跨线程原语**（音频线程 ↔ GUI/控制线程的唯一通道）：`RingBuffer`（SPSC + xrun 计数）、`ScopeBuffer`（seqlock 快照，读不阻塞写；capture 可按 channel snapshot，legacy snapshot 仍返回 mono/downmix）、`DelayFifo`（单帧丢/插 + crossfade）、`Analysis`（采样计数节拍触发 FFT）。
- **GUI**：逻辑集中在 `src/gui/AppUi.*`；用户可见文案集中在 `AppUiText.h`（有对应文案测试）；`Spectrogram.*` 纯 STL、可脱离 GUI 单测。ImGui / ImPlot / googletest 是 `third_party/` 内的 vendored 副本；spdlog 是唯一 git submodule。

## 硬性约束（改代码前必读）

- **音频热路径（capture/render 线程回调）禁止堆分配与阻塞锁**；跨线程通信只用上面列的原语或原子量。
- **core「never prints」**：错误经 `Result`（HRESULT 规范化）向上传；日志经 `wa::log`（spdlog 薄封装），sink 由前端注册，core 不选输出目的地。
- **凡新增/改动 WASAPI/COM/Win32 调用，必须按现有模式加 `wa::log` 插桩**：Info = 生命周期（open/close/start/stop、选中设备、最终格式）；Debug = 控制路径参数+返回值（HRESULT 过 `hrName` 译成符号名）；Trace = 音频热路径逐帧（无堆分配路径）；Warn = 被容忍/被忽略的返回值。插桩纯观测，不得改控制流、返回值或时序。
- **core 只依赖 Win32 + STL + spdlog**；GUI 专用第三方库进 `third_party/`，不进 core。
- **构建约定**：静态 CRT（/MT、/MTd）由顶层 `CMAKE_MSVC_RUNTIME_LIBRARY` 统一控制，新 target 不要自行设置 runtime；自有 target 用 `wa_set_project_warnings(<target>)` 挂 `/W4 /permissive- /utf-8 /sdl`，保持零警告。
- **领域边界**：本工具没有重采样器（Shared 靠 WASAPI 引擎转格式，Exclusive 要求硬件原生支持，漂移补偿只有单帧丢/插+crossfade）；系统/应用 loopback 均 Shared-only；GUI 采集侧 waveform/spectrogram 对实际 2ch+ 格式按 `Ch N` 分开显示最多前 8 个 channel，并按同一 capture end index 同步推进；render 可视化与 level 仍为单声道降混；waveIn/waveOut 未实现。
- **新增核心逻辑配套 gtest**（`src/tests/test_*.cpp`，一模块一文件），且必须能脱离真实音频设备运行（CI runner 无音频硬件）。

## 测试与 CI

- CI（`.github/workflows/ci.yml`）：push / PR 到 `main`，Debug + Release 双配置 build + ctest。**runner 必须 pin `windows-2022`**（`windows-latest` 已预装 VS 2026，`Visual Studio 17 2022` generator 找不到工具链）；checkout 必须 `submodules: recursive`。
- 发布/打包：推 `v*` tag → `release.yml` 构建、打包 zip、创建 GitHub Release；也可在 GitHub Actions 页面手动触发 `Release` workflow，手动运行只上传 zip artifact，不创建正式 GitHub Release。
- 涉及真实设备的行为（延迟、漂移、独占格式协商）单测覆盖不到，相关改动合并前需人工冒烟（CLI `monitor` 或 GUI）。

## Commit 与 PR 规范

### Commit（Conventional Commits 1.0.0，英文）

格式：`<type>(<scope>): <subject>`，正文与 footer 可选。

- **type**：`feat` / `fix` / `docs` / `test` / `refactor` / `perf` / `build` / `ci` / `chore` / `revert`。
- **scope**：按改动子系统，本仓库在用：`core`、`gui`、`cli`、`log`、`loopback`、`audio`、`build`；跨子系统或不适用时可省略（如 `docs: …`、`ci: …`）。
- **subject**：祈使句、小写开头、结尾不加句号、≤ 72 字符（尽量 ≤ 50）；说清"这个 commit 做了什么"。
- **body**（改动动机不自明时必写，空行隔开）：写 what/why（动机、取舍），不逐行复述 diff；每行 ≤ 72 字符。
- **footer**：破坏性变更用 `BREAKING CHANGE: <说明>`；关联 issue 用 `Closes #N` / `Refs #N`。
- **原子性**：一个 commit 只做一件事，且每个 commit 都能独立构建、测试通过（保持可 `git bisect`）；格式化/重构不与功能、修复混在同一 commit。
- **分层提交**（本仓库惯例）：一个功能按 `core → cli/gui → docs` 自底向上拆成多个 commit，每层自洽。

### 分支与 PR

- 分支名：`<type>/<kebab-topic>`，如 `feat/system-loopback`、`fix/loopback-silence`；从 `main` 切出，PR 回 `main`。
- **PR 标题**同 Conventional Commits 格式。注意：`release.yml` 开了 `generate_release_notes`，PR 标题会原样进入 Release Notes，要让用户读得懂。
- **PR 描述**必含三部分（开 PR 时自动带出模板 `.github/PULL_REQUEST_TEMPLATE.md`，按模板填写）：
  1. **What / Why**：改了什么、为什么；关联 issue 或 `docs/superpowers/specs/` 里的设计文档。
  2. **How**：关键设计取舍，一两段即可。
  3. **Testing**：`test.bat` Debug + Release 结果；涉及真实设备的改动附人工冒烟结论（CLI `monitor` 或 GUI）；GUI 界面改动附截图。
- 一个 PR 聚焦一个主题，控制体量（数百行级，重构和功能分开提）；未完成的开 Draft PR。
- 合并前提：CI（Debug + Release）全绿。合并方式为 merge commit（保留分支上的分层提交历史），所以分支上每个 commit 都须符合上述规范，不要依赖 squash 兜底。

## 约定

- 回答用中文；代码、注释、日志文本、API 标识符保持英文。
- `AGENTS.md` 只是指向本文件的指针；agent 规则统一维护在这里。

## Agent skills

### Issue tracker

Issues live in GitHub Issues on peilinok/winaudio (`gh` CLI). See `docs/agents/issue-tracker.md`.

### Triage labels

Default five-role vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `CONTEXT.md` at the repo root and `docs/adr/`. See `docs/agents/domain.md`.
