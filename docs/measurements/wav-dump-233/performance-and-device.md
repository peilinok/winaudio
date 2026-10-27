# 性能与真实音频接口验证

## 固定版本与条件

2026-10-24 至 25 日，Windows 11 专业版 10.0.26200，Intel Core i7-10875H（8 核 / 16 逻辑处理器），NVIDIA RTX 2060，MSVC / Visual Studio 18 2026，Release / x64。本机未安装 VS2022；不修改仓库预设或 windows-2022 CI。

- 旧源码：`3c433575f0bfc77808eaa09113ba1eb2052ffb4e`。
- 新业务源码：`abf5affa4e4453efb0d7ad66b4cc2e991e432588`；后续仅补测试、探针和文档，最终提交前核对 `git diff abf5aff..HEAD -- src/core` 为空。
- 两版依赖均为 spdlog `6fa36017cfd5731d617e1a934f0e5ea9c4445b13`。
- `build-wav-sink-probe.ps1` 从固定提交导出实际 WinAudioCore，附加相同探针；没有复制简化 sink 或修改正式生产 target。
- 早期存在并行编译的探针与锁屏/多开 GUI 图表尝试仅作预检，不纳入最终配对结论。

## WavSink 配对测量方法

使用 [wav-sink-probe.cpp](../../../scripts/performance/wav-sink-probe.cpp)。正常与关闭路径均为 48000 Hz / 双声道 Float32，480 帧 / 3840 字节一块，10 ms 周期，1000 次调用。正常路径使用真实文件 writer，256 KiB 批量、256 KiB stdio 缓冲、5 ms 轮询与原有 ring 容量。每轮停止后以 WavReader 按块逐字节读回，核对格式和 EOF；关闭路径必须接收 0 字节。

每次 push 前后用 steady_clock 计时，包含计时开销和线程抢占。`accepted_bytes_per_s` 是固定供给负载下的实际接收吞吐；`offered_bytes_per_timed_push_s` 仅按计时区间折算调用速率，不是磁盘吞吐或最大承载率。分位数为每轮 1000 个样本的 nearest-rank。控制线程 stop 耗时独立记录，不承诺磁盘无关的上限。编译及功能 GUI 结束后，旧/新交替各三轮，逐个运行。

复现固定快照（输出目录必须不存在）：

```powershell
& scripts/performance/build-wav-sink-probe.ps1 -Revision 3c433575f0bfc77808eaa09113ba1eb2052ffb4e -OutputDirectory .scratch/wav-before
& scripts/performance/build-wav-sink-probe.ps1 -Revision abf5affa4e4453efb0d7ad66b4cc2e991e432588 -OutputDirectory .scratch/wav-after
# 再以各 build/bin/Release/WavSinkProbe.exe 为同版独立文件路径运行三轮，前后交替。
```

最终六轮正常路径均接收 3840000 字节且逐块读回一致；关闭路径均接收 0 字节。下表为逐轮分位数，没有把多轮分位数取平均：

| 版本 / 轮次 | 正常 p50 / µs | 正常 p95 / µs | 正常 p99 / µs | 实际接收 / byte/s | stop / ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 旧 / 1 | 1.1 | 1.7 | 2.4 | 383891.950 | 15.5056 |
| 新 / 1 | 1.1 | 1.8 | 21.8 | 384011.693 | 15.5805 |
| 旧 / 2 | 1.1 | 1.7 | 2.7 | 383942.497 | 0.6824 |
| 新 / 2 | 1.1 | 1.8 | 11.3 | 383957.373 | 0.6484 |
| 旧 / 3 | 1.1 | 1.7 | 2.4 | 383959.696 | 16.4094 |
| 新 / 3 | 1.1 | 1.9 | 15.1 | 384059.410 | 0.7148 |

两版关闭路径三轮均为 p50=0.1 µs、p95=0.2 µs，p99 依次为 0.3 / 0.4 / 0.4 µs，实际接收吞吐为 0。计时结果有约 0.1 µs 的量化粒度，不将这些小值当作精确 CPU 指令成本。

新版正常路径的 p99 明显高于这组三轮旧版记录；这是保留准入与退出协议后的实际成本观察，不宣称性能提升，也不把计时差额全部归因于某一原子指令。当前固定供给下没有丢字节或 overflow；该结果不证明更高吞吐上限。stop 的分散同时受到 writer 轮询与 Windows 调度影响。

原始 CSV：[旧1](artifacts/sink-before-1.csv)、[新1](artifacts/sink-after-1.csv)、[旧2](artifacts/sink-before-2.csv)、[新2](artifacts/sink-after-2.csv)、[旧3](artifacts/sink-before-3.csv)、[新3](artifacts/sink-after-3.csv)；[二进制版本与 SHA256](artifacts/sink-provenance.json)。

## 真实 WASAPI 双来源冒烟

使用 [wav-dump-device-smoke.cpp](../../../scripts/performance/wav-dump-device-smoke.cpp)，显式指定 VB-Audio CABLE Input 虚拟播放设备：`{0.0.0.00000000}.{09793ccf-e246-4dbf-800b-a7cf44856994}`。仅向该 endpoint 播放振幅 0.001 的 Float32 信号；Application Loopback 的目标是探针自身进程。没有修改系统默认设备、音量或增强设置。

```powershell
# 使用上节新快照生成的实际设备探针；先创建空输出目录。
& .scratch/wav-after/build/bin/Release/WavDumpDeviceSmoke.exe '{0.0.0.00000000}.{09793ccf-e246-4dbf-800b-a7cf44856994}' "$PWD\.scratch\wav-device"
```

生产 CaptureTrackList 与真实 WASAPI 后测通过：两来源各三轮独立文件；前两轮逐侧停止 Dump 后，该侧 Scope tap、兄弟 Track 的 tap 仍推进；最后两个接收中的文件通过 destroyAll 收尾。6 个文件均含非零 PCM，RIFF 大小、实际格式、完整帧、System 的 8ch channel mask 与 EOF 正确。System Loopback 实际为 48000 Hz / 8ch / Float32；Application Loopback 实际为 44100 Hz / 2ch / PCM16。后者的 GetMixFormat 返回 E_NOTIMPL，沿用既有自动格式回退，启动及录制成功。

证据：[设备运行结果](artifacts/device-after-result.txt)、[运行日志](artifacts/device-after-runtime.txt)、[WAV 清单与 SHA256](artifacts/device-gui-wav-manifest.json)。清单中的 `device-after` 对应六份 gzip WAV，解压后可独立读回。旧版也通过了同场景预检；该预检只证明设备条件可用，并发缺陷由确定性交错红绿测试证明。

这是实际 Windows 音频接口与虚拟 endpoint 验证，不作为物理扬声器发声、硬实时或磁盘持久化保证。

## 实际 GUI 冒烟

用户解锁后，以新业务快照构建的 WinAudioGui 在独立工作目录运行，由 Computer Use 操作实际界面。

1. System Loopback 选择上述 CABLE Input，创建 48000 Hz / 8ch Track，开始 Dump → 显式停止 → 确认 Track 仍运行、图表仍显示 → 再次开始新文件 → 接收时销毁 Track。
2. Application Loopback 选择本次 source-only 探针进程 PID 6276，IncludeTree，实际格式 44100 Hz / 2ch。开始 Dump → 显式停止 → 确认 Track 仍运行 → 重启新文件 → 接收时销毁 Track。
3. 两侧成功停止/销毁均沿用既有文件揭示行为，随后正常退出 GUI。四个文件均校验 RIFF 大小、完整帧与 EOF，并保留 gzip 原件和 SHA256。
4. GUI 修改的 DumpFolder 目录偏好已恢复原值并读回核对；没有变更系统音频配置。原始 checkout 的三份文档 SHA256 与开始时一致。

证据：[GUI 日志](artifacts/gui-after-runtime.txt)、[System 停止后仍运行](artifacts/gui-system-stopped-running.png)、[Application 停止后仍运行](artifacts/gui-application-stopped-running.png)、[Application 销毁完成](artifacts/gui-application-destroyed.png)。source-only 探针为有界 180 秒，正常退出码 0，见 [source 输出](artifacts/gui-source-output.txt) 与 [退出码](artifacts/gui-source-exit.txt)。它结束后的第二次 Application Dump 名称沿用既有 unknown/PID 回退，因此 GUI 记录仅作为操作接线验证；持续有效来源的三轮实际音频保证以上节设备探针为准。

GUI 未注入文件系统故障；可选文件 open/write/close/overflow 隔离和失败不误报成功由 fake backend、输出 seam 与既有 DumpUi 直接测试覆盖，不将截图当作这些失败场景的证明。

## 图表主线程预算

使用既有 `run-charts.ps1 -Plan smoke`，两条 Render Track 各 8ch，显式发送至上述虚拟 endpoint；3 秒 warmup + 40 秒稳定场景（四阶段各约 10 秒）。以实际可见 WinAudioPerformanceGui 运行。warmup、transition、teardown 单独保留，不并入稳定场景分位数。验收预算 p95 ≤ 8 ms、p99 ≤ 16 ms；这是直接场景冒烟，不冒充完整历史矩阵。

```powershell
& scripts/performance/run-charts.ps1 -EndpointId '{0.0.0.00000000}.{09793ccf-e246-4dbf-800b-a7cf44856994}' -OutputDirectory .scratch/charts-before -BuildDirectory .scratch/wav-before/build -Plan smoke
& scripts/performance/run-charts.ps1 -EndpointId '{0.0.0.00000000}.{09793ccf-e246-4dbf-800b-a7cf44856994}' -OutputDirectory .scratch/charts-after -BuildDirectory .scratch/wav-after/build -Plan smoke
```

须先为各快照执行 `cmake --build <snapshot>/build --config Release --target WinAudioPerformanceGui -j 4`。驱动脚本 config.json 的 source 字段记录的是驱动所在 checkout 的 HEAD；实际二进制对应的固定源码另见归档 provenance，不能将驱动 HEAD 当二进制源码版本。

最终旧/新图表测量时均无本任务编译或其他音频测试并行。旧版开始时文件揭示窗口部分遮挡界面，测量中经 Computer Use 置前；新版也在启动后置前。两次均观察到实际更新的多声道图表。稳定场景结果：

| 稳定场景 | 旧 / 新帧数 | 旧 p95 / ms | 旧 p99 / ms | 新 p95 / ms | 新 p99 / ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| track1-wave | 599 / 599 | 6.4637 | 7.1662 | 6.3470 | 7.0311 |
| track1-spec | 598 / 599 | 4.8429 | 5.3343 | 4.6750 | 5.1356 |
| track2-wave | 599 / 598 | 6.7161 | 7.9166 | 6.5197 | 7.1917 |
| track2-spec | 599 / 599 | 4.7189 | 5.2656 | 4.7034 | 5.3698 |

前后四个稳定场景均满足预算。此处的 main_thread_ms 沿用已有探针定义；不把含 Present 等待的帧间隔混作主线程工作耗时，也不据此宣称整个应用加速。

证据：[旧版汇总](artifacts/charts-before-summary.json)、[新版汇总](artifacts/charts-after-summary.json)、[固定源码和原始帧 SHA256](artifacts/charts-provenance.json)、[新版可见图表](artifacts/gui-charts-after.png)。同目录的 `charts-before-frames.txt.gz`、`charts-after-frames.txt.gz` 保留全部原始帧；config、process.csv 和运行日志一并归档，warmup/transition/teardown 可独立复核。

最终核对：`git diff abf5aff..baa0fbc -- src/core` 为空，实际设备、GUI 与性能快照的业务代码与最终实现一致。
