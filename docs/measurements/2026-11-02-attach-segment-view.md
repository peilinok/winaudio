# 按进程实例查看附加记录段：#257 验收

本次在 `feat/attach-segment-view`、基线 `e2a86a079d117de8cff065c0ff8e6b9de84a18bf` 上实现 #257。无设备状态验证、真实双 PID 控制调用与生产 GUI 功能验证已通过；下文分别说明证据对应的版本与边界。没有暂存、提交或推送。

## 实现行为

- 用 PID 与原始进程创建时间确定实例。后台命令冻结预期实例，查询、注入前和接收交接均核验；活动目标保留进程句柄。进程退出以句柄信号判断，Live session 行消失不结束监控。
- 控制器为成功附加分配单调递增的记录段标记，拥有型输出携带该标记与实例身份。GUI 按标记接收，不在消费时用 PID 猜当前段。失败不建立成功段、不更换选择；同一活动实例重复请求不新增段。
- `CallLogStore` 继续作为唯一调用与段历史存储。每次重新附加建立新段，最后一个目标结束后也保留旧段；独立小列表选择调用来源，新成功目标不抢已有选择，当前段结束后停留在只读历史。
- 分离先关闭记录，再做一次有限读取，记录实际快照时点。归档后拒绝迟到调用和重复结束事件，明确显示覆盖未知，没有等待所有在途调用来声称完整退出快照。
- Call log 的查询、记录选择、接口选项和 Initialize 缓存按段划分。管线图仍由 Live session 独立选择，只用已核验同实例的最新段；新段没有 Initialize 时不会回填旧 Hook 段，已结束来源与最近流限制分别显示。
- 共享布局更新为 7，加入实例和 `observationGeneration`。真实 Hook 入口在原 API 前记住观测代次，返回时跨重新附加代次的调用不写入新段，参数、输出和 HRESULT 的原转发保持不变。已载不兼容 Hook 仍明确拒绝并提示重启目标。

## 验证与命令

证据目录：`.scratch/implement-pipeline-2026-11-02/257/`。所有最终构建、测试和真实探针命令的退出码均为 0。

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| 改动前 x64 Debug 相关基线 | 82/82 | `baseline-debug.log/json` |
| 历史保留缺陷先判红 | 1 项失败，退出码 1；旧逻辑会清轮次、历史和导出范围 | `red-history.log` |
| 在途调用跨重附加缺陷先判红 | 1 项失败，退出码 1；调用在原 API 内跨代仍被旧 Hook 写入 | `red-hook-generation.log` |
| 同批结束旧实例与新实例成功的收尾顺序先判红 | 1 项失败，退出码 1；最小修正后通过 | `red-tail.log`、`green-tail.log` |
| x64 Debug 状态、存储、图与生命周期 | 99/99 | `focused-debug-tail.log/json` |
| x64 Release 同组回归 | 99/99 | `focused-release-tail.log/json` |
| x64 Debug / Release 真实 Hook 入口 | 每配置 25/25 | `hook-debug-verified.*`、`hook-release.*` |
| x86 Debug / Release 真实 Hook 入口与布局 | 每配置 25/25 | `hook-x86-debug.*`、`hook-x86-release.*` |
| x64 Debug 双 PID 控制器与音频目标 | PASS，目标均正常退出 | `real-segments-debug-native.stdout.log`、`.receipt.json`、`.audio.json` |
| x64 Release 双 PID 控制器与实际 DLL 身份 | PASS，目标均正常退出 | `real-segments-release-native.stdout.log`、`.receipt.json`、`.audio.json` |

生产目标及测试按下面方式构建，没有使用 VS2022 preset，也没有并行运行两种配置的测试：

```powershell
cmake --build build --config Debug --target WinAudioTests WinAudioHookTests WinAudioGui HookAudioProbe --parallel 8
cmake --build build --config Release --target WinAudioTests WinAudioHookTests WinAudioGui HookAudioProbe --parallel 8
cmake --build build-x86 --config Debug --target WinAudioHookTests --parallel 8
cmake --build build-x86 --config Release --target WinAudioHookTests --parallel 8
```

定向 Core 测试过滤器如下；分别调用对应配置的 `WinAudioTests.exe`，再运行该配置的 `WinAudioHookTests.exe`。只执行直接相关回归，没有运行全量测试：

```text
CallLogStore.*:CallLogExport.*:PipelineAttachUiModel.*:PipelineAttachSet.*:OnDemandAttach.*:PipelineGraph.*:HookedCall.*:LiveSessionList.*:PumpWindow.*:RemoteInjectionTest.Background*:RemoteInjectionTest.FrozenExpected*:RemoteInjectionTest.ExitedFrozen*:AttachLifecycleTest.*
```

工具链为 Visual Studio 18 2026、MSVC 19.51.36257.0。最终四组构建日志未出现编译警告或错误；`git diff --check` 通过。首次 98 项运行的两个失败为新契约冲突的旧断言：分离后删除 pump 接口选项、映射名称固定布局 6。保留原失败日志 `focused-debug.log`，对应断言已改为历史仍保留、布局为 7，并保留隐藏时失效等原有保障。

## 场景证据

状态边界直接复用生产 `SegmentSelection → CallLogStore` 和控制器输出：验证首成功自动选择、已有选择不变、100 次切换与后台增长、失败不改变历史、退出后选择停留、同实例重新附加、确定性 PID 复用、旧标记迟到与重复归档、查询缓存切换以及最新段没有 Initialize 时图保持 Unknown。实际无音频设备的子进程夹具另验证冻结身份改变、排队目标退出、后台安装等待及取消/收尾行为。

真实探针新增 `HookAudioProbe --segments`，复用生产 `PipelineAttachController`、`SegmentSelection`、`CallLogStore`，以及原有自有目标和实际 WASAPI render＋端点 loopback。两个目标分别持续调用 `SetMasterVolume(0.250)` 与 `SetMasterVolume(0.750)`；逐条核对来源实例、方法、参数和 S_OK，切换 100 次时另一目标继续增长。随后分离 A、对仍存活 A 重新附加、确认旧段不变且新段没有补录 Initialize、正常结束 A、确认退出段，再分离 B。未复制附加或接收算法。

最终 Release 运行的 A/B PID 为 13192 / 23936，创建时间分别为 134380798033595491 / 134380798035586177。双方均为 48000 Hz、2 声道、32 位、格式标记 0xFFFE，端点为 `{0.0.0.00000000}.{46f4256f-bc60-4865-b5f3-4c6eef7ee966}`。A/B 实际回调 124 / 123 次，采集帧均为 91680，调用失败均为 0，真实进程退出码均为 0；最终历史 3 段、选择保持第 1 段。它是功能证据，不能替代长期性能或音质验收。

`run-real-segments.ps1` 缓存 `Start-Process.Handle` 并检查实际退出码，在目标存活时读取两个目标实际加载的 DLL 路径和 SHA256，并读取夹具共享状态中的实际格式。Release 实际加载的 Hook SHA256 为 `bafdd74df8abbb8c12d112a92898e38b551416e0f6851f590fab47674c79061f`；目标 EXE 的最终 SHA256 和两个实际加载路径见对应 `.receipt.json`，并与最终构建收据逐项核对。

## 生产 GUI

主 Agent 操作生产 Debug GUI PID 5836，以及自有真实音频目标 A=32316、B=29356，已验证：首 A 成功后自动选择；B 成功后保持 A；B 的调用与 A 的 Live session 管线图可以独立查看；分离 A 后停留旧段 1；同实例 A 重新附加产生新段 3，仍查看旧段 1；新段 3 退出后历史可继续选择；对已退出目标附加失败不产生成功段，也不改变当前选择。

只剩 B 活动时查看 A 历史，主进程已接收记录数从 4671 增至 4941，证明查看历史没有暂停后台接收。历史明确显示只读、快照时点与覆盖未知。对应截图保存在 `gui-evidence/`：`a-first-attached.png`、`b-attached-preserves-a.png`、`b-selected-calls.png`、`graph-a-calls-b-independent.png`、`a-detached-history-retained.png`、`a-reattach-new-segment-preserves-history.png`、`a-exited-history-selected.png`、`failed-attach-preserves-history.png`、`a-history-b-background-count-4671.png` 和 `a-history-b-background-count-4941.png`。

GUI 正常退出码为 0，详见 `gui-exit.json`。其中 watcher 的 `seconds` 含人工等待，不能作为 GUI 关闭耗时。A 正常停止后退出码 0、调用失败 0；GUI 退出后 B 仍存活，回调计数 28959 → 28976、调用失败 0，随后正常停止且退出码 0，详见 `targets.log`。

这些截图对应同批旧尾修正之前的稳定二进制，身份保存在 `gui-evidence/binary-hashes.json`、`receipt-before-tail.json` 与 `source-before-tail.patch`。最后自查对同批“旧实例结束＋新实例成功”补了可失败状态回归，修正为先接旧段有限尾部、按结束事件时点归档，再接新成功段；未改 GUI 绘制。修正后重新构建两种 x64 配置、各复验 99 项状态相关测试，并重新运行 Debug / Release 真实双 PID 探针。未重用旧截图声称覆盖同批旧尾新例或确定性 PID 复用，这两项由状态层另证。

## 身份与限制

`receipt.json` 保存 27 个改动源码文件、12 个产物以及本报告的 SHA256；`source.patch` 为本票生产源码及测试的实际补丁，SHA256 为 `6572bfe96d195033b0bfdbf93e7832fba2dab440afbf633a1c37ebfa6ecaa4c0`。新增 `ProcessInstance.h` 单独纳入源码收据。修前源码补丁 SHA256 为 `c306ba8b88bc7622d24094f32d07b44c07363320bd06a487f913de7976d867e4`，保存在 `source-before-tail.patch`。`protected-check.json` 确认原有 6 个受保护文件逐字节未变；原有未跟踪文件保留，未改 AGENTS.md 或 CLAUDE.md。

本票以控制调用形成完整功能切片。逐方法配置及继承由 #258 实施；共享传输的完整提交、MPSC/回绕和缺失证明由 #259 实施；每段最近 1024 条高频记录与源端统计由 #260 实施。现有有限 pump 浏览窗口只增加分段历史保留，不宣称这些后续票已经完成。`observationGeneration` 供后续票复用，不另起第二套观测代次。

当前段清空和当前段完整冻结导出分别属于 #261 / #262；既有导出仍按原记录范围工作。正式高频、长期内存、GUI 帧预算与真实音频性能属于 #263。未覆盖 x86 实际注入、用户未提供的应用、未知 COM 实现或附加之前的调用。所有历史明确为已接收数据的有限快照，传输完整性仍标未知。

生产 GUI、状态测试与真实探针按各自覆盖范围分别判定，不互相替代。
