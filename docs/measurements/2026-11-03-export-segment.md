# 当前段冻结导出验证（#262）

本票在 `98a400aa68ba450e827c0eba182e32dcb91d09a7` 上实现当前查看段的导出。冻结内容包含该段已接收的全部保留控制调用、最近窗口和段摘要，不受旧录制范围、调用筛选和窗口显示开关限制。活动接收、切换、退出、重附加及清空均不修改已启动快照。独立审查发现的两项 P2 已修复，修复后的两配置回归、文件解析及新 GUI 补充验收通过；原版本的完整 GUI 场景单独保留，不冒充新产物结果。仍待独立复审、发布与当前 HEAD 的双配置 CI。

## 实现与格式

`CallLogStore::freezeExport(attach, frozenMs)` 复制同一 `Segment` 值和不可变调用的拥有型引用，没有第二套活动记录模型。导出线程仅持有冻结数据。生产按钮改为“保存当前段”，空调用列表仍可保存段信息、未知覆盖或源端统计。

JSONL 首行为 `rowType=segment`、`schemaVersion=2`，后续为 `rowType=call`。调用保留既有字段和结构化 `parameters`，并正确标记控制／窗口通道及实例、段、序号和观测代次。CSV 保留原十列及平面 `args`，追加身份和行类型；摘要使用 `summaryKey/summaryValue` 平面键值。完整格式在 [README](../../README.md) 的 Pipeline 说明中同步维护。

段摘要保留身份、状态、初始与当前配置、宿主实际观察到的成功配置变化、记录统计起点、记录各自时点、接收方传输快照时点、窗口移除和已知／未知缺失。`segmentSnapshotMs` 是段建立或结束的宿主时点；活动段的最新传输／统计快照以 `transport.snapshotMs` 为准。导出点击、调用时间和统计刷新不是跨线程同一瞬间。配置成功事件时间也不冒充目标原子发布的精确墙钟。最近窗口明确不是完整高频历史。

沿用 `CallLogExportJob` 的后台 worker、忙状态、取消、退出等待、同目录临时文件、拒绝覆盖及原子发布。摘要失败与取消同样清理临时内容并保留内存。

## 先红后绿

证据目录为 `.scratch/implement-pipeline-2026-11-02/262`；命令记录包含实际退出码和日志 SHA256，原始日志保留 BOM 编码。

| 场景 | 红 | 绿 |
| --- | --- | --- |
| 当前段排除其他 PID／同实例旧段，仍含隐藏窗口且不受旧录制范围约束 | `red1.log`，exit 1 | `green1.log`，1/1，exit 0 |
| 无调用、已有非零统计／缺失也能导出；配置与统计拥有型冻结 | `red2.log`，exit 1 | `green2.log`，43/43，exit 0 |
| 动态配置与附加处于同一毫秒仍记录独立变化 | `red3.log`，exit 1 | `green3.log`，61/61，exit 0 |
| 同批配置 gen1 后清空 gen2，不改写配置事件的真实代次 | `red4.log`，exit 1 | `green4.log`，1/1，exit 0 |
| 点击保存纳入控制器已排队调用、统计、配置及结束事件 | `red5.log`，exit 1 | `green5.log`，含上述两项 2/2，exit 0 |

开工前现有 Debug／Release 可执行文件的直接基线各 41/41；未提前记录这两份基线文件的散列，不把它们说成新源码构建证明。中间 `files-debug.log` 在构建尚未结束时使用了前一测试产物，不用于最终通过声明；最终以 `final-state-debug.log` 为准。解析器首次把默认方法掩码手填错误，保留为 `parse-debug-initial.log`，不是产品缺陷或产品判红证据；修正预期为方法清单对应的 `9487231` 后重跑通过。

## 独立审查修正

首轮收据、报告、源码及 GUI 证据、8 份产物完整备份为 `backup-before-review-fixes/prior-delivery.zip`，SHA256 为 `3AD47865D9D53A6619C4718317A1775D000B39B832C26EAF40BB55453194B386`，不混入最终证据清单。

第一项问题是保存直接冻结 Store，而接收控制器中已排队的结果可能仍未经过 100 ms 展示交接。现由生产保存入口和组合回归共用 `SegmentSelection::freezeExport`：先记住点击时选择，消费现有本地接收队列，再冻结该段；复用 AppUi 原有事件状态处理，不等待目标产生新数据，也不更换选择。普通展示仍使用原有最多 10 Hz 节拍，显式保存交接不调用或推进该计时门槛。

组合回归通过实际 `PipelineAttachController`、自有无音频目标、共享传输、UI 模型、Store 和真实 `CallLogExportJob` 写盘。先把控制和窗口调用、累计统计、配置变化和结束事件排入控制器队列，确认 Store 仍是旧的 monitoring／0 调用状态；展示节拍拒绝 1050 ms 刷新时，保存边界仍纳入队列并输出 detached、两条调用、统计完成数 1 及新配置。随后节拍仍拒绝 1050 ms、接受 1100 ms，当前段选择未被另一个段抢走。受控生产者不代表真实音频或性能。

第二项问题是同批 `Configured(gen1) → Cleared(gen2)` 先完成清空后，配置变化误取了 Store 的 gen2。现将成功事件自己的 `observationGeneration` 传入配置历史，既有直接 Store 调用仍可使用当前代次。模型＋导出回归核对当前段 gen2、变化 gen1、变化时点 1100，并经 worker 输出对应字段。

## 最终直接验证

两配置的真实组合输出又经独立 JSON 解析，核对两条调用的身份、通道、方法、序号、HRESULT，以及 97 项摘要中的新配置、事件代次、累计数和 detached 状态，详见 `post-review-received-parse.json`。

| 配置 | 构建 | 状态回归 | 配置控制事件 | 独立文件解析 |
| --- | --- | --- | --- | --- |
| x64 Debug | Tests 与生产 GUI，exit 0 | 71/71，exit 0 | 3/3，exit 0 | 三组 JSONL＋CSV 通过，exit 0 |
| x64 Release | Tests 与生产 GUI，exit 0 | 71/71，exit 0 | 3/3，exit 0 | 三组 JSONL＋CSV 通过，exit 0 |

状态回归为 `CallLogStore.*:CallLogExport.*:CallLogFilter.*:PipelineAttachUiModel.*:GuiLanguageText.*`，修复后正向注册并实际执行共 71 项。接收组合为 `RemoteInjectionTest.MethodSelectionPublishesWithoutAnApiAndRetainsOnlySuccessfulInstanceState`、`ClearPublishesANewGenerationPreservesMethodsAndRejectsQueuedSourceData`、`ExportConsumesAlreadyQueuedControllerBatchOutsidePresentationCadence` 三项，使用自有无音频目标，不能外推真实音频或性能。首轮修正前各为 70＋1 项，只是旧版本结果；最终命令使用 `post-review-*` 日志。

确定性用例在 worker 首次写入阻塞期间继续接收、移走窗口记录、切换选择、清空、退出、重附加和清空存储，最终仍只写原始两条调用及原始段摘要。另覆盖无调用的非零统计摘要、open／write／flush／close／publish 五阶段失败、清理失败、取消不发布和真实目标文件拒绝覆盖。保留已有退出等待／取消用例。

独立 `verify-files.py` 使用 Python 标准库 `json` 和 `csv`，预期由固定输入与规格手算，未调用产品序列化生成预期。每配置核验：

- 完整段：1 条控制调用＋1024 条窗口，逐行核对 24702 个 JSON 字段与 22500 个 CSV 单元；摘要含 100 个字段、两次配置变化、窗口 1030 条已接收／6 条移除；序号 7～1030 的窗口全部核对。
- 仅未知摘要：0 调用、94 个摘要字段，未知覆盖保持未知。
- 仅非零统计：0 调用、94 个摘要字段，完成 2000／失败 17／不连续 3，可靠性未知单列。
- 逐字段核对数字／布尔类型、调用身份、HRESULT、可选字段、JSON 参数树、CSV 空值、引号、逗号和多行转义；两配置六种文件的 SHA256 完全一致。

摘要不是将累计调用数减去窗口条数得到的缺失估计。pending 是当前量，overwritten／writeRejected／controllerDropped 是各自已确认下界，不合并为伪造的精确零缺失。

## 真实 GUI

由主代理单独控制生产 GUI，避免并行 UI 操作。以下完整双 PID 场景属于首轮 Debug `WinAudioGui.exe`，SHA256 为 `E6AB88DEBE950B9B0B4780B5C60A126EBC4CDFA257F0C92C5E9BC287C7D3F231`；该产物和证据已备份。两项审查修复后的 Debug GUI 为 `66DCFD65C2AD4D40C4E0423C0EF8E8C890F4DB156E23CF9F7726A978E8F161F6`，详情见更新后的 `gui-ready-artifacts.json`。

主代理以 PID 19240 的生产 GUI 验收，自有 A（27824，段 1）开启采集 GetBuffer，B（27488，段 2）开启播放 GetBuffer。两目标实际加载的 Hook 均为恰一份 layout 10，SHA256 为 `3985BDE67DF317AEA4036A43BA00603282F72F8D0B9DC95AA2934C119E444617`。

A 关闭“显示最近调用”和旧“录制调用”，输入不匹配筛选 `NO_MATCH_EXPORT_262` 后，真实按钮仍保存全部保留内容：810 条控制调用＋1024 条采集窗口调用，97 项摘要。独立解析确认只有 A 的实例／段／代次；后台接收继续增长。切换 B 后按钮另存 1552 条控制调用＋1024 条播放窗口调用，身份全部属于 B；A 原文件未变。

A 经自有夹具正常退出 0 后成为只读历史。清空该段，统计起点变为 12:46:36，窗口／接收／移除均为 0，传输为未知且无快照。按钮仍可用，生成 4269 字节、97 摘要字段且 0 调用的文件，保留实例、段、已退出状态、代次和配置。此场景未动态关闭 A 的窗口方法；已退出目标不会继续产生控制调用。A／B 两份原文件在切换及清空后 SHA256 均未改变。

| 真实文件 | 内容 | SHA256 |
| --- | --- | --- |
| `gui-evidence/a-export.jsonl` | A 段 1，1834 调用 | `BCE64095474AD97E27AC503F513AF8453EA12EE8C7F1DCE4B79C14CE3E928CC7` |
| `gui-evidence/b-export.jsonl` | B 段 2，2576 调用 | `FDBE69D3DA544288388AEA84CBFBB6DDD4ABCE95FB1FD87342D83D06D39D87A7` |
| `gui-evidence/empty-export.jsonl` | A 已退出段清空后纯摘要 | `A182398EF0D48F224BB2936D4E81978C943820AAD2E68D4CD14DD112BDD9751A` |

真实文件解析覆盖各条身份、序号、调用字段、参数结构、配置变化、记录时间范围、统计时点与缺失字段；A 核对 35656 个调用字段、3240 个参数，B 核对 50496 个调用字段、6208 个参数。完整源文件路径、摘要及校验记录见证据目录 `gui-evidence/a-parse.json`、`b-parse.json`、`empty-parse.json`。

GUI Alt+F4 正常退出，真实退出码 0。B 在 GUI 退出后 callbacks 从 46628 增至 47471，frames 从 34983840 增至 35616000；随后自有夹具正常停止，A／B 最终退出码均为 0，helper 和全部自有进程已结束。总核为 `gui-evidence/verification.json`，截图与过程说明见 `gui-evidence/summary-final.md`。

严格的在途 worker 冻结与五阶段失败由确定性回归证明，不以过快 GUI 文件保存冒充覆盖。生产 GUI 没有可输入的目标路径，未修改用户目录权限制造错误；现有仅测试 writer 不冒充真实磁盘故障。

修复后的 Debug GUI（上述 `66DCFD65...`，PID 23452）完成新的真实按钮与文件快照验收。自有 A（17108，创建时间 134381554858571271，段 1）动态开启 capture GetBuffer，关闭显示窗口和旧录制，筛选 `NO_MATCH_EXPORT_262_FINAL`，真实按钮发布 1408 条调用：384 控制＋1024 窗口，97 摘要字段。源端累计与接收 4460，窗口移除 3436；实例、配置初始值／修改值／gen1、实际观测时点、统计和缺失摘要严格解析通过。文件 680274 字节，SHA256 为 `EDD9766086C73963A0A6A9C60CFC8890E641C4B94E277EDEA0B543A41AC34D87`，原文和解析见 `gui-review-fix/final-export.jsonl`、`final-parse.json`。目标实际模块另经只读检查，恰一份匹配 layout 10 和冻结 SHA 的 Hook，见 `a-loaded-hook.json`。

新 GUI 正常退出，真实退出码 0。退出后的两个连续目标样本显示 A callbacks 27736→27753、frames 20834400→20847360；B callbacks 27731→27748、frames 20831040→20844000，随后双目标均正常停止、真实退出 0，全部自有进程结束。补验总核与过程为 `gui-review-fix/verification.json`、`summary-final.md`。

最终产物此轮补验真实按钮和文件；首轮双 PID 隔离及空历史摘要场景属于旧 GUI SHA，保留其既有证明范围。严格的已收排队和同批配置／清空时序由修正后的确定性组合回归证明，不宣称 GUI 操作碰巧命中了这两个时序。

## 边界与交付

未改 Hook 实现、共享 ABI 或音频热路径，未重跑 x86、音频压力和本地全量；按用户授权在全部 15 票结束后再统一全量两配置。未新增整轮导出入口、数据库、落盘历史或配置持久化。未改六份保护文件及用户其他未跟踪文件。当前未提交、推送或创建 PR，后续审查、发布、CI 和 squash 由主代理执行。
