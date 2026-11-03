# 管线观测组合验收（#246）

状态：本票约定范围内的当前 x64 Debug／Release 功能组合、x86 无设备回归及新生产界面验收通过，可以申请更新交付地图；尚待全新独立双轴审查与后续发布／CI／squash，不能称全部交付已完成。父地图和父规格未改动、未关闭。最终冻结凭据位于 `.scratch/implement-pipeline-2026-11-02/246/receipt.json`，以下路径均以仓库根目录为基准。

## 范围、来源和保护

- 基线／HEAD：`1a8cc745efbda0d3b335b697de17af046d48f89c`，分支 `test/pipeline-integration`。#243、#244、#245、#263 已合入后开始本票。live #246 的完整验收评论及父规格 `docs/superpowers/specs/2026-11-02-pipeline-observation-prd.md` 是判据。
- 本票新增 `PipelineIntegrationProbe.cpp` 和一个探针 CMake 目标，复用既有 `HookAudioProbe.cpp` 中的真实音频目标及生产 Controller／Store，不复制 Hook、传输或状态算法。AttachVolumeProbe另补父进程持有目标句柄时的创建时间记录与核验，重跑受影响四API；旧音频探针源码和映像、生产 GUI、Hook、共享 ABI、音频热路径均未因本票改变。新探针不注册为无设备 CI 用例。
- 新探针的测试专用 TLS 只写独立阶段标记和进入既有 loader gate，不执行 COM 或安装逻辑；CRT/main 标记、门事件、生产启动状态与QPC共同证明等待。公共 `StartupRequest.timeoutMs=90000` 只用于该受控夹具；生产 GUI 默认仍为15秒。门内不增加日志框架调用；控制器与探针控制路径按现有模式记日志，独立共享标记保留阶段时间。
- 保留用户原有三个 dirty 文件和原 untracked。六项保护沿 `protected-after-243.json`，最终凭据逐项核SHA；未改 AGENTS、CLAUDE，未自行 commit／push／merge。当前只运行直接相关检查，全票结束后由主代理统一执行全量双配置。
- 沿用 Visual Studio 18 2026 的 `build`／`build-x86` 缓存，两配置顺序构建，没有切 VS2022 preset。设备为默认 render 端点 `{0.0.0.00000000}.{46f4256f-bc60-4865-b5f3-4c6eef7ee966}`，本轮实际音频为48kHz、双声道、32bit；原始格式字段、实际模块路径、PID＋创建时间见逐命令日志和冻结清单。

## 四层证据

| 层次 | 本轮证据 | 判定范围 |
| --- | --- | --- |
| 无设备行为 | x64 相关状态、存储、导出、身份和启动测试；独立进程执行 Hook ABI／原调用回归；x86 两配置另列 | 确定性地验证在途调用、PID复用、未知实现、完整提交、历史、失败与统计分列；不外推硬件表现 |
| 真实跨进程合成 | 当前 HookTransportProbe、SegmentWindowProbe；#263 正式压力／过载／内存／GUI计量按身份复用 | 真实共享映射与生产接收器；源调用仍是合成入口，不能称真实WASAPI音频 |
| 真实注入／音频 | 当前四API附加前后对照、双目标逐方法回路、启动第一条及新增 A/B/C/D 组合 | 自有目标实际调用 Windows render／端点loopback；记录实际退出，不把存活或非空日志当通过 |
| 生产界面 | root独占 node_repl＋sky 操作新生产GUI，33张新截图、六份新导出、三既有目标和成功启动B的实际模块快照 | 详情／滚动、独立选择、历史和导出通过；GUI退出后A/C/B原调用及帧推进，随后全部目标真实退出 |

## 自动回归与构建

采前两配置直接相关基线各137/137，日志为 `baseline-{debug,release}.log`。扩展到本票所有相关存储／图来源检查后的当前回归如下，命令和真实退出保存于 `commands-x64-{debug,release}.json`，单项日志同名；构建为增量相关目标构建，不称全新全量构建。

| 检查 | x64 Debug | x64 Release |
| --- | --- | --- |
| 相关 Controller／Store／Export／Graph／Startup 等13套件 | 161/161 | 161/161 |
| HookBuffer／Services／Forwarding／MethodSelection／Publication／Transport，CTest逐例独立进程 | 49/49 | 49/49 |
| StartupLaunchBoundary 真实安装在途资源与故障监督 | 5/5 | 5/5 |
| 四API，attached／unattached各4个独立目标 | 8个父探针及目标exit=0 | 同左 |
| StartupGateProbe 五种入口／配置／边界模式 | 全部exit=0 | 同左 |
| HookAudioProbe --batch | exit=0，双真实目标exit=0 | 同左 |
| HookTransportProbe／SegmentWindowProbe | 均exit=0 | 同左 |
| WAIT_FAILED 原系统错误保留，独立单项 | 1/1 | 1/1 |

x86 Debug／Release各独立执行Hook用例49/49、协议／身份／窗口32/32，HookTransportProbe真实跨进程合成均exit=0；相关增量构建零警告／错误。没有运行x86实际音频注入或启动，不用这些无设备数字替代真实覆盖。

`run-validation.py` 保存准确命令、退出、原始日志散列；原生stdout/stderr直接写字节文件，避免PowerShell把stderr转为错误对象后折行。最早一轮探针的PowerShell包装原件仍保留，没有用于逐字段验收。最终Debug探针首次真实exit0，但同时写入一个文件的stdout/stderr在第四条实际模块路径中间交错，独立解析器因此exit1拒绝四模块判定。`integration-frozen-x64-debug.log`和`verify-frozen-x64-debug.log`及命令原件保留；改为分别保存原始字节流，仅补跑受影响新组合，未重跑已通过的Debug四API。`integration-verified-x64-{debug,release}`两配置随后均真实exit0且独立解析exit0，原件保留为修前冻结证据；审查后最终当前映像采用下述`integration-standards-p2-x64-{debug,release}`。合并日志按完整stdout再stderr排列，不冒充跨流时间顺序。`commands-frozen.json`保留早期失败，最终凭据把它列为采集／解析失败，不计入最终成功运行。

独立Standards审查指出新增探针控制路径等待漏记真实wait值／系统错误，且`finish()`把WAIT_FAILED误译为超时。先以无效句柄调用实际`IntegrationGate::finish`复现：真实wait=4294967295，修前日志为0x800705B4，检查exit81；修正后同路径记录WAIT_FAILED、result=4294967295、error=6、0x80070006，检查exit0。夹具仅重命名完整真实源码入口，未复制等待算法，不创建音频或注入目标。修正只为新探针的非TLS阻塞控制等待补日志并即时保存GetLastError，成功／失败判定、TLS和0ms轮询未变；没有改既有音频探针或生产实现。

上一冻结凭据SHA `E388EBF9D2F30C93416FAA50183C3D7461C6241AEFC1BE5A693AF2E5F1F3CB6B`、三份manifest、源码ZIP、补丁、冻结脚本和双配置旧探针映像保存在`prior-freeze/`。修前探针源码SHA为 `C71083EBFFB77CA9349E4A453FA4AD1591408AF336FE0E7832A579C8DDFB26BB`，修后为 `D25F56A2DF40AD03BF66EC11D6559DEEF006ACF00FDDBF542A93A266F91DFEFC`。只增量重建新探针Debug／Release，各零警告／错误，并只重跑受影响两组合及独立解析，均exit0；`commands-standards-p2.json`列准确命令、源流散列与双配置新映像SHA。其余34项产物保持原散列，Volume四API、GUI及正式性能证据继续按匹配身份使用，没有重复运行。

Hook的49项用CTest分进程执行，保留恢复失败类用例的隔离需求。包含全部SDK方法关闭时原调用恰一次与零记录准备、S_OK／S_FALSE／失败及有效空输出、音量／静音／效果通知六槽、不同实现、并发安装、四生产者跨容量、慢写者、回绕、未知统计、在途跨清空／重新附加等当前行为。不是把SDK声明改写成另一套槽位表来测试副本。

当前两配置SegmentWindowProbe各有两个真实合成源，每PID完整收到10013条、核最近1024条（序号8990→10013）、独立保留1条控制；源完成数10013、有意失败910，API失败与音频不连续及传输覆盖／拒写／未知各列，不以窗口淘汰数推算记录缺失。初始统计零、结束历史冻结及新段空窗口均通过，真实源exit=0。HookTransportProbe每进程四写者合计向控制／pump各尝试16384条，保留实际覆盖而invalid／duplicate为0；其JSON明确不含慢写者／回绕／换代场景，这三项由当前Hook隔离行为用例证明，不冒称该跨进程压力运行覆盖它们。

## 当前四API和启动首条

四API分别为 GetMasterVolume、SetMasterVolume、GetMute、SetMute。每配置各以 `AttachVolumeProbe --method N --no-attach` 和 `--method N` 创建独立自有播放目标。结果分别为0.625、设置后读回0.500、FALSE、设置后读回TRUE；HRESULT与适用读回S_OK、行为匹配、附加后匹配方法记录正确。16个真实子目标均正常exit=0，放行前核验唯一实际Hook内容身份。效果接口的真实硬件能力没有外推。

旧修复前访问异常及SDK两用例判红仍指向 #243 报告：`.scratch/attach-volume-cdb-confirmed.log`（`HookMuteGet`、0xC0000005）及 `.scratch/attach-slot-fix/red-sdk-tests.txt`。它们是缺陷来源证据，不是本轮通过数字，也未为重现而覆盖当前产品或用户工作。

StartupGateProbe 本轮两配置执行 `--delayed-readiness`、`--product`、`--product-off`、`--product-initialize`、`--product-boundaries`。独立目标在入口后CRT、main和后续线程各发起 Activate／Initialize／Start／Stop；默认12条、全关0条、仅Initialize3条，与源PID／创建时间、顺序、stream地址、参数及HRESULT逐项核对。延迟就绪模式在安装、初始配置、接收交接三处保持入口，独立共享标记证明CRT/main尚未进入，初始mask先为0、发布后仅所选Initialize且generation=1，再放行取得准确首条。

边界模式验证入口前TLS标记、迟到winhttp.dll、未观测的另一个真实PID不加载Hook；未知实现由修改前拒绝用例覆盖。未声称任意迟到DLL的新COM实现都可捕获，也未声称转交既有实例或其他进程属于当前启动PID。

## A活动、B启动等待、C/D同批真实区间

新增探针启动三个已有真实音频目标A/C/D，另以生产启动入口创建持续音频目标B。B进入测试专用TLS loader门时已有PID和独立TLS时间，尚未执行CRT/main或创建音频会话；生产状态仍未提交。C/D在同一冻结批次先成功，随后一个真实loader阻塞项保留批次进行状态；另有排队退出、自进程拒绝、A的排队项及重复项。A在这个共同区间内持续接收、修改方法并单独分离。

独立QPC频率、A/B/批次全部起止、共同交集以及交集内两点源／接收增量保存在最终日志。取A增量基线时，C/D已经成功且loader仍pending，两个样本都位于B等待＋批次进行的共同区间。分离确认时再核B的TLS=1、CRT/main=0、批次仍进行。不能把三段独立运行拼成并行结果。

| 最终当前映像 | 三项交集 | 交集内已采样跨度 | A接收／源调用 | C／D累计窗口接收 | C／D保留窗口；控制 |
| --- | --- | --- | --- | --- | --- |
| x64 Debug | 0.4575411秒 | 0.2685296秒 | 38→91；244→352 | 4243／4218 | 各1024；332／328 |
| x64 Release | 0.4237531秒 | 0.2662559秒 | 34→86；224→332 | 4243／4214 | 各1024；各330 |

两配置各六个混合批次项状态依次为成功、成功、待收尾、目标已变化、拒绝、取消；B独立TLS／CRT／main及提交标记按顺序变化。最终C冻结文件Debug为1356条（332控制＋1024窗口）、Release为1354条（330＋1024），跨清空后再次序列化全部字节相同；四音频实际唯一Hook路径和内容SHA、PID／创建时间、各段方法／代次与统计、关Controller后各自调用／帧增量、四音频及两gate真实退出均由独立解析器核验。

本轮首版夹具把“A分离会取消它尚未发起的同批项”误期望为成功，最终exit=81。实际产品按契约取消、C/D成功未回滚，所有自有目标正常退出。保留 `integration-debug.log`／`.exit`，修正夹具预期后再显式重附加A；这是夹具预期修正，不是产品缺陷修复。随后补齐独立阶段、QPC和各段窗口日志，最终两配置运行以凭据所列新源码／映像为准；中间PASS原件保留但不冒充最终身份。

批量核验C/D成功、loader超时待收尾、排队目标真实退出后TargetChanged、自进程Rejected、单分离取消尚未开始A项。释放loader门后生产pending归零，未迟到开启该目标；另一个批次对活动C重复选择去重为一项并Skipped。A重附加沿用最后成功方法，旧段仍只读且选择停留，成功B和新批次不抢当前历史查看对象。

A与B的逐方法mask不同；C/D各完整接收超过1024，窗口各保留1024，累计量和淘汰继续增加，控制调用独立保留。冻结C后切D、继续接收和清空C，C新代次记录继续、D累计接收增长；清空前记录及统计不回流。`frozen-before.jsonl`／`frozen-after.jsonl`按全部字节核同一冻结快照，JSONL再由独立解析器核身份、记录数、窗口／控制、统计和缺失字段。

关闭Controller后，再等待300ms；A/C/D三个既有目标及已提交启动B都保持同实例存活，原调用和真实采集帧均增加。之后通过自有stop通知正常退出，四目标及两个gate目标实际exit均0。每目标各一次源启动不连续如实保留；源启动不连续和本段API失败／transport缺失不是同一统计，不能把它们相减或混称。本功能回路不替代 #263 四态正式音频配对。

## 历史、导出、清空与所有权

本轮生产GUI由root独占 node_repl＋sky 操作，PID31380、创建FILETIME `134381799277854442`；映像SHA256为 `1D407AE8480E5E54FA375ABD593543EBFD079E4F9645A3E85450B06A447C7B42`。A/C/D为新建既有音频目标，B由生产启动按钮成功创建；四目标各唯一加载布局10、内容SHA `3985BDE67DF317AEA4036A43BA00603282F72F8D0B9DC95AA2934C119E444617` 的Hook。原始模块快照和GUI-ready源码／映像清单保留，不用文件名推断实际加载版本。

| GUI目标 | PID | 创建FILETIME | 已导出段／token／代次 |
| --- | --- | --- | --- |
| A | 30924 | 134381797929698614 | 1／1／1；重附加后4／4／3 |
| C | 5132 | 134381797931291352 | 2／2／1 |
| D | 6884 | 134381797932223848 | 退出并清空后3／3／1 |
| 成功启动B | 16856 | 134381813062821099 | 5／5／1 |

新截图记录1024窗口及长距离滚动、buffer参数、C图与A调用日志独立选择、A单分离后选择停留、同实例重附加新段与配置继承、A清空完成反馈及gen3增长、C启用render方法、B启动成功及Initialize深层参数。清空成功使用图16／17，图14／15点击未命中；深层参数成功使用图23，图22滚动未聚焦，这些未命中不计成功。B准备期间C/D同批成功，A接收从63037增至64184且查看选择没有被成功项抢走；人工窗口内的运行截图与新探针的精确三项区间分别保存。首次门控B01在15秒默认期限自然超时，图07文件名虽含cancel，不能称GUI主动取消；其未提交目标23660最终确认exit1223，夹具owner真实exit0。主动取消的完整启动阶段由当前确定性用例覆盖。

生产按钮共取得六份新JSONL，独立逐行解析实例／段／token／代次、配置变化、冻结时间、统计与各层缺失／未知、控制／窗口上界及结构化参数；归档至本票 `gui/exports/`。A初次4686条（3662控制＋1024窗口），隐藏最近窗口后5104条（4080＋1024）；B设置无匹配显示筛选仍导出1546条控制调用及启动来源；C无匹配筛选仍导出10280条（9256＋1024）；A重附加清空后gen3导出7942条（6918＋1024）。A/C方法mask分别为9488255／9491327，B为9487231；导出保留各自配置，未把显示筛选当存储筛选。B首条Initialize导出的48000Hz、双声道、32bit、每秒384000字节、blockAlign8、cbSize22及S_OK前后值与详情一致，启动入口地址、RVA、就绪QPC、协议及能力边界同时保留。

D主动退出后旧段保持选择，再清空只读历史；生产保存按钮仍可写出一行纯摘要，calls=0、控制／窗口=0、state=exited。统计明确为unknown、transport两路unknown，不能把它称为GUI已构造非零统计源；非零统计且无调用的所有权由下列确定性用例另证。初次A文件在后续接收、切换、分离、重附加、清空和新增导出后SHA仍为 `9323F2A8DEBA8712923F27D708D9B361072CD125AD46E52B8B6D994F3BFB6C1C`。人工操作可能发生于写线程发布之后；真正writer仍在途时的并发修改由阻塞writer用例证明，不用新GUI文件稳定外推人工时序。

生产GUI正常Alt+F4退出，由提前打开并持续持有的真实进程句柄确认wait=0、退出码0。约23秒前后样本中，A原调用1028094→1038634、帧108273120→109382880；C原调用1028064→1038604、帧108269760→109379520；成功启动B原调用338090→348644、帧35629920→36741120，PID与创建时间一致，API失败0且各启动不连续仍为1。D此前已经主动退出，不要求退出后推进。随后两个音频owner协作stop并真实wait均exit0，四音频目标真实exit均0，B01门owner也已正常退出；不把关闭句柄当远程线程结束。`gui/verification.json`内嵌33图散列、六份导出解析、四模块、前后增量和真实退出，人工结论／动作／模块／前后原始文件复制为无root前缀的稳定证据。初次A导出在关闭GUI后再次核SHA不变。

本轮GUI没有执行全部分离／最后目标结束后再次附加，也没有覆盖各启动阶段主动取消；这些边界由当前确定性及真实进程用例证明，不称GUI覆盖。全部远程资源与未提交目标收尾仍以对应进程／线程断言为准。

当前确定性用例补足不能靠人工时序证明的边界：

- `ImmediateClearFeedbackIsDeliveredOnceWithoutReplayingOldQueuedData`、`FastFeedbackPreservesLifecycleBarriersAndFinalSnapshots`、`ControllerFeedbackDeliversCommandFailuresOnceWithoutOrdinaryPolling`、`SuccessfulClearRejectsQueuedOldRowsAndStatisticsWithoutChangingSelection`：快速反馈恰一次、不越过Attached/Detached、旧transport／调用／统计不回流、新代第一条不丢。反馈先到不等于普通批次已交接。
- Hook同步事件把原调用卡在真实返回前，验证进入时方法快照、在途动态配置、旧代／旧段不能进入清空后或重附加段。PID复用使用确定性创建时间替身，不依赖操作系统碰巧重用PID。
- `SummaryOnlyExportOwnsStatisticsAndConfigurationAcrossClearAndReattach`、`InFlightSegmentKeepsCallsWindowAndSummaryWhenSelectionAndStoreChange`及真实JSONL／CSV文件用例：冻结引用独立，显示筛选不影响导出；非零统计而无逐条调用仍能导出，写失败不删历史。GUI纯摘要文件与此确定性非零统计场景分别报告。
- 单目标／全部分离、目标主动退出、同实例重附加、最后目标后再次附加均保留旧段；新段从空窗口／零统计开始、旧段只读、配置按实例继承，图不拿旧实例／旧段Initialize填当前空段。
- StartupLaunchTest当前复验Created、EntryHeld、Preparing、ReceiverReady、EntryReleased、EntryAdvanced、DebugDetached各未提交阶段的取消、超时、目标退出及控制方故障；提交切换窗口和原断点／外来挂起也有真实进程断言。StartupLaunchBoundary额外观察实际远程参数分配、loader仍在途、目标退出及pending归零，未以关闭句柄冒充远程完成。

## 性能证据复用

不重复一小时正式测量。#263固定正式身份、13个调度项及其原始数据保留，receipt SHA256为 `A89DABAAA4850DA4A6D62A872FDB6D3D9871D8B6D63D2A573BB40B142B1FC736`；完整数字见 `docs/measurements/2026-11-03-pipeline-performance.md`。

本票重新比较 #263 当前artifact清单14个文件，均与该冻结清单一致；包括原微基准、真实音频、PerformanceGui与Hook。相对正式计时源码只有 #263已说明的三项采后差异：verify及其测试、SegmentWindowTiming计时结束后的退出查询日志。新组合探针为新增映像，不声称参加旧计时；生产GUI也不冒充PerformanceGui映像。`performance-reuse.json`保存逐文件比较、正式调度清单和历史receipt散列。

复用双PID各≥2000/s三轮、A≥20000/s过载三轮及独立停收、600秒稳态、三轮GUI帧／七类操作、48组微基准与24组真实音频四态配对。既有普通压力少量传输覆盖、过载A大量覆盖／拒写、完整帧和单调用最大长尾全部继续保留，不改阈值、不合并轮次、不删异常。复用证明范围仍以原报告的实际计时入口、采样完整性和未覆盖字段为限。

## 交付状态与未覆盖

本票四个任务文件、补丁、源码ZIP、源码／产物／原始证据清单、准确命令及日志散列、六保护与以上解析结果统一冻结。允许申请更新交付地图，不自动关闭父票。最终独立双轴审查、提交推送、当前HEAD双CI与squash由root继续；全部15票结束后的全量双配置回归仍未在本票提前执行。

未知用户应用未提供且不再索取，不能宣称其所有崩溃／启动行为都已解决。真实效果硬件、x86实际注入／启动、普通第三方应用启动、任意COM实现和所有入口前DLL/TLS音频均未扩大覆盖。自有静音render与loopback证明原API及音频帧推进，不证明可听音质。父issue #237／#247／#252状态不在本票修改范围。
