# #234 执行记录

- 工作目录：`E:\WORK\vibecoding\winaudio`；分支：`codex/wav-dump-234`。
- 固定审查基线：`9360b232513b22a41014fec19eb0c536c9e3689f`。
- 前置 #233 已 squash 合并、CLOSED；主线合并后 CI `40437060450` Debug / Release 成功，#234 原生 open blocker=0。
- 全部修改、构建与测试使用原目录；`.archify/`、`_shots/`、`shots/` 保留。
- 测试边界：生产 MonitorEngine 的 BackendFactory 和 WavSinkOutput、实际 WavSink、生产 GUI 接线。文件系统用可控 adapter；不加入生产调度钩子。

## 逐行为红绿

1. 基线 Release：MonitorEngine、WavSink、WavFile、CaptureTrackList、DumpUi 共 103/103 通过（[日志](artifacts/baseline-release.txt)、[XML](artifacts/baseline-release.xml)）。原目录当时没有 Debug 二进制，后续已完整构建。
2. 慢 Render close：`SlowRenderCloseDoesNotBlockPlaybackOffOrCaptureDump` 在只添加输出 factory 注入边界、保持旧停用逻辑时真实失败：render 不能变 Idle，capture 不推进，Capture WAV 只有 20000/40000 字节。将 pump 停用中的同步 `stop` 改为 `requestStop` 后通过，条件变量仍阻塞 close 时 capture、Capture Dump 与 render 停用可继续。证据：[红测](artifacts/red-01-close.txt)、[绿测](artifacts/green-01-close.txt)。
3. 来源先于 Preparing 结束：`SourceEndsBeforePreparing` 在同源专用构建中固定暂停后发布来源无效、请求停止，旧逻辑仍返回启动成功并接受 4 字节。增加打开与 scratch 准备之后、CAS Ready 之前及之后的原子来源检查后，Preparing 前、Ready 前、Ready 后三个固定边界均通过，最终为 Cancelled。证据：[红测](artifacts/red-02-source.txt)、[绿测](artifacts/green-02-source.txt)。
4. 文件状态：`CompletionIsSeparateFromAdmissionAndKeepsItsGeneration` 先证实 close 阻塞时旧 `isRunning` 仍为 true，缺 Finalizing 与来源结果；修复后关闭准入立刻不再接收，close 完成前始终 Finalizing，同一 generation / source / path 完成后才变 Success。证据：[红测](artifacts/red-03-phase.txt)、[绿测](artifacts/green-03-phase.txt)。
5. 来源换代：`ReengageDuringOldCloseUsesANewSourceAndConsistentFormat` 在旧 close 阻塞时将 render 从 2ch PCM16 切为 1ch Float32，先得到来源始终为 1 的真实失败；迁移 Monitor 来源代次和原子格式快照后通过。旧 WAV 的字节与顺序保持原格式，新的 WAV 读回为 1ch Float32。证据：[红测](artifacts/red-04-rollover.txt)、[绿测](artifacts/green-04-rollover.txt)。首轮新增测试缺 `<algorithm>` 的编译错误已修正，不计作行为红测；红测日志是成功编译后的 1 项真实失败。
6. GUI 新边界测试先产生缺 `Completion::take` / `canStart` 实现的链接失败，随后实现生产接线后通过。这是新接口的构建红测，不冒称旧 GUI 的运行反例。测试覆盖收尾禁止新 Dump、新代次出现后旧完成失效、同代成功最多一次、失败 / 取消不揭示，以及全部六种状态的英文和中文文案。证据：[链接红测](artifacts/red-05-gui-link.txt)、[绿测](artifacts/green-05-gui-state.txt)。

补充直接行为覆盖：慢 write 被条件变量阻塞时 capture 可独立完成并逐字节读回；慢 open 中停用、换格式重新 engage 不复活旧来源；延迟的旧 source 停止请求在新代次开始后不能关闭新文件；open / write / close 失败保留首错、Capture Dump 继续且新轮次可重试；Monitor stop 等待已在收尾的文件，返回后两个 sink 都是对应代次成功。阶段验证为 Release [123/123 项直接测试](artifacts/stable-release-tests.txt)和 [18/18 项实际 sink 专用测试](artifacts/stable-lifecycle-tests.txt)，最终四配置回归见下表。

## 协议与生产接线

- `WavSinkStatus` 提供独立 `phase`、`generation`、`source`、路径和消息；Preparing / Accepting / Finalizing 由同一 gate 推导，最终结果只在实际 close 后于元数据锁下发布。保留原 Idle / Running / Error 查询兼容。每轮 metadata 重置与新代次保留在同一锁内，poll 不会把新路径配给旧完成。
- Monitor capture 每 session、render 每成功 engage 换来源代次，不回绕；先把有效来源发布为 0，再 requestStop 对应 sink。两个来源原子和 WavSink gate 都保留 seq_cst。
- render 格式的 sampleRate / channels / bits / isFloat / validBits / channelMask 都是原子。控制方只接收前后相同、非零的来源版本；版本不复用，因此格式字段不能在同一有效版本下被覆盖。普通 `renderFmt_` 仅供 pump 使用。
- WavSink 先保留 Preparing，文件与 scratch 就绪后检查来源；只对同代 Preparing CAS Ready，再复核来源。Preparing 之前丢失的停止请求由来源有效性检查补足。来源结束若在复核之后发生，合法顺序为先启动、随后关闭准入收尾。
- pump 停用 render 与退出只发原子请求；teardown 先 join pump，再等待两个文件和回收。renderScope 仍在 session 期间存活。复用唯一 WavSink，不改 ring、批量、stdio 或轮询参数。
- AppUi 播放开关只提交 `setPlaybackEnabled`。每帧、显式 Dump stop、Monitor stop 以及下一 Dump 启动前，统一按代次消费完成。显式 stop 仍同步；Finalizing 时按钮禁新 Dump；成功才揭示一次，失败与取消只显示各自状态。

## 验收台账

- [x] 各协议修复先记录真实行为红测；GUI 新接口构建红测单独标明。
- [x] 慢 write / close 时 render 可停，capture Scope 与 Capture Dump 继续。
- [x] 正常完成文件字节、顺序、实际格式正确；两侧独立。
- [x] Preparing 前、期间、Ready 附近来源结束不复活。
- [x] 来源换代、格式原子快照、旧请求与结果隔离。
- [x] 取消、I/O 失败、重试、停止和析构回收的确定性测试。
- [x] GUI 状态及生产接线已实施，单测验证禁新 Dump 与按代次只消费成功一次。
- [x] Monitor stop 同步完成两个 sink；播放切换仅发意图。
- [x] 保留采样位置、格式、单侧设备错误与文件失败隔离。
- [x] 英文及中文文案测试；实际 GUI 冒烟仍由总控后验。
- [x] 直接回归、生产热路径核对、四配置构建与锁自由。
- [x] 正式 sink 配对性能和真实虚拟 WASAPI 设备冒烟，原始结果、WAV 与失败尝试均归档。
- [x] 总控实际 GUI 慢 close、播放切换、重启与两语言状态验证通过；图表前后四场景均满足 p95≤8 ms / p99≤16 ms，独立证据见总记录。
- [x] Standards / Spec 两轴审查及修复，最终两轴均为 0 项，见[审查记录](review.md)。

本地只运行目标和直接相关测试；CI 保持既有完整检查。真实设备、GUI 与性能不借用 #233 的旧结果。

## 四配置构建与直接回归

验证提交为 `39d0047981596e7abb03b921f4e6354316f6e092`。本机使用 `Visual Studio 18 2026`；x64 沿用 `build`，x86 新建 `build-x86`（`Win32`）。每个配置均构建 `ALL_BUILD` 和额外的 `WinAudioPerformanceGui`，未使用 `--clean-first`，不称全仓清理重建。

| 配置 | 构建状态 | 直接 CTest | 证据 |
| --- | --- | --- | --- |
| x64 Debug | 既有 cache 中本配置首次完整编译，随后增量复核 | 223/223，16.13 秒 | [首次构建](artifacts/build-x64-debug-initial.txt)、[最终构建](artifacts/build-x64-debug.txt)、[测试](artifacts/direct-x64-debug.txt)、[XML](artifacts/direct-x64-debug.xml) |
| x64 Release | 增量完整目标构建，随后增量复核 | 223/223，13.27 秒 | [首次记录](artifacts/build-x64-release-initial.txt)、[最终构建](artifacts/build-x64-release.txt)、[测试](artifacts/direct-x64-release.txt)、[XML](artifacts/direct-x64-release.xml) |
| x86 Debug | 新 build-x86，本配置首次完整编译，测试转换修正后增量复核 | 223/223，16.86 秒 | [首次构建](artifacts/build-x86-debug-initial.txt)、[最终构建](artifacts/build-x86-debug.txt)、[测试](artifacts/direct-x86-debug.txt)、[XML](artifacts/direct-x86-debug.xml) |
| x86 Release | 新 build-x86，本配置首次完整编译 | 223/223，13.73 秒 | [构建](artifacts/build-x86-release.txt)、[测试](artifacts/direct-x86-release.txt)、[XML](artifacts/direct-x86-release.xml) |

四份 XML 均为 `failures=0, skipped=0`。直接相关过滤器固定为：

```text
^(WavLifecycle\.|MonitorEngine\.|MonitorScopeReader\.|WavSink\.|WavFile\.|CaptureTrackList\.|DumpUi\.|GuiLanguage(Text)?\.|DelayFifo\.|ScopeBuffer\.|SampleConvert\.|AudioFormat\.)
```

执行命令（`<build>` 为 `build` 或 `build-x86`，`<config>` 为 `Debug` 或 `Release`）：

```powershell
cmake -S . -B build-x86 -G 'Visual Studio 18 2026' -A Win32
cmake --build <build> --config <config> --target ALL_BUILD -j 6
cmake --build <build> --config <config> --target WinAudioPerformanceGui -j 6
ctest --test-dir <build> -C <config> --output-on-failure -R '<上方过滤器>' --output-junit <xml>
```

构建均为零 error。既有 warning 保留并注明：`test_wave_envelope.cpp` 有两条 C4828；x86 `AppUi.cpp` 的既有 Inspector 左移表达式有 C4293（归属 `07021f04`，不在本票 diff）。x86 Release 的 ALL_BUILD 有三条既有 warning，额外 PerformanceGui 有一条既有 C4293。首次 x86 Debug 的本票两个 C4244 已由 `39d0047` 的有界 `size_t` 转换修正，不扩修无关代码。

## 热路径与证据完整性

四配置编译均通过原子 lock-free/alignment 静态断言。核对唯一 WavSink 的 gate、ring 与 writer：保留 `seq_cst`、每次 push 至多一次 strong CAS；未新增锁、分配、通知或 I/O 热路径。x64/x86 Debug 的 `dumpbin` 核对显示：生产 `WinAudioCore.lib` 中 `wavSinkTest` 符号为 0，专用同源 `WavSink.obj` 中为 3。计数及符号输出见[热路径与测试钩子证明](artifacts/hot-path-and-hook-proof.txt)。

[证据索引](artifacts/evidence-index.json)固定验证提交、原日志 SHA-256 与归档 SHA-256。MSBuild 的 UTF-16 日志转为可审查的 UTF-8；内容保留，行为红测、构建红测和最终绿测分别存档。真实 GUI、设备、性能与图表使用各自的新结果，不能以这里的单元测试代替。

## 固定版本探针与正式配对

在原目录下用既有 `build-wav-sink-probe.ps1` 导出源码快照，不创建 worktree。基线固定为 `9360b232513b22a41014fec19eb0c536c9e3689f`，候选固定为 `649015e15576d570aa06f1b5636dffe2c053010f`；候选生产源码与完成四配置回归的 `39d0047` 相同。两个快照均为 x64 Release、同一工具链和 `spdlog=6fa36017cfd5731d617e1a934f0e5ea9c4445b13`，额外构建了 `WinAudioPerformanceGui`，候选加 `-Monitor` 构建实际 Monitor 探针。源码、依赖、驱动脚本、exe SHA-256 和四份构建日志见[探针溯源](artifacts/probe-provenance.json)。构建脚本自带 `result.csv` 仅为预检，排除在正式比较外。

总控明确确认 GUI 已退出、没有其他音频/构建/磁盘负载后，按旧 1、新 1、旧 2、新 2、旧 3、新 3 顺序运行。每轮正常和关闭路径各 1000 次 push，48000 Hz / 2ch / Float32、480 帧、10 ms 固定供给周期，使用真实文件 writer；每轮约 20 秒。每轮使用独立 WAV/CSV，生产 WavReader 和额外独立解析均验证全部 1000 块逐字节一致；正常路径均接收 3,840,000 字节，关闭路径接收 0。全部六个 WAV 的原始 SHA-256 相同；gzip 解压后再次核对原始字节。见[六轮 CSV、WAV 哈希与时间记录](artifacts/sink-provenance.json)和[正式运行脚本](artifacts/sink-runner.py)。

| 版本/轮次 | 正常 p50 / µs | p95 / µs | p99 / µs | 实际接收 / byte/s | stop / ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 旧 1 | 1.1 | 1.9 | 13.5 | 383906.876 | 0.5791 |
| 新 1 | 1.1 | 2.0 | 26.0 | 383876.503 | 0.6614 |
| 旧 2 | 1.2 | 1.9 | 22.6 | 384081.026 | 16.1464 |
| 新 2 | 1.2 | 1.9 | 29.6 | 383873.207 | 16.5329 |
| 旧 3 | 1.2 | 2.0 | 25.1 | 384219.512 | 15.7837 |
| 新 3 | 1.1 | 1.9 | 11.3 | 384142.963 | 16.3025 |

关闭路径六轮 p50=0.1 µs、p95=0.2 µs，p99 为旧 0.4/0.4/0.3 µs、新 0.5/0.4/0.4 µs。计时包含两次时钟读取，结果呈约 0.1 µs 粒度。正常 p99 有波动且新版前两轮较高，保留所有结果，不宣称加速、吞吐上限或硬实时保证；stop 也受到 writer 轮询和 Windows 调度影响。

## 真实虚拟端点 Monitor 冒烟

固定候选探针直接运行生产 MonitorEngine/WavSink，源为 CABLE Input `{0.0.0.00000000}.{09793ccf-e246-4dbf-800b-a7cf44856994}`，mix=48000 Hz / 8ch / Float32。首次目的端 CABLE In 16ch `{0.0.0.00000000}.{38a33ff5-6929-4608-8859-e17e9732f9ad}`（mix=192000 Hz / 8ch）在 render `Initialize(shared,requested)` 请求 48000 Hz / 8ch / Float32 时返回 `AUDCLNT_E_DEVICE_IN_USE`，采集已打开但没有进入 Dump，探针 exit=8。保留[首次失败日志](artifacts/device/failed-cable-runtime.txt)及两端 caps；该 HRESULT 不足以证明独占持有者或共用驱动冲突的根因。

只把命令行目的端换为已枚举的 Dubbing Speaker `{0.0.0.00000000}.{07eb2780-45a9-42b2-aef9-6604815fa189}`（mix=48000 Hz / 2ch），同一 exe 运行 exit=0。Shared 应用 render 请求并成功使用 48000 Hz / 8ch / Float32。没有修改生产源码、系统默认设备、设备格式、音量或增强设置。实际 WASAPI 流传输合成非零 PCM，覆盖播放停用后 Capture Dump 继续、Render Dump 单独完成、显式重启新代次、Monitor stop 同步收尾与下一 session 来源推进。

七个文件全部 Success，生产 WavReader 验证实际应用流格式、channel mask、完整帧和 EOF；独立 RIFF 解析再次验证长度、48000/8ch/Float32、mask=0x63F、非零且全部有限的样本，并保存 gzip、原始/压缩 SHA-256。见[逐文件设备溯源](artifacts/device/device-provenance.json)、[运行输出](artifacts/device/stdout.txt)及[完整日志](artifacts/device/runtime.txt)。本轮播放关闭请求观测 100 ns、render Idle 观测 15.42 ms，停播期间 capture 继续 9600 帧；这些是单轮观测，不是时延上限。

这是虚拟 WASAPI 端点验证，不代表物理麦克风/扬声器。实际慢 close GUI、截图与图表预算由总控另行记录在[性能与设备总记录](performance-and-device.md)，单独区分可证明的范围。

## 总控本地验收收据

2026-10-27，总控完成实际 AppUi 慢关闭演示、3 份 GUI WAV 读回、偏好恢复，以及固定前后快照的图表冒烟。Render close 等待 135.306 秒期间，设备可先停用、capture 继续，重新 engage 不自动重开文件；成功关闭后才揭示同代结果。Monitor stop 仍等待两侧文件完成。GUI 截图、原始日志、WAV gzip 和二进制溯源均已提交到 `artifacts/gui/`。

前后图表均运行 3 秒 warmup + 40 秒稳定场景；新版四个稳定场景最高 p95=5.9603 ms、最高 p99=6.8096 ms，均通过。warmup、transition、teardown 分开保留；原始帧、config、进程采样和 SHA256 位于 `artifacts/charts/`。没有并行构建或音频测试。总控独立核对 3 份 GUI 与 7 份设备压缩 WAV 的原始/归档 SHA256，图表原始帧解压后逐字节相同，文档相对链接均存在。

固定后测为 `649015e`；后续 `9483853` 及最终总控证据提交仅添加文档/测量文件，发布前再次核对 `src/`、`scripts/performance/` 和构建配置无增量。两轴代码审查无遗留；远端 CI、squash 合并和 issue 关闭由总控继续核验，不以本地通过代替远端交付。
