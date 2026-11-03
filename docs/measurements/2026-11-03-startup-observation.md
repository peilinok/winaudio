# 启动观测：产品握手、共同接收与有限入口覆盖

本报告对应 #244，基于 `0ef48db058b72e0752a4dbf565e3592949d37b62` 实施。Pipeline 新增独立的“启动并观测”入口，不要求已有音频会话；显式创建目标并在主线程 EXE 的 PE 入口保持期间完成 Hook、初始方法配置和接收准备，再恢复入口、确认主线程已前进、解除调试并提交。

这实现了 ADR-0004 已确定的同位数、显式目标路线。ADR-0004 仍是原有未跟踪草案，本票没有把整份用户草案纳入提交或改写其他未交付范围。#255 的技术门控结果是本票基础，本报告另列实际产品握手、真实音频和 GUI 证据；二者不能互相替代。

## 交付行为与所有权

GUI 可填写 EXE、原始参数和工作目录，空工作目录采用 EXE 所在目录。创建前把 EXE 按 WinAudio 当前目录解析成明确的绝对路径，再显式传给 `CreateProcessW`，参数原样交给目标，不经过 shell；验证了绝对／相对路径、空格、Unicode、引号参数、默认目录及显式工作目录。初始逐方法选择默认开启关键控制调用，缓冲区和查询默认关闭。

`PipelineAttachController` 的专用启动线程只拥有准备中的启动工作。它复用生产 `launchAtEntry` 和 `OnDemandAttach`，在门内核对实例、内容命名 Hook、共享布局、已发现实现能力、初始方法／观测代次及可用接收状态。初始选择发布后回读完整方法位图和代次。安装期间关闭记录，安装自有调用不作为应用调用；接收 cursor 在门内建立，放行和拥有型交接时不重新跳到最新写位置。

成功目标交给唯一共同 receiver、`PipelineAttachSet` 和 `CallLogStore`，复用逐 PID 配置、控制调用、最近 1024 条高频窗口、累计统计、历史、清空和导出。没有另一套 Hook、共享 ABI、传输队列或历史模型。启动准备与普通批量安装有独立 owner，旧目标的接收、配置和单项分离继续由共同 receiver 处理；新的成功段不抢占已有查看选择，调用视图与管线图仍独立。

取消与清除 job 的 `KILL_ON_JOB_CLOSE` 策略使用同一个提交锁，锁内再次核对取消和期限。提交前失败、取消、超时或工具结束沿 #255 路线终止并确认自建目标；提交后取消只停止观测，目标继续独立运行。锁内没有等待远程线程或执行 progress 回调。运行中 attach 的既有目标不会被当作自建未提交目标终止。

## 完整性表达

界面显示启动来源、主线程、入口、协议和就绪时接受／拒绝次数，提示中有实际内容命名 Hook 路径及有限边界。没有收到所选调用时另行解释，不能把空闲、方法关闭、转交既有实例或未知实现等同于完整捕获。

当前段快照沿用共同 JSONL 摘要，新增 `observationMode=startup` 和 `startup.*`：入口边界／排除范围、主线程、映像地址、入口 RVA、协议、就绪能力、Hook 路径及就绪 QPC。实例、初始／当前方法、配置变化、代次、统计和传输缺失仍使用共同段字段。冻结导出后清空或切换轮次不会修改已冻结的启动来源。

能力值只是就绪时已经发现实现的快照。后续 DLL 加载不意味着其中任何 COM 实现都被监控；不同实现必须正确转发或在修改前拒绝。入口前 DLL／TLS、先行辅助线程、未知实现、其他 PID、子进程及转交既有实例不保证覆盖。本模式不会补造 attach 之前的调用，也不自动跟随目标树。

## 可重复证据

原始命令、退出码和日志 SHA 收在 `.scratch/implement-pipeline-2026-11-02/244/final-commands.json`；源码、产物和证据的最终冻结身份由同目录 `receipt.json`、`source-manifest.json`、`artifact-manifest.json` 和 `evidence-manifest.json` 固定。运行环境为 Windows、Visual Studio 18 2026 现有缓存，未以 VS2022 preset 重配。只跑本票直接相关测试，所有票完成后的本地全量回归由总流程归并。

### 先判红及修复

- 改前相关 Debug 基线 77/77，真实 exit 0，见 `baseline-debug.log`。新增产品 API 测试先因接口尚不存在编译失败，见 `red-product-api.log`，exit 1。
- Unicode 名称用例真实判红：目标路径／参数可运行，但段名把宽字符截成单字节，见 `red-unicode-name.log`。随后改为 UTF-8 转换，增加控制路径日志；不改变目标选择或 Hook 路由。
- root 在生产 GUI 发现新增边界提示的“诺”缺字。把全部 17 条新文案加入现有生产字体范围测试，`red-startup-font.log` 唯一缺字为 U+8BFA；修复采用现有字体可显示的“覆盖”表述。原有其他文案缺字不在本票范围。

### 无设备与真实进程功能回归

初版冻结时 x64 Debug／Release 的直接相关集合各 111/111，`StartupLaunchBoundaryTests` 各 4/4，Hook 测试各 48/48，全部真实 exit 0，编译零警告。独立审查后的路径修复又增加一个相关用例及一个系统边界用例：最终两配置各 112/112、5/5，并重新跑五种真实启动探针，均正常退出且编译零警告。直接相关集合包括 `StartupLaunchTest`、`RemoteInjectionTest`、生命周期、远程等待、`OnDemandAttach`、UI 段模型、导出和 GUI 文案；系统边界测试重新覆盖 #255 的异常收尾。保留初版 26 条命令，加路径修复后 16 条，共 42 条成功命令；逐项解析见 `parsed-debug.json`、`parsed-release.json` 和 `parsed-x86.json`。Hook／共享 ABI／传输没有因路径修复变化，其四配置 48 项及传输证据按原身份复用，不重复声称是本次新跑。

产品新增用例核对共同接收／段来源、已有选择不抢、Unicode 名称／原始参数／目录、提交锁等待时取消、产品取消／自然期限下实际退出且不建段，以及启动等待期间旧目标配置、普通批次和单分离可前进。既有逐阶段故障用例核对实际断点恢复、主线程前进、外来挂起不被消耗、目标自主退出、工具强退及未提交目标收尾。

x86 Debug／Release 各运行当前 `WinAudioHookTests` 48/48 和 `HookTransportProbe`，全部 exit 0。各传输探针有两个实际子进程，均 exit 0，布局 10、32 位、CallPod 1048 字节，收到的参数没有无效或重复记录。它们是无音频的合成跨进程协议证据，不能写成 x86 真实启动／注入或音质验证。

Hook 和共享 ABI 本票未改。四配置 Hook 回归包含不同实现 A／B 的独立原函数转发，以及稍后遇到不支持实现时保持其 vtable 不变、报告 `ERROR_NOT_SUPPORTED` 和有限覆盖；不从安装成功推断未知实现全部可用。

### 真实音频与三层就绪

`StartupGateProbe --product` 调用生产 `startStartup`，由共同 receiver 和 Store 接收。目标在 CRT 全局初始化、main 和后续线程各执行 Activate→Initialize→Start→Stop，独立源端标记逐条核对 PID／创建时间／代次、顺序、HRESULT、对象地址和初始化参数。默认选择预期 12 条；`--product-off` 预期 0 条；`--product-initialize` 预期 3 条。三模式源端都实际完成 12 次调用，不把关闭方法当作目标没调用。主动延迟 GUI poll 后仍收到首条；控制器 shutdown 后目标继续，再发停止事件确认真实 exit 0。

`--delayed-readiness` 用三个独立同步门暂停安装、初始配置和 ReceiverReady 放行。前两门内源端 CRT／main 均为 0，mask／generation／capture 均为 0；最后一门内 CRT／main 仍为 0，而 mask=Initialize、generation=1、capture=1。放行后收到唯一首条 Initialize，源端实际四次调用，目标 exit 0。同步门在现有测试目标和控制路径观察回调中，正式 Hook 热路径没有测试开关。

两配置的上述模式均通过。另运行 `--product-boundaries`：观察目标在 main 标记后加载原先未加载的系统 `winhttp.dll`，随后 main／worker 的已知调用仍逐条收到；同时显式创建未附加目标，源端另有 12 次实际音频调用，核其没有 Hook，Store 仍只有原 PID 的一段／12 条。两个目标真实 exit 0。此用例不把系统 DLL 加载扩展为“任意迟到 DLL 中新实现已捕获”；未知 COM 实现的修改前拒绝由独立 Hook 测试证明。

TLS 回调只写安全阶段标记、设置测试环境和加载既有测试门 DLL，没有在 TLS／DllMain 违规调用 COM。真实产品探针核对 TLS 标记先于接收就绪，接收就绪先于 CRT 和首个所选调用；这里只证明所设主线程入口边界，不证明所有先行线程都停在入口后。

### 生产 GUI

root 独占 Windows GUI 操作，按冻结 GUI／Hook／目标身份记录截图、创建时间和真实模块路径。首轮 GUI 身份为 `C14DC0DACE5D20280CD0A6A87211BB265A7ABEA5D3EC29559F9AF3FCFF2958C4`，验证了无 Live session 的独立入口、Unicode／空格 EXE、12 条启动调用和实际唯一 Hook；PID 11756 的保存文件有 1 条段摘要及 12 条调用。该轮含待修的“诺”缺字，其自动目标寿命先于后续查活，不用于证明关闭 GUI 后目标仍存活。

修正缺字后、路径修复前最后一轮 GUI 身份为 `944A54510F8A30E92124DA9D828B23FA80E715A91B98A9CC7FD32FF6F836CBCD`，PID 31440，实际 exit 0。`gui-final/verification.json` 汇总以下有效结果：

- A32604 正在监控、B31488 在入口前同步门等待时，A 的配置成功由 9487231 变为 9487227，导出配置变化数为 1。B 自然超时，实际 exit 1223、源端 0 次调用；不把这一轮写成手动取消成功。
- B28608 等待时可单项分离 A，A 的只读历史保持选中；A 重新附加为新段仍不抢旧选择。之后“全部分离”取消未提交 B8512，B 源端 0 次调用、实际 exit 1223，既有音频 A 继续运行。
- 新成功目标 25440，创建时间 FILETIME `134381664868440154`，源端 12 次调用，新成功不抢已有选择。GUI 正常关闭后立即实际枚举到同一目标／创建时间及唯一匹配 Hook，之后目标 exit 0；A 的回调由关闭前 47601 增至关闭后 48923，最后 exit 0。相关 fixture owner 均真实 exit 0。
- 新文案显示“就绪时接受／拒绝”和“不覆盖”，保留有限入口提示。该轮音频不连续计数初始已有 1，不把它解释为本票造成的音质问题，也不据此声明音质或性能通过。

首轮导出按旧二进制身份复用：两轮之间 `CallLogStore` 导出、`StartupObservation`、Controller 和 GUI 保存路径未修改；没有首轮源码 SHA 清单，不能补造旧源哈希匹配。最终 `OnDemandAttach` 已追加初始 mask／generation 回读，首轮导出不用于证明这项最终握手。当前两配置的导出／快照回归和真实 product 摘要独立验证最终源代码。该最后一轮 GUI 的一次保存点击没有 start／finish 或文件，作为未确认命中排除，不写成导出成功或产品导出失败。截图只证明交互行为，不作为所有切换窗口的严格时序或性能证据。路径修复后 GUI 已重新链接，当前产物 SHA 另见冻结清单；未重跑完整生产 GUI，旧截图／存活／导出证据只按原身份复用，不冒充重链后 GUI 的直接验收。

## 独立审查后的路径修复

独立 Spec 审查发现默认目录使用截断字符串：`E:\foo.exe` 得到 `E:`，而 `foo.exe` 得到 `foo.exe`，违反“默认 EXE 所在目录”。生产修复仅在 `StartupLaunch.cpp`：先用 `GetFullPathNameW` 取得明确绝对 EXE 并记录结果，再用标准库 `parent_path()` 取得默认目录，保留盘符根分隔符；显式工作目录仍原样传入，原始参数不重写。没有改 receiver、Hook、共享协议或 GUI 逻辑。

现有系统边界测试增加一个 `CreateProcessW` 参数观察缝隙，直接调用生产启动入口，核根目录、裸相对文件和相对子目录 EXE 的传入路径／目录／命令行后返回文件不存在，完全不在盘符根创建文件。`path-fix/red-root-relative-boundary.log` 真实记录 `E:`／`foo.exe` 错误。另在自有临时目录复制测试 EXE，真实产品启动先因默认目录错误失败，见 `path-fix/red-real-relative.log`，两份判红均 exit 1。

修复后两个用例转绿。真实启动分别使用裸相对 EXE＋默认目录，以及相对子目录 EXE＋显式 Unicode／空格工作目录；核对实际映像、目标自己记录的命令行／工作目录、同一实例身份和真实 exit 0，未以参数拦截代替相对路径实际运行。两配置完整相关回归中各有两个实际相对目标正常退出。当前两配置的五种产品探针也重新通过，证明原有绝对路径、初始选择和门控／接收功能未回退。

初版 receipt／源码 ZIP／manifest／patch／26 条命令索引按原身份保留在 `pre-path-fix-backup`，首次独立审查报告也保留。新冻结包含 23 个源码文件，比旧版本多纳入一份既有系统边界测试文件；与旧冻结相比只修改生产 `StartupLaunch.cpp`、两份对应测试及本报告。最终累计 patch、ZIP、产物身份和 42 条命令索引均重新冻结，六个保护文件不变。

## 保留的失败准备轮

- 初次延迟就绪探针把 `FILE_MAP_READ` 映射用于需要 CAS 的 `monitorState`，探针真实 exit `0xC0000005`。`delayed-readiness-cdb.log` 定位到原子读取；只把探针映射改为 `FILE_MAP_ALL_ACCESS` 后通过。没有修改生产原子算法；失败日志 `probe-delayed-readiness-debug.log` 保留，不计为产品通过。
- 最早真实产品探针的 PowerShell 准备命令用了 `ErrorActionPreference=Stop`，被原生 stderr 打断；保留 `probe-product-debug-aborted-powershell.log`，不作有效命令或成功证据。后续 runner 直接等待子进程并采集真实退出码。
- 首次 GUI owner 也有相同 PowerShell 准备中断，由 root 单独列为排除轮；有效重试使用不同 owner 和实例，不把旧轮日志覆盖成成功。
- 结果解析器初版假设 stdout 摘要始终独占一行，被 stderr 的时间戳日志插入 JSON 后解析失败，见 `parser-first.log`。修正只在解析视图中移除完整时间戳日志行，保留原始命令日志，随后核验通过；没有修改产品断言或重写运行结果。

## 未覆盖与后续

本票不宣称用户未提供的原崩溃应用已经验证，不宣称任意应用的完整启动历史。没有实现跨位数 helper、保护进程绕过、Detours、自动子进程或失败后无观测放行；x86 真实启动尚未验证。

本票的真实音频用例是功能检查，不是音质或正式性能证明。Release 生产 GUI 三轮预算归 #263，最终导出／批量／生命周期组合归 #246；父级 #237／#247／#252 不由本票关闭。全新独立审查、远端双配置 CI 和 squash 状态由总流程另行记录。
