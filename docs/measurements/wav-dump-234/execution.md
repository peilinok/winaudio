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
- [ ] 配对性能、真实设备 / GUI 与图表后测由总控执行，另存证据。
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

[证据索引](artifacts/evidence-index.json)固定验证提交、原日志 SHA-256 与归档 SHA-256。MSBuild 的 UTF-16 日志转为可审查的 UTF-8；内容保留，行为红测、构建红测和最终绿测分别存档。真实 GUI、设备、性能与图表后测仍需新结果，不能以这里的单元测试代替。
