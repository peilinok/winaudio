# #234 性能、真实音频与 GUI 验收

所有修改、构建、测试和证据均在原目录 `E:\WORK\vibecoding\winaudio` 执行；固定源码导出在该目录的 `.scratch/wav-dump-234/` 下，不创建 worktree。

## 固定版本与方法

- 前测为 #233 合并后的主线 `9360b232513b22a41014fec19eb0c536c9e3689f`。
- 后测为 `649015e15576d570aa06f1b5636dffe2c053010f`；生产代码与四配置构建、GUI 冒烟使用的 `39d0047981596e7abb03b921f4e6354316f6e092` 相同。已核对两者的 `src/`、`scripts/` 和 `CMakeLists.txt` 无差异。
- 两版 spdlog 均固定 `6fa36017cfd5731d617e1a934f0e5ea9c4445b13`，Release / x64，Visual Studio 18 2026。本地工具链与 CI 的 windows-2022 / VS2022 分别验证。
- [构建与二进制溯源](artifacts/probe-provenance.json) 记录固定源码、探针、驱动脚本、exe 和构建日志 SHA256。builder 自动运行的 `result.csv` 仅作预检，不进入正式配对数据。
- 正式 sink 配对在 GUI 和构建结束后按 before1 → after1 → before2 → after2 → before3 → after3 逐个运行。正式图表测量另行逐版运行；不与编译、设备冒烟或其他音频任务并行。

`WavSinkProbe` 使用两版实际 WinAudioCore 与相同探针，48000 Hz / 双声道 Float32，每块 480 帧 / 3840 字节，每 10 ms 一次、每路径 1000 次调用。保留 256 KiB writer 批量与 stdio 缓冲、5 ms 轮询、原 ring 容量。每轮正常路径停止后由 WavReader 逐块逐字节核对格式、内容与 EOF；关闭路径必须接收零字节。

push 的 steady_clock 计时包含计时开销与抢占，按每轮 nearest-rank 给出 p50 / p95 / p99，不合并多轮分位数。固定供给下的接收吞吐不是磁盘极限；`offered_bytes_per_timed_push_s` 也不是磁盘吞吐。控制线程 stop 单独计时，不承诺 Windows 硬实时或磁盘无关上限。

复现固定快照，输出目录必须不存在：

```powershell
& scripts/performance/build-wav-sink-probe.ps1 -Revision 9360b232513b22a41014fec19eb0c536c9e3689f -OutputDirectory .scratch/wav-dump-234/repro-before
& scripts/performance/build-wav-sink-probe.ps1 -Revision 649015e15576d570aa06f1b5636dffe2c053010f -OutputDirectory .scratch/wav-dump-234/repro-after -Monitor
# 分别运行两个 build/bin/Release/WavSinkProbe.exe，使用六个独立 WAV 路径。
# 图表工具需另外构建：cmake --build <snapshot>/build --config Release --target WinAudioPerformanceGui -j 4
```

## 实际 GUI 慢关闭、播放切换与重启

使用原目录 `build/bin/Release/WinAudioPerformanceGui.exe` 的 `monitor-dump` 场景。它运行同一个生产 AppUi；测试专用输出 adapter 包装真实 WavWriter，仅在 Render 文件 writer 的 close 前等待有界的命名事件。正常 WinAudioGui 不包含该 fixture。

本轮环境为 `WINAUDIO_PERF_MODE=monitor-dump`、`WINAUDIO_PERF_DURATION=900`、`WINAUDIO_PERF_WARMUP=0`、`WINAUDIO_PERF_MONITOR_CLOSE_EVENT=Local\WinAudioWav234Close20261027`；实际操作提前正常退出。通过 Computer Use 操作真实按钮、目录对话框和语言选择。

Capture 选择 `CABLE Output (VB-Audio Virtual Cable)`，端点 `{0.0.1.00000000}.{bd3a5c06-b951-4df1-924f-8d7e6244a38a}`；Render 选择 `CABLE In 16ch (VB-Audio Virtual Cable)`，端点 `{0.0.0.00000000}.{38a33ff5-6929-4608-8859-e17e9732f9ad}`。应用侧两流均为 192000 Hz / 2ch Float32，没有打开物理麦克风或扬声器，没有修改系统默认设备、音量或增强。

1. 两侧开始独立 Dump，均显示 Recording / 正在接收。[两侧接收截图](artifacts/gui/01-both-recording.png)。
2. 11:05:50.079 关闭同步播放，11:05:50.092 Render writer 已进入 close 等待。界面同时显示 `ren=Idle`、Capture Recording 和 Render Finalizing；同侧新 Dump 按钮禁用，尚未揭示文件。[收尾截图](artifacts/gui/02-render-finalizing.png)。
3. close 保持阻塞，Capture Scope 帧数由 23134080 增至 31541760，Render Scope 停在 23001600；Capture Dump 仍接收。[Capture 继续推进](artifacts/gui/03-capture-advancing.png)。逐路径读取文件属性，Capture 文件长度由 223346744 增至 319029304 字节，见 [前一次](artifacts/gui/04-file-sizes-by-path.json) / [释放时](artifacts/gui/05-file-sizes-at-release.json)。目录枚举缓存的长度不作为写入进度证据。
4. 11:06:48.422 重新 engage，设备 Running，旧 Render 文件仍 Finalizing，新 Dump 仍禁用；没有自动重开。[重新启用期间仍收尾](artifacts/gui/04-reengaged-while-finalizing.png)。中文实时显示“正在收尾”，Capture 为“正在接收”。[中文截图](artifacts/gui/05-chinese-finalizing.png)。
5. 11:08:05.398 释放事件，实际等待 135.306 秒，writer 才真正关闭旧文件；GUI 随后显示“文件已完成”，日志消费 generation=1 一次。[释放后的成功状态](artifacts/gui/06-success-after-release.png)。
6. 显式开始新的 Render Dump，使用新文件名与 generation=2，Capture 文件保持原轮次。[显式新文件](artifacts/gui/07-new-render-generation.png)。
7. 11:09:43.349 点击 Monitor 停止，新 Render close 仍受事件阻塞；11:09:57.354 释放后 stop 才返回，两侧均显示“文件已完成”、Stream Idle。[停止后两侧成功](artifacts/gui/08-monitor-stopped-both-success.png)。这验证保留控制线程同步 stop 语义；播放开关的异步请求与其分别验证。
8. 正常退出，无 PerformanceGui 残留；DumpFolder 与 exe 目录的 `winaudio.ui.ini` 均恢复原值并读回校验。[偏好恢复收据](artifacts/gui/preferences-restored.json)。

[运行日志](artifacts/gui/runtime.log) 与截图给出上述事件顺序。三份 GUI WAV 均为静音，证明 GUI 接线、持续帧传输与收尾行为；非零 PCM 的真实声流验证另由设备探针完成，不用截图替代。

三份文件逐一检查 RIFF 总大小、IEEE Float32 格式、完整帧、data 长度和 EOF，均通过；[读回与 SHA256](artifacts/gui/wav-readback.json)、[gzip 原件清单](artifacts/gui/wav-archives.json)、[GUI 二进制溯源](artifacts/gui/binary-provenance.json)。Capture 为 305.73 秒 / 58700160 帧，两个 Render 分别为 13.35 秒 / 2563200 帧与 36.98 秒 / 7100160 帧。旧 Render 在重新 engage 及继续采集期间没有续写新来源；Capture 文件持续至 Monitor stop。

GUI 未注入 open/write 故障。取消、首错、失败重试、慢 write、不同格式换代和迟到旧请求由实际 sink 与 Monitor fake backend 的确定性红绿测试验证，见 [执行记录](execution.md) 和 [审查记录](review.md)。

## 正式 sink 配对结果

六轮正常路径均接收 3840000 字节，关闭路径均接收零字节；生产 WavReader 逐块逐字节读回与独立解析均通过，六份 WAV 的原始 SHA256 相同。旧版正常 p50 为 1.1–1.2 µs、p95 为 1.9–2.0 µs、p99 为 13.5 / 22.6 / 25.1 µs；新版依次为 p50 1.1–1.2 µs、p95 1.9–2.0 µs、p99 26.0 / 29.6 / 11.3 µs。

两版正常接收吞吐分别约 383907–384220 byte/s 与 383873–384143 byte/s。关闭路径两版 p50=0.1 µs、p95=0.2 µs；旧 p99=0.4 / 0.4 / 0.3 µs，新为 0.5 / 0.4 / 0.4 µs。新版前两轮正常 p99 较高，第三轮较低；保留波动，不宣称加速。控制方 stop 约 0.58–16.53 ms，为本组观察而非时限。

逐轮表见[执行记录](execution.md)，[正式 CSV 与六份压缩 WAV 溯源](artifacts/sink-provenance.json)可独立复核。没有用 builder 预检替代六轮正式结果，也没有把多轮分位数取平均。

## 真实 WASAPI 非零 PCM 与来源换代

使用 [wav-monitor-device-smoke.cpp](../../../scripts/performance/wav-monitor-device-smoke.cpp) 与后测实际 WinAudioCore。源为 CABLE Input `{0.0.0.00000000}.{09793ccf-e246-4dbf-800b-a7cf44856994}`，System Loopback 实际格式 48000 Hz / 8ch Float32；目的为 Dubbing Speaker `{0.0.0.00000000}.{07eb2780-45a9-42b2-aef9-6604815fa189}`。目的端 mix 是 48000 Hz / 2ch，Shared 应用流请求并成功使用 48000 Hz / 8ch Float32；验证的是应用流实际格式。

```powershell
New-Item -ItemType Directory .scratch/wav-dump-234/repro-device
& .scratch/wav-dump-234/performance-after/build/bin/Release/WavMonitorDeviceSmoke.exe `
  '{0.0.0.00000000}.{09793ccf-e246-4dbf-800b-a7cf44856994}' `
  '{0.0.0.00000000}.{07eb2780-45a9-42b2-aef9-6604815fa189}' `
  "$PWD/.scratch/wav-dump-234/repro-device"
```

探针正常退出 0，七份文件均为 Success，均含非零且有限的 PCM；生产 WavReader 检查实际格式、channel mask、完整帧与 EOF，独立 RIFF 解析再次检查大小和 0x63F 掩码。覆盖两侧独立 Dump、关闭播放后 Capture 继续、Render 独立完成、重新 engage 不自动开文件、显式新文件、Monitor stop 和下一 session 来源换代。Render 文件代次为 1→2→3→4，来源为 1→2→2→3；Capture 文件代次为 1→2→3，来源为 1→1→2，两种身份分别推进。

本轮关闭播放请求观测 100 ns，Render Idle 观测 15.42 ms，停播期间 Capture Scope 继续 9600 帧。这些是单轮观察，不是上限；慢文件下的非阻塞保证由确定性交错测试和上述 GUI 分别证明。

首次以 CABLE In 16ch 为目的端时，render `Initialize(shared,requested)` 返回 `AUDCLNT_E_DEVICE_IN_USE`（0x8889000A），探针未满足 render PCM 前置条件、退出 8，未开始 Dump。只更换为已枚举的独立虚拟目的端后通过，没有修改生产代码、设备格式、默认设备、音量或增强设置。首次失败日志完整保留；HRESULT 本身不足以确定持有者或驱动冲突根因。[设备运行输出](artifacts/device/stdout.txt)、[成功运行日志](artifacts/device/runtime.txt)、[首次失败日志](artifacts/device/failed-cable-runtime.txt)、[七份 WAV 及哈希](artifacts/device/device-provenance.json)。

上述为真实 Windows 音频 API 与虚拟端点验证，不代表物理麦克风或扬声器发声，也不宣称 Render Dump 中的每个字节已被设备实际播放。

## 图表主线程预算

两版分别运行既有 `run-charts.ps1 -Plan smoke`，两条 Render Track、每条 8ch，显式使用同一 CABLE Input 虚拟端点；3 秒 warmup、40 秒稳定场景，每阶段约 10 秒。两次均由 Computer Use 观察实际可见窗口并置前，其他本任务构建、设备测试与归档已结束。后测在波形阶段即确认可见，见[截图](artifacts/gui/09-charts-after-visible.png)。左侧新建 Track 控件的默认设备预览不代表已由 fixture 创建的 Track；实际 open 端点见运行日志和 config。

| 稳定场景 | 旧 / 新帧数 | 旧 p95 / ms | 旧 p99 / ms | 新 p95 / ms | 新 p99 / ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| track1-wave | 600 / 599 | 6.0037 | 6.8186 | 5.9603 | 6.8096 |
| track1-spec | 598 / 599 | 4.5136 | 4.9268 | 4.4186 | 5.0962 |
| track2-wave | 599 / 598 | 5.9408 | 6.4643 | 5.9056 | 6.6009 |
| track2-spec | 598 / 599 | 4.4156 | 4.8713 | 4.5842 | 5.4172 |

前后四个稳定场景均满足 p95≤8 ms、p99≤16 ms，两轮原始帧记录均完整。warmup、transition、teardown 分开保留，不混入稳定分位数；旧版最初 Monitor warmup 一帧为 98.5897 ms，也保留在原始记录中。main_thread_ms 沿用既有 FrameProbe 定义，不把 Present 等待或帧间隔混作主线程工作耗时。这是直接场景冒烟，不替代历史完整矩阵，不宣称整个应用加速。

[旧版汇总](artifacts/charts/before/summary.json)、[新版汇总](artifacts/charts/after/summary.json)、[固定源码与原始帧 SHA256](artifacts/charts/provenance.json)和各目录的 `frames.txt.gz`、config、process.csv、运行日志均已归档。config 的 source 字段是驱动 checkout HEAD，二进制源码以 provenance 的 source_pin 为准。归档原始帧解压后与原件逐字节相同。

可读日志副本仅去除行尾空白、统一为 UTF-8；对应 `.log.raw.gz` 保留原始字节，原件与可读副本哈希见各自 provenance。全部本票后测均重新执行；#233 的历史数字没有作为本票通过依据。最终发布前再次核对生产源码与上述固定后测版本一致。
