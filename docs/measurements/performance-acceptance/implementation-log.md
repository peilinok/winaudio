# 性能优化实施记录

依据：[实施规格 #219](https://github.com/peilinok/winaudio/issues/219) 及 #220～#231。

起点：`2483b0cbe7c07bfaba1e9f6d576a203aa03db3ee`，当前分支 `main`。只提交本轮文件，保留原有 `.archify/`、`_shots/`、`shots/`。不推送、不发布版本。

## 执行顺序与状态

| 任务 | 状态 | 证据 |
| --- | --- | --- |
| #220 GUI 计量基础 | 本地实施与验收完成 | 20/20（含真实 ImGui 页签）；汇总工具 5/5；真实 GUI 三次切页、完整日志校验、三模式干扰对照，见 meter-report.md |
| #221 图表基线 | 本地实施与基线完成；远端 CI 未运行 | 双／单八声道各三次、完整负载干扰对照、冻结／缩放／滚回／恢复检查及原始归档；见 charts-baseline-report.md |
| #222 Inspector 基线 | 工具完成，固定数据与真实双 PID 短检查通过；正式基线待采 | 生成器红绿回归；Debug／Release 各 52 项相关测试；完整场景、十分钟内存及真实会话待采 |
| #223 波形刷新与缓存 | 待开始 | |
| #224 声谱图纹理归属 | 待开始 | |
| #225 控制历史与观察轮次 | 待开始 | |
| #226 查询索引与可见行 | 待开始 | |
| #227 图摘要缓存 | 待开始 | |
| #228 后台导出 | 待开始 | |
| #229 Hook 关闭快速路径 | 本地实施与独立验收完成；远端 CI 未运行 | 八项行为测试；x64／x86 三组配对；真实音频前后三组均 PASS；两轴复核无未关闭发现；见 hook-report.md |
| #230 图表验收 | 待开始 | |
| #231 Inspector 验收 | 待开始 | |

## 已确认的实施规则

沿用规格的测试入口：FrameProbe、Scope Reader → Chart Data Pipeline → WaveEnvelope、Store 及图摘要、ExportJob 的文件系统边界、生产四个 Hook、生产纹理持有／选择 → DX11 WARP。逐个行为先失败再修复；每批运行直接相关测试与编译，最后运行完整测试和规范／规格两轴评审。

图表与 Inspector 分别验收，Hook 单独验证目标开销；测量条件、原始数据、失败及尚未验证项分开记录。达到预算停止扩大，不自动加入延期的大改造。

## 问题与处理

### Inspector 固定负载和真实来源分别验证

PerformanceCalls 初始空实现使“应返回 10000 条”的回归明确失败（performance-calls-red.xml）。实现确定性生成器后通过：每条 16 个结构化参数，含深度 0／1／2、枚举、指针、HRESULT 和多行 Pre／Post；双 PID 交替，方法按各 PID 本地序号重复十种控制方法，三个 streamId，固定迟到／同时间戳和 HRESULT 分布。输入在帧计量前生成、序列化并散列，复测使用同一数据。Debug／Release 的固定数据、匹配、Store、FrameProbe 和 Attach 集合共 52 项相关测试通过；Python 汇总 6 项通过。

正式优化前工具保留旧的全量复制、排序、行提交和同步保存算法。三秒预热的短烟测不是正式基线，曾得到一万条 p95 95.9934 ms、操作 p95 225.5906 ms，排序切片 p95 54.6417 ms，明确显示待优化成本，不能用作三轮性能结论。

HookAudioProbe 增加独立 continuous 模式：两个新启动、同位数、真实静音 render＋endpoint loopback 目标由 GUI 的既有 Attach 路径观察；每 400 个目标回调执行一组控制方法，避免为十分钟观察故意制造超出一万条的控制负载。旧七阶段 Hook 验收模式保持每 20 回调及原有行为。fixture 登记实际加载的 WinAudioHook-5.dll 路径，脚本散列模块文件；仅看到 GUI 重启不能证明 DLL 更新。Attach 适配器补控制环被覆盖条数的观察计数，不改原读取／IPC 发布算法；它不证明不存在其他交付损失。

真实双 PID 短检查第一次已正常完成，但 Windows PowerShell 的 Start-Process 对象在未先绑定 Handle 时，等待后 ExitCode 为空，脚本按失败处理。改为就绪轮询前取得 Handle、退出后 Refresh；两秒受控目标复验得到退出码 0，完整短检查也通过。原失败目录保留，不计入正式基线。每个目标的初始 discontinuity 与真实 API 失败分列；短检查不替代十分钟真实会话。

### 旧包络微基准出现一次波动

在未改 WaveEnvelope 算法时运行相关测试，EightChannelFullHistoryTiming 的缓存查询为 476493 ns，超过旧 250000 ns 绝对阈值；单独复跑为 218931 ns 并通过。保留本次波动，不调整既有阈值、不将短微基准作为 GUI 预算结论；正式图表基线与编译隔离，仍按阶段独立验收。原文件的两条 C4828 编码警告保持为既有问题，没有整体重编码。

### 图表短行为检查完成

测试驱动增加显式 manual 开关，仅停用自动滚动，产品图表算法不变。真实检查第一条 Track 冻结后缩放到约 3～7 秒、滚至其他声道／声谱及第二条 Track，再滚回，冻结内容与缩放保留；第二条 Track 仍为约 0～10 秒独立轴。解除冻结后第一条恢复更新。7338 帧正常结束，原始记录确认 1651～5667 中 4017 帧仅第二条请求新波形，第一条无请求；见 charts-behavior-before.json 与 gzip。无新增复用由原有 ChartDataPipeline 回归覆盖。短检查期间允许独立编译，因此不作性能预算或配对耗时证据；该短检查二进制未另存哈希，已明确登记。

### 正式计时期间隔离编译负载

三轮图表主场景和三轮单 Track 对照均完成，接着顺序采 off／buffered／flush 完整负载对照。期间仅阅读代码、准备 Inspector 计量接线和文档，不编译、不运行其他性能负载。图表各轮使用同一 `bcc5c71` 二进制及 SHA256，不能将随后尚未编译的 Inspector 源码当作该二进制内容。用户再次确认已解锁，实时窗口截图确认正常显示真实声谱；没有要求重复解锁。

Inspector 测试场景复用生产接收、图关联、筛选、主表、参数面板和保存入口。固定上下文明确标为合成 session，真实十分钟 Attach 会话独立验收，不用固定记录冒称真实注入。操作从预定 QPC 发出时间计至相应状态提交帧；ImGui 滚动延迟到下一帧生效，不能在调用 SetScrollY 时提前结束计时。

### 图表基线的可重复负载与观察口径

在 Chart Data Pipeline、WaveEnvelope、HeatmapTexture 和绘图入口接通按 Track／方向／声道区分的工作量，保持原刷新算法。包络工作量和汇总分布用例均先失败再通过；原 `test_wave_envelope.cpp` 有非法 UTF-8 字节，新增用例以 ASCII 追加，保留原文件全部既有字节。

独立、显式构建的 WinAudioPerformanceGui 复用生产 AppUi／D3D 主循环，通过现有 RenderTrackList 接口创建真实八声道 Track，使用生产 Channel ident。两 Track 分别为 65%／25% 音量，以便区分 tap；端点由复跑参数明确指定。仅测试程序含负载准备与定时滚动，发布 GUI 不含测试接入。保持 2048／1024／8、491520 样本和 128×480。

短烟测确认四阶段均实际绘图：当前视口约四张波形或三张声谱，CPU 历史仍维护两 Track 各八声道。客户区为应用逻辑坐标 1264×761，DPI API 返回 96（应用未启用 DPI awareness）；真实 D3D adapter 为 NVIDIA GeForce RTX 2060。启动前十秒未满历史时整窗读取成本较高，不能缩短既定 30 秒预热。滚动命令下一帧生效，因此过渡帧单列为 transition，不用上一视口稀释下一阶段。

测试程序首次编译发现日志枚举使用 Err、HRESULT 名称函数位于 wa::log；改用仓库已有标识符后构建通过。短烟测不作为三组基线或预算验收，正式结果另行归档。

### 解锁后完成 #220 的真实 GUI 核对与干扰对照

2026-10-24 用户确认解锁后重新获取窗口，启动当前 Release；正常切换 Render、Pipeline、System Loopback 后关闭。三条操作都结束于对应页面的实际提交帧，2629 帧原始记录通过完整性检查。操作响应分别为 675.2915、81.6915、655.7215 ms，包含真实输入排队；这是计量链路烟测，不作为 Inspector 100 ms 操作验收。

另以相同 Release、初始窗口、空闲 Monitor 和展开日志采集探针关闭／缓冲写入／逐帧 flush：外部每秒读进程 CPU、Private Bytes、Working Set，预热 10 秒后观察 60 秒；启用模式另外从原始帧记录预热 30 秒后取 60 秒。各模式单次对照，不冒称三组主负载验收。缓冲写入／flush 的 I/O p95 分别 0.0318／0.0988 ms，主线程 p95 为 0.3162／0.3039 ms。关闭模式没有内部计时。详见 meter-report.md、meter-summary.json 和哈希清单；原始日志以 gzip 无损保留。

原有锁屏阻挡已经解除。接着按 #221／#222 的真实负载获取优化前基线；本次空闲干扰对照不能替代它们。计量工具的额外工作量接线会在完整负载下再次观察 I/O 和外部 CPU，不能从空闲开销外推。

进程 CSV 初次按原始 CRLF 字节入库触发 Git 尾部空白检查；改为 UTF-8／LF 并重新计算汇总与清单哈希，数值不变。原文件留在 build；汇总同时登记 original_process_sha256 与入库 process_sha256。Python 3.7 的 gzip.compress 不支持 mtime 参数，归档改用标准库 GzipFile，固定归档时间并逐份验证解压字节。

### 构建工具与指南版本不一致

本机没有 VS2022，配置 `Visual Studio 17 2022` 返回找不到实例。`vswhere` 确认安装 VS2019 和 VS2026，原有 build 缓存本来使用 VS2026。采用独立的 `build/performance-vs18` 和已安装 VS2026 编译，记录实际编译器；不改仓库 CI 的 windows-2022／VS2022 要求，不触动用户已有产物。是否通过规定的 CI 与本机编译分别记录。

### 现有帧计量的边界不完整

FrameProbe 在 AppUi::draw 内清零，缺 NewFrame 前的工作；只输出四种非空切片，场景标签固定。把帧边界放到主循环，记录真实页面、整帧与间隔、操作完成帧和丢样。保留禁用时不取时的行为，不增加音频线程工作。

### Windows 桌面锁屏使 GUI 前置验收受阻

Computer Use 截图确认桌面锁屏，已请求用户解锁；不尝试解锁或继续桌面输入。依据该技能 `docs/guidance.md` 的明确要求：`If the Windows desktop is locked, stop immediately and ask the user to unlock the desktop.` 暂停的是桌面操作，继续没有 GUI 前置依赖的 #229。

#220 还需要真实 GUI 场景核对、探针关闭／缓冲写入／逐帧 flush 的干扰对照。#221、#222 明确要求前置完成后领取，因此没有先改图表或 Inspector 的生产优化路径，也没有用旧日志或自动测试替代优化前基线。#223～#228、#230、#231 继续保留原依赖。

### 计量工具也需要失败回归

增加可控时钟后发现一次输入可能重复完成两个操作，失败测试复现后消耗该输入标记。汇总工具原本忽略没有帧号的截断尾行，新增失败测试后改为拒绝；同时校验切片频率、操作 ticks 和对应绘制提交帧。正常关闭写入结束标记，`--require-complete` 防止把仍在运行或异常截断的日志当完整验收记录。

### Hook 测试夹具的栈溢出

预分配 Ring 大小为 2,197,032 字节；`ring = {}` 在本机产生同样大小的栈临时对象，测量和测试首次退出 `0xC00000FD`。改为在预分配区原位清零后重跑。首次崩溃的空输出不计入基线；之后取得的优化前数据保留在 `hook-baseline-x64.csv`。

### 双架构构建的临时目录冲突

构建基线脚本首次让 x64／x86 共写一份临时源文件，出现文件占用。改为每种架构分别提取源码和配置构建目录，不删除原目录。提取固定 Git blob 字节，不依赖 PowerShell 默认编码；没有新增 Git 工作树或修改用户产物。

### 真实目标覆盖不能只看日志非空

最初目标使用已启动的 render 和 endpoint loopback capture，Attach(Render) 后日志只有 render 方法。增加按接口检查后该轮明确失败，未计为完整通过。受控目标随后定期重新获取两种服务，验证关闭调用记录时 GetService 的必要安装工作仍生效；生产 Activate／GetService 及 IPC 协议均未扩大修改。测试同时覆盖捕获／播放、控制调用、关闭门控、再次 Attach 和只 Detach 一项。

### Hook 并发与切换决定

四个真实 API 返回后才初检；共享 emit 在记录取时前终检。quiet 用 ReadAcquire，capture／pump 的对齐 32 位字段用 ReadULongAcquire，运行期写用 InterlockedExchange。保持布局 5、CallPod、Ring、映射名和原发布顺序。没有锁、等待、音频线程分配或 I/O；通过终检后的在途提交被允许，不宣称严格排空或证明整个 IPC 发布协议。

### 评审发现的计量遗漏

两轴评审发现三个实际问题，已分别增加失败回归后修正：页签点击帧的 BeginTabItem 返回 false，原接线漏记点击；后来的无关键鼠消息覆盖输入起点；最后一帧未结束的切片未记入丢失。页签现在无论是否可见均记录点击，等目标内容真正显示才完成操作；只采左键按下作为页签起点；序列化时将仍打开的切片计入丢失，汇总拒绝该记录。

headless ImGui 测试最初把移入和点击放同一帧，未触发页签选中。诊断确认点击可见、但 AllowOverlap 需要前一帧悬停；按真实输入顺序先移入一帧，再按下，下一帧目标显示。回归验证响应包含等待至实际目标提交。测试夹具修正没有改 vendored ImGui。

复核补充发现连续按下可在主循环一次排空、随后被 ImGui 默认输入队列分到不同帧。新增失败回归复现“首个点击误用后一次时间戳”；采用最小处理：不覆盖首个未消费起点，额外按下或目标点击缺起点均计丢失，汇总拒绝该轮。未消费起点在帧结束丢弃，避免错接后续动作；双击消息按左键按下处理。保留生产默认输入队列，测试亦使用默认配置，不增加并行事件调度框架。

### 同目录并发构建造成 PDB 冲突

一次 Debug 构建尚未退出时误启动第二次同配置构建，触发 C1041。等待原进程结束后串行重建通过；没有加 /FS 掩盖，也没有改生产编译选项。后续同一构建目录／配置串行执行。

### 测量文件编码与换行

PowerShell 重定向的六份音频原输出为 UTF-16LE，已无损转为 UTF-8；原字节保存在 `build/performance-vs18/audio-original-output/`。复核发现转换时带入重复 CR，入库文本统一 UTF-8／LF，数值和记录内容保留。规范化前的文本及双 SHA256 清单保存在 `build/performance-vs18/pre-normalization-records/`；汇总哈希对应规范化后的入库文件。局部 .gitattributes 保持原始记录字节，避免不同 Windows checkout 自动换行后哈希漂移。后续采样显式使用 UTF-8 输出。

### Hook 实测的结论边界

关闭路径每组配对均体现消重，全开路径与真实音频未见三组一致回退；个别 Detach／再次 Attach p99 尖峰完整保留在报告，不删样本或夸大成所有尾延迟改善。静音两声道实机证据不替代图表八声道或所有音频设备验收。详见 [独立验收报告](hook-report.md)。

规格复核纠正了证据文案：计时是音频处理循环主体，不含周期性服务获取和 Sleep；原始数值未修改。补齐旧探针放置、三组前后音频、实际加载路径与 DLL 哈希核对的完整复跑命令。

### 完整测试中的既有硬件失败

本轮 Debug 和 Release 全量测试出现 `AudioEffects.SetHardwareSkipsIfUnavailable` 失败，断言位于 `src/tests/test_audio_effects.cpp:193`：要求切换后状态为 On，设备仍回报 Off。已有 `build/bin/Release/WinAudioTests.exe` 单独运行同名测试也复现同一结果；相关测试及 AudioEffects 生产文件与实施起点无差异。本轮保留该失败，不修改不相关硬件音效逻辑，不把完整测试写成全绿。既有 `test_wave_envelope.cpp` 两处 C4828 编码警告亦原样记录。

为不覆盖已运行的自有 Debug GUI，最终 Debug 在独立 `build/performance-vs18-verify` 完整配置／构建；Release 使用 `build/performance-vs18`。构建运行中修改过测试文件，一次增量构建未收进新用例；通过核对 gtest 枚举发现后，更新该文件时间并重建，确认新用例实际执行并先失败再通过。后续以实际测试枚举与执行数量核对，不能以构建退出码替代。

## 本地最终验证（2026-10-24）

| 项目 | 结果 |
| --- | --- |
| x64 Debug 全部目标构建 | 通过，独立 verify 目录 |
| x64 Release 全部目标构建 | 通过 |
| x64 Debug CTest | 550／551；上述既有硬件测试失败，退出码 8 |
| x64 Release CTest | 550／551；同一既有硬件测试失败，退出码 8 |
| FrameProbe 与实际 ImGui 页签测试 | 20／20，通过禁用、跨帧、缺失与歧义输入等回归 |
| 汇总工具单元测试 | 5／5 |
| Hook x86 Release 生产入口测试 | 8／8 |
| Hook x64 Debug／Release 生产入口测试 | 各 8／8，已包含在 CTest |
| Hook x64／x86 生成代码 | 两种位数均已检查，摘录入库 |
| 原始配对数据哈希 | 10 份配对文件的工作区与 Git 暂存字节均匹配汇总 SHA256 |
| README PowerShell 命令 | 三个代码块语法解析通过 |
| Standards 轴最终复核 | 0 项未关闭发现；此前四项计量缺陷均已修复 |
| Spec 轴最终复核 | 0 项未关闭发现；两项计时范围／复跑说明问题已修复 |
| 真实 GUI／探针干扰对照 | 未验证，桌面仍锁屏 |
| 远端 CI | 未运行，本轮不推送 |

完整 CTest 日志分别保存在 `build/performance-vs18-verify/ctest-debug-final.log`、`build/performance-vs18/ctest-release-final.log`。没有把 gtest／CTest 成功代替真实 GUI 证明，也没有关闭远端票据。下一步仍是解锁后完成 #220 的真实链路与三模式干扰对照，再依次采 #221／#222 优化前基线。

## 本地提交

- `718bc57`：#220 的完整帧／操作计量基础与自动测试；真实 GUI 验收仍未完成。
- `62bec9d`：#229 的生产门控、行为测试、双架构探针与复跑工具。

提交到当前 `main`，不推送、不发布。其余票按上表保留未完成状态，不以本地提交自动关闭 GitHub Issues。

## ???????2026-10-24??????

### Inspector ?????????

`ec33113` ????????? PID ??? Live session???????????????????????????????????????? `build/performance-vs18/inspector-baseline-before` ??????????????`e53a314` ???????????????? `build/performance-inspector-before/bin/Release`????????? `inspector-baseline-before-verified`????????? SHA256 ?????????????????? endpoint???? PID ???????

### ?????????

???? `ScopeReader ? refreshCharts ? ????` ?????????? -999 ????????????????????????? 4 ??? 5?Release ?????? `build/performance-vs18/chart-failure-red.xml`?????????????????? W ?????????????????????????????????????????????Inspector ??????????????????????????

### ?????????????????

`inspector-baseline-before-verified` ??????????? p95 85.94 ?????????????????????????????????????? ScrollMax?????????????????????????????????????????????`2f0b37c` ???????????????????????????????????????????????????????

???????????????????? `2f0b37c` Git ??? spdlog ????????????? `build/performance-inspector-before-v2` ????????????????????????? `inspector-baseline-before-final`??? build-inspector-baseline.ps1 ?????????????????????????

### #223 ???????????

capture?render ???? WaveRefreshState??? waveFilled ????? epoch??? written ?????mono ?? ending-at??????????????GUI ???????????? WaveSnapshotScratch???? Track ????????MinMaxPyramid ??????????????? generation?revision ????????????????????????????????????????64 ?????? raw?exact?coarse ??? FFT ???

Release 16 ? ChartDataPipeline ???????????????????????????????????????written ????????? ScopeBuffer ????????????Debug ???? GUI ??????????55?55????????????Release ?? GUI ?????????????? #230????????
