# WAV Dump 实施记录：#233

## 目标与基线

- 按 #233 → #234 串行实施；#233 合并且关闭、#234 原生 open blocker 为 0 后才启动 #234。
- 远端：`git@github.com:peilinok/winaudio.git`。
- 2026-10-24 已 fetch；最新 `origin/main` 为 `3c433575f0bfc77808eaa09113ba1eb2052ffb4e`，与设计复核基线一致。
- 原始目录 `E:\WORK\vibecoding\winaudio` 保持不动；其 `CONTEXT.md`、ADR-0003 与设计文档仅将直接相关内容复制到实施工作区，截图与 `.archify` 不带入。
- 工作区：`C:\Users\peili\.codex\worktrees\wav-dump-233\winaudio`；分支 `codex/wav-dump-233`。
- 开始时两票均 OPEN，无 assignee、评论和对应 open PR；#233 blocked_by=0，#234 blocked_by=1，唯一 blocker 为 #233。已领取 #233。

## 已授权测试边界

真实 WavSink 的 start/push/requestStop/stop/status 与 WAV 读回；窄文件输出 adapter；同源专用测试构建的三个确定性调度点；CaptureTrackList 与既有 Monitor fake backend；直接相关 GUI 行为。用户已明确授权这些测试 seam，无需再确认。

## 验收台账

- [x] 既有 sink、Capture Track、Monitor 直接回归基线，记录原有失败。
- [x] 三个固定交错：读准入后、准入后、数据发布前；记录旧实现红测，再最小修复。
- [x] 准入后 stop 等待、最后一块完整排空、旧快照重启后拒绝；文件字节顺序与头正确。
- [x] 对齐且始终 lock-free 的 64 位 gate，代次不复用，单次 strong CAS、seq_cst、退出 RMW 保留状态。
- [x] 固定 Capture Track 来源；连续多轮 Dump；停止后继续输入，Scope tap 推进且设备不停。
- [x] 可选/必需文件策略固定、正常拒收与真正失败区分、兄弟 Track 隔离。
- [x] overflow/stop 交错、write 后 close 失败、首错保留、重复 stop、失败后重试。
- [x] destroy/destroyAll/析构/启动准备异常与失败正确回收；pump 只请求停止。
- [x] 原有格式、命名、碰撞后缀、应用名/PID、目录与 CLI 精确路径、失败文件保留。
- [x] 生产路径无锁/分配/等待/文件操作/共享所有权/测试钩子；不改变生产类布局。
- [x] 既有 Monitor 显式停止和文件读回通过；不提前迁移 #234 自动停用与 GUI 语义。
- [x] 同格式、块大小、producer/writer 负载的前后吞吐与 p50/p95/p99；关闭路径另记。
- [x] Debug/Release、x64/x86 完整构建和直接回归。
- [x] 真实设备/GUI 冒烟与证据边界（总控单独核验）。
- [x] Standards / Spec 固定基线两轴代码审查，修复发现的问题并复测；最终测量材料另做增量复核。
- [ ] 独立 PR、远端 head 校验、必需 CI 成功、squash 合并、主线祖先关系与 issue 关闭核实。

## 进度

业务实现提交 `abf5affa4e4453efb0d7ad66b4cc2e991e432588`，补充覆盖提交 `ede33e81ca9eb10a35f2b125afd662465eec8378`。x64/x86、Debug/Release 四配置完整构建成功，每配置生产直接回归 102/102、专用同源回归 58/58 均通过。性能、真实设备/GUI、独立审查与远端交付由总控继续核验。

## 红绿测试、构建、审查与发布证据

以下按真实执行顺序保留红绿证据；未完成的性能、设备/GUI、CI 与审批不由本地单测代替。

### 2026-10-24：既有基线与测试接线

- VS2022 preset 在本机不可用（未安装 VS2022）；改用本机 VS2026 生成器、独立 `build-233`，不修改预设或 CI。
- `git submodule update --init` 完成。
- 基线命令：`build-233/bin/Debug/WinAudioTests.exe --gtest_filter=WavSink.*:WavFile.*:CaptureTrackList.*:MonitorEngine.*:DumpUi.*`，96/96 通过。日志：[baseline-x64-debug.txt](artifacts/baseline-x64-debug.txt)。
- 增加窄文件输出 adapter 与同源专用测试目标；此时仍保留旧生命周期算法。三个 producer 调度点不改变类布局；writer drain 仅被动报告、不阻塞。文件 adapter 在 close 处同步以避免红测触发真实释放后访问。

### 切片 1：已准入 push 与停止

- 红测：`WinAudioWavLifecycleTests --gtest_filter=WavSinkLifecycle.StopWaitsForAdmittedPushAndKeepsItsBytes`。旧实现 writer 实际进入 close 时 producer 仍暂停，读回 0 字节而非 8；见 [red-01-admitted.txt](artifacts/red-01-admitted.txt)。
- 最小修复：同字 Ready/Closed/Active；一次 strong CAS 准入，退出 RMW 保留 Closed；writer 观察到 Active 时继续等待，最终读 ring 在 Closed/no-Active 观察之后。
- 此切片暂未加入重启代次，下一切片先固定旧快照反例再补齐。
- 切片 1 绿测及直接 sink 回归：10/10，通过，见 [green-01-admitted.txt](artifacts/green-01-admitted.txt)。

### 切片 2：旧快照与重启隔离

- 红测先在尚无 generation 的实际同字 gate 实现运行：迟到 push 接收 4 字节，新文件出现旧的 111/-111 前缀，见 [red-02-snapshot.txt](artifacts/red-02-snapshot.txt)。
- 最小修复：gate 高 61 位加入单调、不复用 generation；达到上限拒绝启动。停止不清零代次。
- 切片 2 绿测与直接 sink 回归 11/11，通过：[green-02-snapshot.txt](artifacts/green-02-snapshot.txt)。

### 切片 3：最后一块发布

- 用 `git show 3c433575:src/core/WavSink.{h,cpp}` 重放原始算法，仅接入相同输出 seam/调度点；恢复后的基线源与生产 ring 在 head.store 前停住。
- 旧实现确已进入 close，尚未发布的最后 8 字节最终全部丢失；[red-03-last-publication-legacy.txt](artifacts/red-03-last-publication-legacy.txt)。测试期间 close 受控，避免资源提前释放后继续访问。
- Active 修复覆盖相同风险；保留独立 BeforePublish 行为测试。

### 切片 4：首个实质错误

- 先写入失败、close 再失败的红测，旧行为将主错误覆盖成 later close failure；[red-04-first-error.txt](artifacts/red-04-first-error.txt)。
- close 仅在尚无主错误时写 message；重复 stop 保持相同失败。此时直接 sink 13/13，通过：[green-04-first-error.txt](artifacts/green-04-first-error.txt)。

### 切片 5：准备异常、清理及就绪

- 红测在文件已 open、buffer/scratch 准备抛 bad_alloc 时，旧 writer 未捕获，测试进程退出 3：[red-05-preparation.txt](artifacts/red-05-preparation.txt)。
- 修复 writer 入口捕获并统一 close、错误、就绪通知；scratch 预分配后才 Ready。start 控制路径捕获 ring/线程创建异常；start/stop 经内部 control mutex 串行，控制侧 CV 等就绪，去掉伪 2 秒超时轮询。
- adapter 准备异常后 close 已发生、重复 stop 保留失败、同 sink 可重新启动；绿测及 sink 直接回归记录 [green-05-preparation.txt](artifacts/green-05-preparation.txt)。

### 切片 6、7：来源与 overflow 首错

- 来源红测：绑定 42 却接收来源 41，新文件出现错误前缀；[red-06-source.txt](artifacts/red-06-source.txt)。准入后以 seq_cst 核对原子来源；来源不符退出 Active，不访问 ring；绿测 15/15：[green-06-source.txt](artifacts/green-06-source.txt)。
- overflow 首错红测：overflow 先发生，但主错误被之后 write failure 取代；[red-07-overflow-error.txt](artifacts/red-07-overflow-error.txt)。改用原子 Failure 首次 CAS 锁定主错误；producer 不构造字符串，writer 负责错误文案与后续诊断。绿测 16/16：[green-07-overflow-error.txt](artifacts/green-07-overflow-error.txt)。后续测试 adapter 进一步用 CV 保证 write 故障只能在 overflow 返回之后发生，消除偶然时序依赖。

### 切片 8：必需 WAV 异步失败

- 真实红测：已成功接收最后一块后 writer 写失败，不再供 PCM，原 pump 长期保持 Running；[red-08-required-async.txt](artifacts/red-08-required-async.txt)。
- 修复 Track：创建期 const wavFatal、const source generation；直接 sink.push，不把 isRunning 当资源保护；每轮 pump 用无锁 hasFailed 查询，必需文件真实失败仅标记对应 Track Error。pump 退出仅 requestStop；控制方先 join pump 再 sink.stop。
- 首次绿测及已有 Track/sink 47/47：[green-08-required-async.txt](artifacts/green-08-required-async.txt)。

### 切片 9：准备期间的停止请求

- 窄文件输出 seam 固定慢 open；请求停止后旧实现仍把 start 返回为成功，[red-09-cancel-preparation.txt](artifacts/red-09-cancel-preparation.txt)。
- Ready 改用同代 Preparing 的 CAS 发布；取消不算文件故障，控制方等待清理后返回 start cancelled。绿测 48/48：[green-09-cancel-preparation.txt](artifacts/green-09-cancel-preparation.txt)。
- start 自动命名也纳入内部 control mutex 串行区；WavWriter stdio buffer 设置错误现在通过 Result 返回。

### 直接回归覆盖扩充

- 同源专用测试补 ring/scratch allocation 与 thread launch 的故障注入，调用点完全剔除于生产；这些只注入异常，不增加暂停调度点。
- 两种 loopback fake backend 各连续三轮独立 Dump，每次停止后输入 PCM，两个 tap 继续、两个设备未 stop；可选 open/write/close 失败隔离；destroyAll/析构读回完整字节。
- Monitor 仅将 pump 退出的两个 sink.stop 移至 join pump 之后的 teardown，pump 退出改为 requestStop。自动 render 停用、GUI 状态/接线仍明确留给 #234。

### 2026-10-25：最终构建与直接回归

本机 MSVC 14.51.36231 / VS2026；CMake 生成器差异仅属于本机环境，仓库预设及 windows-2022 CI 未修改。全量构建包含 CLI、GUI、core、已有测试和专用测试 target；运行只限定本票直接组。

| 架构 / 配置 | 完整构建 | 生产直接组 | 专用同源组 | 证据 |
| --- | --- | --- | --- | --- |
| x64 Debug | 通过 | 102/102 | 58/58 | [构建](artifacts/build-x64-debug.txt)、[直接组](artifacts/direct-x64-debug.txt)、[同源组](artifacts/lifecycle-x64-debug.txt) |
| x64 Release | 通过 | 102/102 | 58/58 | [构建](artifacts/build-x64-release.txt)、[直接组](artifacts/direct-x64-release.txt)、[同源组](artifacts/lifecycle-x64-release.txt) |
| x86 Debug | 通过 | 102/102 | 58/58 | [构建](artifacts/build-x86-debug.txt)、[直接组](artifacts/direct-x86-debug.txt)、[同源组](artifacts/lifecycle-x86-debug.txt) |
| x86 Release | 通过 | 102/102 | 58/58 | [构建](artifacts/build-x86-release.txt)、[直接组](artifacts/direct-x86-release.txt)、[同源组](artifacts/lifecycle-x86-release.txt) |

可复现命令（在本票工作区执行）：

```powershell
cmake -S . -B build-233 -G 'Visual Studio 18 2026' -A x64
cmake -S . -B build-233-x86 -G 'Visual Studio 18 2026' -A Win32
cmake --build build-233 --config Debug -j 4
cmake --build build-233 --config Release -j 4
cmake --build build-233-x86 --config Debug -j 4
cmake --build build-233-x86 --config Release -j 4
# 对 build-233 / build-233-x86 的 Debug / Release 各执行以下两个命令：
.\build-233\bin\Debug\WinAudioTests.exe --gtest_filter=WavSink.*:WavFile.*:CaptureTrackList.*:MonitorEngine.*:DumpUi.*
.\build-233\bin\Debug\WinAudioWavLifecycleTests.exe
```

本票新增或修改的源文件未出现编译警告。保留无关既有警告：x86 `AppUi.cpp:1657` 的 C4293 位移警告；`test_wave_envelope.cpp` 的 C4828 文件编码警告。两文件相对设计基线无 diff，本票未修改。

### 生产路径与测试隔离证明

- `WavSink.h` 不包含测试宏条件字段。实编布局探针显示 x64 生产/测试均 `sizeof=472, alignof=8`；x86 均 `sizeof=320, alignof=8`，四次 `atomic<uint64_t>::is_always_lock_free=1`：[布局与锁自由](artifacts/layout-lock-free.txt)。
- 四配置编译期断言均通过：64 位 gate 始终 lock-free，类和 gate 对齐至少 8 字节，Failure 原子查询始终 lock-free。gate/source 使用 seq_cst；生产 RingBuffer 的 acquire/release 未改变。
- dumpbin 检查两个架构 Debug 对象：生产 `WavSink.obj`、`RingBuffer.obj` 均无 `wavSinkTest` 符号；专用 WavSink 对象包含 checkpoint、drainObserved、prepare 三类测试依赖：[符号证据](artifacts/production-hook-symbols.txt)。这些调用全部在 `WA_WAV_SINK_TESTING` 宏内，Release 生产 target 同样不定义该宏。
- 正常 push 源码只执行 gate load、一次 strong CAS、source load、现有 ring memcpy/head 发布和退出 fetch_and。正常路径无字符串、分配、mutex、条件变量、文件调用、系统等待、shared_ptr 或虚调用；关闭路径一次 gate load 即拒收。overflow 另以原子 CAS 锁定首错并关闭准入。
- writer 仍以 5 ms 轮询，保留 1–16 MiB ring 容量算法、256 KiB 批量与 stdio 缓冲；producer 不通知 writer 的系统事件。

### 原始实现的独立重放

[replay-legacy.py](replay-legacy.py) 从固定基线 `3c433575f0bfc77808eaa09113ba1eb2052ffb4e` 读取原始 WavSink；仅加输出 adapter 与三个调度点/被动观察，不改旧准入或停止算法。它在 `.scratch/wav-dump-legacy-replay` 中编译，复用已构建的 Debug 支持库，不修改 checkout 源码，也不复制简化 sink。

```powershell
py -3 docs/measurements/wav-dump-233/replay-legacy.py --core-build build-233
```

最终复跑三个确定性交错均真实失败：[原始实现三项红测](artifacts/replay-legacy-three-red.txt)。两个已准入暂停场景均在 producer 暂停时进入 close，最终文件丢失 8 字节；旧快照场景把旧的 111/-111 写入新文件。相同 seam 的修复后测试已包含在四配置 58/58 绿测。

准备异常单独覆盖 ring/scratch 分配与线程创建调用点：专用测试在真实准备点注入 bad_alloc/system_error，验证失败通知、close/回收、重复 stop 和重试；不依赖真实耗尽系统资源。超时仅用于报告测试死锁，确定性交错由 C++17 条件变量决定。

### 待总控完成的交付项

- 在相同格式、块大小与负载下完成性能比较，真实设备/GUI 冒烟分别记录证据边界。
- 业务固定提交 abf5aff 的 Standards/Spec 静态审查已由总控报告各 0 条问题；后续覆盖与文档需增量复审。
- 建立 PR、核对最终远端 head、完成完整 CI 与外部审批后按规则 squash；不得借 owner bypass 绕过所需的独立 approving review。
- #234 仍依赖 #233；本票未实施 Monitor 自动 render 停用、来源格式快照或 GUI 收尾迁移。

### Standards 增量修复：慢输出测试的失败清理

- 审查定位：SlowDumpClose 在启动 Dump 后的 ASSERT 提前返回时，原局部变量按 list → gate 顺序析构；list 的 writer close 等待尚未释放的 gate，测试会永久等待。
- 新增 `SlowDumpFatalAssertionDoesNotBlockTrackTeardown`，通过 gtest EXPECT_FATAL_FAILURE 在真实 TrackList/SlowOutput seam 中注入提前断言，分别覆盖 close 阻塞与 write 阻塞。
- 红测在无 RAII 守卫的真实实现中被 CTest 自动 `Timeout 30.02 sec`：[故障清理红测](artifacts/red-review-cleanup-bounded.txt)。首次超时接线曾使用在 CTest 中不生效的 if(TEST) 判断，该次故障进程手动结束；随后改为检查 discovery 生成的 WinAudioTests_TESTS 名单，JSON 确认超时后才取得这里的自动超时红测，不把首次手动结束表述为自动超时。
- 最小修复：声明于 TrackList 之后的 SlowOutputRelease 作用域守卫，在提前返回时先释放门，再让 TrackList 完成 pump/writer 回收；正常交错仍显式在原边界 release。两个受影响慢输出测试及故障回归均使用守卫。
- 只给这三个生产 CTest 注册 30 秒上限，专用 target 继续沿用已有 30 秒上限；没有扩大其他生产测试的超时配置。[实际注册属性](artifacts/review-cleanup-timeouts.txt)。
- 仅构建 Debug 的两个受影响测试 target，再运行六个受影响生产/专用 CTest，6/6 通过（5.68 秒）：[绿测](artifacts/green-review-cleanup.txt)。没有修改业务源码，也没有追加全量本地回归。

```powershell
cmake --build build-233 --config Debug --target WinAudioTests WinAudioWavLifecycleTests -j 4
ctest --test-dir build-233 -C Debug -R '^(WavLifecycle\.)?CaptureTrackList\.(SlowDumpCloseKeepsBothCaptureTapsAndDevicesAdvancing|OptionalOverflowKeepsCapturingAndDrainsTheAcceptedPrefix|SlowDumpFatalAssertionDoesNotBlockTrackTeardown)$' --output-on-failure
```

### Standards 增量修复：原始实现重放 helper 编译规则

- replay-legacy.py 生成的临时 target 现 include 仓库 cmake/CompilerWarnings.cmake，并调用 wa_set_project_warnings；覆盖 /W4、/permissive-、/utf-8、/sdl，没有另设一套规则。
- 重放构建无新增编译警告；原始三个确定性交错仍全部预期失败，脚本按预期退出 0：[重放结果](artifacts/replay-review-warnings.txt)、[生成项目编译属性](artifacts/replay-warning-flags.txt)。
- 这两项审查修复不改变业务提交 abf5aff 的生产代码。上述四配置 102/102、58/58 是修复前固定业务与覆盖快照的记录；新增故障清理用例本轮只跑所述六项直接回归，最终完整 CI 由总控继续。

### 总控核验：设备、GUI、性能和保护

- 两来源真实 WASAPI 各三轮正常收尾及持续 tap 验证通过；GUI 两来源均完成停止、重启、接收中销毁与正常退出。实际音频使用虚拟 CABLE endpoint，不改默认设备、音量或增强设置；DumpFolder 已恢复并读回核对。
- 六轮配对生产探针全部接收并逐字节读回 3840000 字节，关闭路径接收 0。正常 push 的旧版 p95=1.7 µs，新版 1.8–1.9 µs；新版 p99=11.3–21.8 µs，高于旧版 2.4–2.7 µs。保留这项成本，不宣称加速或硬实时。
- 前后四个图表稳定场景均满足 p95≤8 ms、p99≤16 ms；新版最高 p95=6.5197 ms、p99=7.1917 ms。原始帧、逐轮 CSV、WAV gzip、SHA256、GUI 截图均已归档，详见 [测量与设备记录](performance-and-device.md)。
- `git diff abf5aff..baa0fbc -- src/core` 为空；所测二进制的固定业务源码与最终实现一致。四配置日志、三项旧实现红测、生产无钩子证据由总控直接读取核对。
- 原始 checkout 状态仍为原有 CONTEXT/ADR/spec 与截图目录改动，三份相关文档 SHA256 与开始时一致；没有 stash/reset/clean 或改动原始文件。
- 两轴代码审查：Standards 最终遗留 0（增量发现的 P2 清理死锁与 P3 编译规则已修复并复核）；Spec 0。最终测量文档的增量复核、PR、CI 与审批记录在后续发布时补充。
- 2026-10-25 复查规则集 `protect-main`（18481045）仍要求 1 个独立 approving review、仅 squash。当前身份虽为 owner bypass actor，也不会绕过所需审批。#233 仍 OPEN；#234 唯一原生 open blocker 仍为 #233，未开始业务实施。
