# WinAudio

[English](README.md)

[![CI](https://github.com/peilinok/winaudio/actions/workflows/ci.yml/badge.svg)](https://github.com/peilinok/winaudio/actions/workflows/ci.yml)

小巧的 Windows 音频测试工具：对系统音频设备做采集、播放、回环（loopback）与延迟监听测试，带实时波形 / 声谱图可视化。桌面界面使用 Dear ImGui + DX11。

## 运行截图

![WinAudio Monitor 运行截图](docs/images/winaudio-monitor-running.zh-CN.png)

更多 GUI 模式截图见 [GUI screenshots](docs/gui-screenshots.zh-CN.md)。

## 功能

- **WASAPI 双后端**：Shared（共享模式）与 Exclusive（独占模式、低延迟），同一流程可切换后端做对比测试。
- **采集 / 播放**：Capture Track 支持随时开始或停止 WAV Dump；Render Track 播放声道识别语音。目标格式在 GUI 中选择（Shared 通过 WASAPI `AUTOCONVERTPCM` 转换；Exclusive 要求硬件原生支持）。
- **系统 loopback**：以 render endpoint 为 **Capture source** 回采系统输出，可选静音 render keepalive 保持时钟推进。GUI Loopback 页可同时跑多路 **Capture Track**。
- **Application Loopback**：按 PID 做 WASAPI process loopback（Shared-only；Windows 10 build 20348+）。默认 **IncludeTree**（只采目标进程及其子进程树）；可选 **ExcludeTree**（采 process-loopback 混音中除该进程树以外的部分）。GUI Application Loopback 页同样是多路 Capture Track。
- **双流延迟监听**：**Monitor** 仍是一对：正好一路 Capture Track，加上至多一路 Render Track（capture → 固定延迟 FIFO（漂移补偿）→ render），显示 fifo 水位 / drift / xrun。Monitor 不做多路列表。
- **设备能力查询**：Mix / Device / OEM 三来源格式 + 候选格式在 Shared/Exclusive 下的支持矩阵。
- **格式探测**：检测设备是否支持指定格式（shared 反映 WASAPI 混音器转换能力，exclusive 反映硬件独占可用性）。
- **实时分析**：dB 时域波形 + 滚动 log 频率声谱图（自带 radix-2 FFT，Hann 窗，dBFS 标定）；GUI 采集侧 2ch+ 按实际 channel 分开显示。
- **详细接口调用日志**：所有 WASAPI/COM/Win32 调用的参数与返回值可观测（基于 spdlog，级别运行时可调）。

## 下载与运行

- 从 [Releases](https://github.com/peilinok/winaudio/releases) 下载 `WinAudio-<版本>-win-x64.zip` 或 `WinAudio-<版本>-win-x86.zip`，解压即用。
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

产物在 `build\bin\<Config>\`：`WinAudioGui.exe`、`WinAudioTests.exe`。

- 若 `cmake` / `ctest` 不在 PATH，可用 VS 自带的（`<VS 安装目录>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`）。
- 也可直接用 CMake preset：`cmake --preset vs2022`，然后 `cmake --build build --config Release -j`。
- Windows SDK 低于 10.0.20348 时仍可构建，但 Application Loopback 功能会被编译为不可用（运行时报错提示）。

## GUI 使用

端点音频增强开关只能在开始音频流之前修改。需要管理员权限时，GUI 会弹出 UAC，并启动自身的第二个实例完成这一项操作；取消或失败会显示在原窗口。辅助实例不创建窗口、不启动音频流，执行后立即退出。

运行 `WinAudioGui.exe`，可切换 **Monitor** / **Loopback** / **Application Loopback** / **Render** / **Pipeline**。页签数量表示后台运行的任务、轨道或已附加进程，空闲时不显示数量。顶部的 `mmsys.cpl` 统一打开 Windows 声音控制面板的播放页，`ms-settings` 打开声音设置。日志默认写入工作目录的 `winaudio.log`（双击启动时通常是程序目录）；面板支持调整高度、收起、搜索、跟随最新、清空及复制或保存筛选结果。记录级别控制实际写入的日志，显示级别只筛选面板内容；没有级别标记的界面事件仍保留。

领域用语：**Track** 是一路可单独创建/销毁的单向流（采集或播放），Create 立即开流，没有 idle Track；**Capture source** 是配方（设备 / PID+mode），不是正在跑的那一路；**Capture Track** 绑定一个 Capture source，GUI 上可随时 Dump / Stop dump；**Monitor** 是唯一的双流会话名；**Chart Host** 是一路图的工具栏 + 面板。两个 loopback 页各自一份列表，每次启动为空；切走 tab 不会停采集。

### Monitor（双流延迟监听）

顶部控制 Start / Stop 和同步播放；左侧配置设备、格式和参数，右侧显示图表，底部是日志。配置区宽度可以拖动调整。图表提供双流适屏、纵向详细、仅采集或仅播放布局，可交换两流位置或恢复布局。多声道采集默认显示 `Ch 1`，可选择前 8 个声道之一或全部声道；所有采集声道的历史仍按同一个结束位置更新。Monitor 的播放图表保持单声道降混，采集和播放共用时间轴。

- **Devices**：选择采集设备（默认系统默认设备）与后端（Shared / Exclusive）；「Capture caps…」弹窗显示该设备的三来源格式与 Shared/Exclusive 能力矩阵。
- **Format**：显示当前将用格式（如 `48000 Hz, 16-bit, 2-ch`）；候选下拉第一项为 System default，随后是当前后端支持的候选格式，末项 Custom 可手动输入；切换设备或后端时自动重算默认值。
- **Control**：「Options」弹窗配置高级流参数——capture/render 各自的 `SetClientProperties` 总开关、category、offload、stream option（None / Raw / MatchFormat / Ambisonics / Post-volume loopback）、ducking（仅 render）、buffer ms；弹窗仅 Start 前可改。「同步播放」checkbox 实时启停 render 直通流（Exclusive 模式要求渲染设备支持采集格式）。**Dump capture** / **Dump render** 各自随时开停 WAV sink（选文件夹、自动命名；播放未开时 Dump render 禁用）。
- **Status**：实时显示 fifo ms / drift，方便观察跨设备时钟漂移。
- Monitor 保持单一会话：Start / Stop 控制这一对，没有「Destroy all」跨页清列表。DelayFifo 属于 Monitor，不是 Track。

### Loopback（系统回采）

本页是一份 System Loopback **Capture Track** 列表（Shared-only），不是单路 Start/Stop。

- **Create Track**：选一个 render device 作为 Capture source。Format 下拉为 System default（不传 requested，走 mix）+ 当前 render 设备 Shared 候选 + Custom Apply（`sr/bits/ch[f]`，与 Monitor 相同）。Options 为采集列（Set client properties、category、offload、stream option、buffer；无 Ducking、无 AutoConvert 控件），写入下一次 Create 的 Stream params。Silent render keepalive 默认开启，且始终全默认 mix，不继承本页配方。Create 立即开流，没有第二步 Start。同一 render device 允许同时开多路。本页配方与 Monitor / Application Loopback 互不共用；移除轨道和切 tab 不清配方。
- **Stop and remove** / **Stop and remove all**：停止并移除单条或本页全部轨道，同时结束对应 WAV 录音并保留文件，不影响其他页。
- 右侧先显示轨道摘要，再按“当前轨道”“两轨比较”或“全部展开”显示图表。选择比较对象后可并排查看两条轨道，移除或刷新时会修正选择。图表暂停、声道选择和时间轴归各条轨道独立管理。
- **Dump** / **Stop dump**：每路独立、运行中随时开停的 WAV sink。Start dump 选保存文件夹，文件名按固定 prefix、实际格式和时间戳生成；Stop dump（或移除轨道）成功后用资源管理器选中该文件。多路不会混成一个流或一个文件。

### Application Loopback（按进程回采）

本页独立管理 Application Loopback 采集轨道，支持创建、停止并移除、两轨比较及逐轨 WAV Dump。采集源是 **PID + IncludeTree / ExcludeTree**；可直接输入 PID，也可搜索进程名或 PID 后选择音频会话。“格式参考”仅提供格式候选，不限制实际采集设备；不选参考时可使用自动格式或自定义格式。参数只用于下一次创建，轨道摘要显示实际格式，Dump 文件名包含进程名和 PID。本页操作不影响 Monitor 或系统 Loopback 轨道。

打开 tab 后自动枚举当前有 Audio Session 的进程（进程名 + PID，按进程名排序），「Refresh」重新枚举。Session 列表只做发现：点击一行把 PID 填入输入框，也可手输任意非零 PID（不必出现在列表中）。点列表不会开流。

**PID** 与采集范围选项用于下一次创建，并保存在该轨道的采集源中。默认采集此进程及子进程；另一选项采集除此进程树外的音频。运行中轨道的范围不随选项改变；需要改变范围或再次采集同一 PID 时，创建新轨道。

| 模式 | UI | 该路状态 | 语义（Windows process tree） |
|------|----|----------|------------------------------|
| **IncludeTree**（默认） | 采集此进程及子进程 | `IncludeTree` | 只采集目标 PID 及其**子进程树**的音频 |
| **ExcludeTree** | 采集除此进程树外的音频 | `ExcludeTree` | 采集 process-loopback 混音中**除**该进程树以外的部分 |

### Render（按声道试听）

左侧选择输出端点和声道布局，也可输入 1–8 的自定义声道数。客户端格式固定为 48000 Hz / PCM16。新轨道的全部声道默认暂停；在右侧按声道选择继续循环、暂停、播放一次或调整音量。轨道摘要显示客户端格式、实际格式及流状态；全部声道暂停时播放流仍可能运行，因此页签数量按运行中的流统计。图表支持声道选择、两轨比较和全部展开。

### Pipeline（Live 会话 API 监控）

输入 **PID**，或从左侧进程/会话列表选择目标，再点击 **Attach** 注入同架构的 `WinAudioHook.dll`。可同时附加多个 PID；新增目标不会分离其他目标。目标列表支持进程名/PID 搜索和音频会话筛选；PID 只有一个会话时自动选中管线目标，多设备或多方向时需明确选择会话。管线图和调用记录分别选择目标，查看不同进程时会提示。**Detach PID** 分离输入框中的进程，调用记录旁的“分离此进程”分离当前记录段所属进程，**Detach all** 分离全部目标；历史记录继续保留。

管线图、调用表、参数、API 监控选择及统计可同屏显示，各区域分隔条可拖动，图或调用区域可单独最大化后恢复。调用记录支持模糊搜索、接口和失败结果筛选；API 监控选择控制实际采集的方法，显示窗口开关只影响显示。**保存当前段 / Save current segment** 将当前查看段快照写为 JSONL，目录为 `%LOCALAPPDATA%\WinAudio\pipeline-calls`，不受显示筛选或显示窗口开关限制。管线图继续区分 Initialize 来源（none / ETW / Hooked）、主动探测与观察结果，以及多个 Hooked 流；只合并所选会话所属进程的调用。高频 GetBuffer/ReleaseBuffer 方法默认关闭；不支持跨架构注入、任意 API 或调用栈采集。

性能测量、复跑方法及已知验证限制见 [性能实施与验收记录](docs/measurements/performance-acceptance/README.md)。控制历史仍完整保存在内存，新增记录时内存会增长；本轮未加入长期归档。


## 日志

- 级别：`Info`（默认，流生命周期：open/close/start/stop、选中设备、最终格式）；`Debug`（所有控制路径调用的参数 + 返回值，HRESULT 译成符号名，如 `AUDCLNT_E_DEVICE_INVALIDATED`）；`Trace`（音频热路径逐帧，诊断用，开启时可能对被测音频有轻微扰动）。
- GUI：左栏日志面板（运行时切级别）+ 运行目录 `winaudio.log`（轮转文件）。

## 已知限制

- **无重采样器**：Shared 依赖 WASAPI 引擎转换格式；Exclusive 要求硬件原生支持；monitor 的 Exclusive 模式要求渲染设备支持采集格式（不匹配则 render 启动失败）。
- **漂移补偿为单帧丢/插 + crossfade**：跨时钟域设备组合（如 USB 麦克风 + HDMI 输出）漂移率高；同接口 loopback 漂移很小。
- GUI 采集侧 waveform / spectrogram 对实际 2ch+ 格式按 `Ch N` 分开显示，最多显示前 8 个 channel；播放端可视化与电平指示仍为单声道降混。
- 系统 loopback 与 Application Loopback 仅支持 Shared 后端。
- Application Loopback 的 Exclude 作用于**整个目标进程树**（含子进程），不是任意多 PID 过滤；与 System Loopback tab 路径不同，对比时请按实际听感理解。
- 多路 Capture Track 各自独立：不混成一路音频，也不合成一个 WAV。没有产品侧的并发路数上限，也不把某个数字写成 Windows / WASAPI 限制。
- waveIn / waveOut（MME 后端）未实现。

## 开发

- 开发者指南（构建细节、架构、编码约束）见 [CLAUDE.md](CLAUDE.md)；领域用语见 [CONTEXT.md](CONTEXT.md)；历次功能的设计文档在 `docs/superpowers/specs/`，实施计划在 `docs/superpowers/plans/`。
- CI：push / PR 到 `main` 自动以 Debug + Release 双配置构建并跑测试；推送 `v*` tag 自动发布 Release；`Release` workflow 也支持在 GitHub Actions 页面手动触发，手动运行会构建 Release、跑测试并上传 zip artifact，不创建正式 GitHub Release。
