# 启动入口门控与异常收尾技术回路

本报告对应 [#255](https://github.com/peilinok/winaudio/issues/255)，依据[父规格 #252](https://github.com/peilinok/winaudio/issues/252)及[启动设计 #241](https://github.com/peilinok/winaudio/issues/241)。起点为 `502565f3c8b49c1b0d05b1dab8df32a1877733c4`。本票交付可复用的真实启动控制路径和受控验证入口，不表示完整启动 GUI、逐方法配置、记录段代次或后续产品握手已经交付。

## 控制路径与提交点

`launchAtEntry` 在专用 Win32 调试线程创建同位数目标。外层调用方保留临时 job、目标进程句柄和调试线程句柄；目标以 `DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED` 创建，加入 `KILL_ON_JOB_CLOSE` job 后只恢复创建时的一次挂起。

创建调试事件中的实际映像基址，加上从目标内 PE 头读取的入口 RVA，决定本工具的单字节断点位置。命中该主线程入口后恢复原字节及执行位置，再增加一次本工具持有的显式挂起。继续调试事件后，准备线程调用现有 `OnDemandAttach`，调试线程持续处理加载器、远程线程等事件；不在暂停全部线程的事件中等待安装完成，不在 DllMain 初始化 COM，不用固定 Sleep 猜测就绪。

准备成功后设置本工具的单步标志，只撤销自己的一次挂起，实际收到主线程执行原入口指令后的单步异常，再清除该标志、继续事件并解除调试。`ResumeThread` 返回值和解除调试本身都不单独充当主线程前进的证明。外来挂起计数发生变化时安全失败，不循环恢复他人持有的挂起。

解除调试后仍由 job 保护未提交目标。最后清除 job 的 `KILL_ON_JOB_CLOSE`，以该系统调用成功作为提交点；代码先发布 `committed`，再进行日志或回调。此后即使成功响应丢失或工具强退，目标也独立运行。提交前的确定退出会失败；进程存活检查与 job 提交不是跨进程原子操作，目标可以在成功提交后立即退出，那属于后续目标退出，不能声称消除了所有并发退出窗口。

正常失败／取消显式终止本次自建目标并确认退出。控制线程 `ExitThread` 绕过栈析构时，外层监督仍收尾未提交 job；工具进程强退时，job 和尚在连接中的调试终止策略分别提供实际验证过的保护。准备回调必须响应取消或目标退出，未结束的远程操作继续由既有资源寿命逻辑托管，不强停远程线程。

## 可重复入口

```powershell
cmake --build build --config Debug --target WinAudioTests StartupLaunchBoundaryTests --parallel 8
build/bin/Debug/WinAudioTests.exe --gtest_filter="StartupLaunchTest.*:RemoteInjectionTest.*:AttachLifecycleTest.*:RemoteThreadWait.*:OnDemandAttach.*"
build/bin/Debug/StartupLaunchBoundaryTests.exe
build/bin/Debug/StartupGateProbe.exe --audio
build/bin/Debug/StartupGateProbe.exe --audio-release
```

将 `Debug` 替换为 `Release` 可运行另一配置。`StartupGateProbe --audio` 使用真实 `OnDemandAttach` 和实际接收通道；`--audio-release` 只改变受控目标中 client 的释放时机，用于核对额外的内部 Stop 来源。测试还通过 `StartupGateProbe --controller TOKEN CHECKPOINT` 创建独立工具进程，在命名事件指定的阶段强制退出，核验其目标状态。

无设备测试走同一个生产门控函数；入口所有权测试的无操作准备回调不冒称安装音频 Hook。独立的系统边界测试直接编译原生产 `StartupLaunch.cpp`，仅替换创建线程和等待调试事件这两个 Win32 调用；远程操作、`OnDemandAttach`、job、恢复及监督逻辑均使用原实现。正式产品必须由准备回调完成能力、初始配置、接收等后续票要求的核验，不能用无操作回调建立音频覆盖承诺。

## 验证结果

系统为 Windows 11 专业版 `10.0.26200`，本机沿用 `Visual Studio 18 2026` CMake 缓存。两配置按顺序执行，未并行运行同一 build 的两套测试。

| 证据 | Debug | Release |
| --- | --- | --- |
| 直接相关命名回归，含 17 个启动测试 | 40/40 | 40/40 |
| 创建线程错误与真实挂起安装组合 | 4/4 | 4/4 |
| 真实 Hook，入口后 CRT 中四个已知调用 | PASS，目标 32280，退出 0 | PASS，目标 26872，退出 0 |
| 提前 Release 诊断对照 | PASS，目标 32100，退出 0 | PASS，目标 7792，退出 0 |
| 新改动编译警告 | 0 | 0 |

启动测试使用真实 ASLR EXE。目标在 CRT 初始化独立记录映像基址、入口 RVA、原指令字节及 QPC，在 main 再记录进度；准备阶段两者均未发生。目标的实际基址加 RVA、恢复后的字节与控制端一致。真实远程 `LoadLibraryW(version.dll)` 在主线程入口保持期间完成，证明调试事件泵没有把加载器或远程线程永远停住。

同步检查点为创建、入口保持、准备、接收就绪、撤销挂起后、实际单步前进后、解除调试后、已提交。前七个检查点分别执行取消、期限到达、控制线程 `ExitThread`，均确认未提交目标退出。独立控制器在八个检查点分别被 `TerminateProcess`：先确认目标仍存活，工具退出后前七个目标均退出，已提交目标继续运行并响应正常停止。另验证控制器正常退出后目标继续运行；提交后的 `ExitThread` 响应丢失也不回滚目标。

目标主动退出有独立证据：TLS 仅创建不等待、不调用 COM 的辅助线程，辅助线程在入口保持期间响应同步事件并自行 `ExitProcess(75)`；另在已解除调试但尚未提交时自行退出 75。两者都被确认退出并判为失败。外部终止检测另行测试，不把控制器的 `TerminateProcess` 冒充目标主动退出。目标入口前的原生 DebugBreak 被继续为未处理异常，真实退出 `0x80000003`，没有吞掉目标自己的断点。

独立审查后补充 `EntryReleased`、`EntryAdvanced` 两个切换阶段的自主退出：到达所选阶段才发出请求，由目标原辅助线程执行 `ExitProcess(75)`。测试故障注入器有限推进单步、加载器和退出调试事件，防止在暂停全部目标线程时等待辅助线程；真实退出码、已到达阶段、不提交、退出确认均独立断言，结果必须为 `ERROR_PROCESS_ABORTED`，不能用期限到达代替退出检测。它是带事件干预的阶段故障注入，不证明所有自然调度下的退出事件竞争；原有准备期由生产循环接收 `EXIT_PROCESS_DEBUG_EVENT` 的测试仍保留。

真实未完成安装组合使用原 `RemoteInjectionGate.dll`：在入口保持期以真实远程 `LoadLibraryW` 进入该测试 DLL 的同步门，保持 loader lock，再调用真实 `OnDemandAttach::start`。只有独立观测线程同时确认 Hook 共享映射为安装未完成、两个路径参数页内容有效、目标存活且额外远程线程已经建立，才发布故障注入事件。随后分别取消、到达启动期限、在真实 `WaitForDebugEvent` 返回边界执行控制线程 `ExitThread(71)`，以及对独立控制器 `TerminateProcess(81)`。前三者验证生产监督终止目标并 join 准备线程，然后核对远程 owner 参数已清、pending 预留为零、关闭测试自身映射引用后命名映射消失；工具强退由外部父测试先独立读取活跃参数和映射，再确认控制器及目标实际退出、映射消失。未提前释放共享映射或远程参数，未强停目标中的远程线程。测试门只存在于测试 DLL／目标及系统边界测试，不改变正式 Hook 的时序或热路径。

该组合覆盖真实 Hook 远程加载等待和新启动控制器的监督／终止／join；不是把本地空准备回调或旧分离测试当成组合证据，也不宣称验证了 Hook 内任意安装指令处的强退。

工具强退后的未提交目标，系统观测退出码可能为 0 或 `0xC0000354`；通过判据是强退前仍存活、强退后实际进程句柄已发信号，不把这些失败收尾的退出 0 当成业务正常完成。

## 首条调用、来源与额外 Stop

两配置目标 PE 均为 AMD64，`DYNAMIC_BASE` 和 `HIGH_ENTROPY_VA` 已开启，首选映像基址均为 `0x140000000`。最终真实目标实际基址分别为 Debug `0x7FF67F8D0000`、Release `0x7FF611D50000`，入口 RVA 分别为 `0x39C7`、`0x2690`，不是把首选加载地址或 main 当作停止点。

真实模式在主线程入口后 CRT 初始化中，以单线程显式执行 `Activate → Initialize → Start → Stop`，保存源端线程、完整 client 地址、格式和各 HRESULT。接收端逐条核对顺序、接口、方法、返回值、Activate 输出地址、Initialize 实参及后续流归属；QPC 证明接收就绪先于 CRT 和首条调用。默认目标保留 client 至接收快照完成，四条记录均为 S_OK，主线程继续进入 main，最终目标退出 0。

提前释放 client 的对照中，真实共享传输行数在 `client.Reset()` 前为 4、之后为 5，`CoUninitialize` 后仍为 5；第五条是相同流的 `Stop(S_FALSE)`。因此它来自对象释放期间的内部调用，源端显式控制序列仍只有四次。生产代码不删除 S_FALSE，也不为通过探针而放宽次数；默认与诊断对照分别核对四条和五条记录。

当前协议没有 Hook 线程字段，`OnDemandAttach::drain` 也不自行填 PID；本探针按已绑定且核验过的接收器 PID 归属，并用独立目标标记确认源端线程等于创建的主线程。这不是宣称现有 IPC 已提供 TID 或完整实例／代次身份。现有 streamId 的截断指针也不在本票被当成跨段永久身份。

实际加载的 Hook 由现有内容命名及 `inspectHookModules` 核验；具体路径、SHA256、目标／探针／测试源码与二进制身份在本地证据清单中。就绪仍限定为当前已发现并验证的对象实现，不能从 `installed=ok` 外推所有 Core Audio 实现。

## 判红与限制

- `red-entry.log` 保存公共入口尚未实现时的真实测试失败，再实现入口门控后转绿。
- 原型中提前关闭系统拥有的调试事件进程／线程句柄，会在 `DebugActiveProcessStop → CloseAllProcessHandles` 触发无效句柄，CDB 保存 `0xC0000008`。修复为保留这些系统调试句柄至退出／解除调试；本工具自己取得的 PI 句柄仍由外层关闭。复验实际解除调试与多阶段故障回路通过。
- `red-detached-self-exit.log` 保存目标在解除调试后已退出却仍被提交的判红；加入最终进程存活核验后同用例转绿。该修复只拒绝提交前已经确定退出的目标，不把存活检查夸大为原子保证。
- `red-thread-creation.log` 用确定性 Win32 创建失败替身保存真实错误被覆盖的判红；仅在系统调用未出错且工作线程异常结束时合成异常错误，复验保留 `ERROR_NOT_ENOUGH_MEMORY`。不耗尽系统线程或资源。
- `red-released-self-exit.log` 保存撤销挂起后目标已自行退出 75 却被误报为超时的判红；生产循环增加实际进程退出核验，同用例 `green-released-self-exit.log` 转为 `ERROR_PROCESS_ABORTED`。
- 本票没有运行本地全量 CTest，没有修改既有硬件 `AudioEffects.SetHardwareSkipsIfUnavailable` 失败；完整构建／全套测试由当前 PR HEAD 的 CI 核验。
- 首轮审查指出的真实未完成安装组合和两个切换阶段自主退出缺口，已按上述同步回路补验；最终两配置证据分别保留在 `pending-install-final-{debug,release}.log`、`tests-review-{debug,release}.log`。首次判红与审查者独立日志均保留，没有覆盖。
- 不承诺入口前 DLL／TLS、先行辅助线程、其他 PID、未知对象实现或所有普通应用的启动调用。没有在 TLS／DllMain 中调用 COM，没有取得用户原应用，也没有以此声称其全部问题已解决。
- 逐方法选择、观测代次、完整传输、GUI、正式启动握手、批量组合和性能签收属于后续票。本票仅将已经实测的门控和收尾能力交给后续复用。

本地完整命令输出、系统／PE 与哈希清单保存在 `.scratch/implement-pipeline-2026-11-02/255/`；该目录是本次运行证据，不作为已推送的永久附件。报告给出可重复命令，远端 CI 结果由 PR 另行记录。
