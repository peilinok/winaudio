# 集成 GUI 行为证据

2026-10-24。本目录区分真实窗口观察与确定性测试；截图不是性能预算样本。正式采样另见图表和 Inspector 后测报告。

## 图表

`charts-distinct-channels.png`、`charts-frozen-return.png`、`charts-channel-paused.png`、`charts-recreated-source.png` 来自当前首批实现的 WinAudioPerformanceGui，SHA256 为 `4EC3DFBAF134D8ABBE4BB2AA1A0FA293CABFEAC1F9EA5D56CF113948792DA5D9`。手动开关只停用自动滚动；真实 Track、Chart Data Pipeline、WaveEnvelope 和 DX11 路径不变。

两条实际八声道 Track 使用既有 Channel ident，音量 65%／25%。观察不同声道内容；第一条冻结几分钟后缩放至约 3～7.5 秒，滚到第二条及其他声道声谱，再回到第一条，冻结内容和范围保留，第二条仍为约 0～10 秒独立范围。短／长冻结恢复正常；暂停 Ch 1 后变为平线，再恢复更新。删除两条 Track 后空页无旧图；新建 id 3 的默认 Realtek 双声道 Track 全暂停为平线，没有继承旧八声道内容或缩放。正常退出，原始记录完整性校验通过。

原始行为目录为 `build/performance-vs18/charts-gui-final/`。夹具强制 Render 页签，因此不能用该窗口证明 Monitor 页的行为。图内拖动未获得可靠证据；随后补充的轴范围平移另见下段，不登记拖动交互通过。

`monitor-capture.png`、`monitor-render.png` 来自独立生产 WinAudioGui。采集为 CABLE Output，输出为 Realtek；显式 48000／16／2，开启同步播放，状态显示 capture／render 同时运行、两端帧数持续增加。滚动分别观察两个 Capture 声道和 Render 的波形／声谱。观察到平线；另外尝试 CLI 向 CABLE Input 播放双声道正弦文件仍未在此链路得到可区分的非零内容，因此该次只能证明静音链路呈现和正常停止，不能作为 Monitor 非零双方向内容隔离证据。停止后图清空；正常退出日志完整。

### 平移与非零 Monitor 补证

`charts-pan-final` 使用本地提交 `0c687a4` 的性能程序，SHA256 `CFE5B072B6527947A234994B10ECE076A112D457AD746BD78C3C52B9D32ED552`，两条真实八声道 RenderTrack、CABLE Input 端点及 65%／25% Channel ident。冻结第一条图表后，放大到 2.56～7.68 秒，再通过 X 轴菜单编辑、按 Return 提交为 1.56～6.68 秒；视窗宽度均为 5.12 秒。两个采集声道的横轴同步变化，滚至第二条时仍为约 0～10 秒全历史；其音频没有随图表冻结而停止。截图为 `charts-pan-range-before.png`、`charts-pan-range-after.png` 和 `charts-other-track-after-pan.png`。`charts-pan-after.png` 记录的是没有改变范围的拖动尝试，不能当作拖动成功证据。

`monitor-nonzero-final` 是独立生产 GUI，SHA256 `CB1FE578E891F36AFA97541D960EB9CD2BB93F87B82F05B6E642E2BF552FF6D2`。用上述 RenderTrack 作为真实信号源，选择 CABLE Output 采集、Realtek 输出、显式 48000／16／2、同步播放。16:38:16.737 两端运行，观察到两个采集声道可区分的非零波形及各自声谱，延迟输出为非零单声道降混波形和独立声谱。冻结后放大，采集各声道与输出共用横轴范围；16:39:54.474 停止后图清空，16:40:05.965 正常退出。证据为 `monitor-nonzero-capture.png`、`monitor-host-zoom.png`、`monitor-ch2-render-zoom.png`、`monitor-nonzero-render.png`、`monitor-stop-clears.png`。只验证内容与生命周期，不由截图推算真实音频延迟；前述静音尝试仍保留，不修正本轮范围外的 CLI 播放问题。

## Inspector

以下来自同一生产 GUI，原始记录为 `build/performance-vs18/monitor-gui-final/`，包含真实 Attach、Detach 和导出日志；该次不是预算采样。

| 行为 | 观察结果 | 证据 |
| --- | --- | --- |
| 同轮多 PID | 先附加 27840，再附加 27620，界面列出两者，旧调用仍保留；表中可见两种 PID | inspector-two-pids.png |
| 单个 Detach | 分离 27620，27840 保持连接，控制历史仍可见 | inspector-single-detach.png |
| 同 PID 重连 | 再附加 27620，没有替换整轮；导出记录证实同 PID 使用新的 attachId | GUI 日志及 180 条 JSONL |
| 全部 Detach | 两 PID 均分离，保存按钮仍可用，旧轮仍保留 | GUI 日志和随后替换提示 |
| 替换前取消 | 提示保存／放弃／取消，显示保存 180 条、将替换 180 条；取消后仍未附加，下一次提示仍为 180 条 | inspector-replace-prompt.png |
| 保存后附加 | 先发布 180 条 JSONL，回收完成结果，再实际附加 27620；旧轮完成保存后才替换 | inspector-replaced-after-save.png |
| pump 选择过期 | 打开 pump 并筛选 Render GetBuffer；选中后近期窗口迅速轮换，详情显示“记录已不在近期窗口”，没有跳到另一条调用；关闭 pump 后提示仍在 | inspector-pump-expired.png |

180 条导出文件为 `calls-multi-20261024-142214-19516-256479765-0.jsonl`。实读 JSONL：PID 27840 共 102 条，27620 共 78 条；全部 roundId=1，attachId 1／2／3 分别 102／21／57 条，证明单轮多 PID、同 PID 重连身份没有合并。

另一次真实按钮已导出固定一万条记录，每条十六个参数，约 21.6 MB；记录见 implementation-log.md。正式后测另测当前版本的保存启动、后台总时长、最慢帧和峰值内存。

Explorer 自动定位该导出文件时弹出“位置不可用”，但目录存在、文件可读取且内容正确。该限制来自既有 `DumpUi::revealDumpFile` 路径，未将其当作导出失败，也不宣称文件定位通过。

### 生产录制范围与失败边界

`gui-production-boundaries` 使用上述生产 GUI。空历史时开启再关闭录制，随后 Attach 自有 PID 26324 并收到调用；Save 明确提示“录制范围内无记录”，没有回退导出后来调用。再次开启录制纳入当前十五条，关闭时冻结二十七条；继续接收到八十七条后 Detach all，替换提示为“保存 27 条／替换 87 条”。取消保留历史，随后实际 Save 写出二十七条、sequence 1～27 的 JSONL。相关截图为 `inspector-empty-recording.png`、`inspector-recording-frozen.png`、`inspector-recording-subset.png`。

真实文件 `calls-pid26324-20261024-162207-25932-263672593-0.jsonl` 的 SHA256 为 `B2839351B7E4B0787834CC12148C7AA35E0BB2F635E125E5AC25DCDACDCB7E6D`，16:22:07.283 启动、16:22:07.288 发布。随后向不存在的 PID 4294967295 选择“放弃后附加”，实际失败提示为 missing debug rights；八十七条旧历史和二十七条范围保留。再向自有 PID 21592 附加，提示仍为 27／87，成功后才换新轮。截图为 `inspector-attach-failure-keeps-history.png`、`inspector-failed-attach-range-unchanged.png`、`inspector-discard-success-new-round.png`。此处证明真实失败路径保留旧状态，不将既有权限提示当作精确的目标不存在诊断。

两个自有音频目标随后自然退出；PID 26324／21592 分别处理 57780／57776 次，API failures 均为 0，discontinuities 各 1；原始 targets.txt 已归档。这是行为目标的收尾，不当作正式 Hook 性能配对。

### 慢写时的真实界面

`0c687a4` 仅在性能程序编译宏内增加受控 writer：`WINAUDIO_PERF_EXPORT_TEST=wait|fail`，首次 write 在专用 worker 等六十秒，分别模拟成功／写失败。它不创建真实文件、不复制参数树，生产 GUI 的默认 writer 不变。以下均由真实按钮和消息循环驱动；不冒充磁盘故障实验或正式保存性能。

| 目录与操作 | 观察结果 | 截图 |
| --- | --- | --- |
| gui-wait-idle，一万条 Save | 保存忙提示；重复 Save 和整轮 Attach 替换禁用；表格仍可滚动、选择；Alt+F4 提供等待／取消，选择等待后完成回收再退出。16:12:17.839 启动，16:13:18.021 模拟发布一万条 | export-busy.png、export-busy-browse.png、export-exit-choice.png |
| gui-cancel，普通取消 | 当前 write 返回后取消生效；旧一万条历史仍可查看，再次保存仍冻结一万条，第一次没有发布 | export-cancel-keeps-history.png |
| gui-cancel，第二次保存中退出 | Alt+F4 选择取消并退出，消息循环继续显示等待；16:17:01.863 取消完成，16:17:01.892 正常停止，没有发布 | export-cancel-exit-pending.png |
| gui-export-fail，保存后附加 | 一万条旧历史上请求下一次 Attach，先保存；六十秒后受控 write 失败，未继续 Attach，历史保留 | export-before-attach-busy.png、export-failure-keeps-history.png |
| gui-live-export，真实双 PID | 16:27:10.952 冻结三十条；保存忙时继续接收，control-total 从 frame 2000 的 30 增至 78；16:28:01.844 Detach all 后仍保留历史、禁止换轮，16:28:10.953 仅模拟发布冻结的三十条 | export-live-start.png、export-live-detached-busy.png |

最后一轮的 Alt+F4 发生在保存完成后，不用它证明忙时退出等待；忙时等待由一万条 idle 轮次覆盖。模拟成功仍触发现有 Explorer 定位，但没有真实文件，不能把定位或模拟发布当作真实保存。普通取消和取消退出都只在当前 write 返回后生效，符合非强制中断的契约。

复跑慢写界面时先构建 WinAudioPerformanceGui，在独立工作目录设置 `WINAUDIO_PERF_MODE=idle`、`WINAUDIO_PERF_RECORDS=10000`、`WINAUDIO_PERF_PIDS=2`、`WINAUDIO_PERF_WARMUP=0`、`WINAUDIO_PERF_DURATION=1800` 和上述 EXPORT_TEST 后可见启动，再按表中操作。真实接收轮使用 MODE=live、LIVE_PIDS 为自己创建的两个目标 PID；PID 不硬编码为本次历史数字。正式性能脚本不设置 EXPORT_TEST。

## 原始归档与完整性

[artifacts/manifest.json](artifacts/manifest.json) 登记 111 份手工 GUI、真实 JSONL、构建／红绿／最终测试及分析脚本的源 SHA256、压缩前内容 SHA256、gzip SHA256 和字节数。每份 gzip 均实际解压散列核验；不改原始日志编码。正式性能目录的 manifest 独立维护。

[artifacts/integrity.json](artifacts/integrity.json) 按运行头登记帧连续性、结束标记与丢失计数。`gui-wait-idle` 起初误以 Hidden 启动，未导出就结束了该窗口，首段仅 1364 帧且尾行截断；下一次运行头因此附在截断行尾。原始字节保留，明确分为不完整诊断段和最终 4481 帧正常结束段，不合并为连续运行。其余八份行为日志均正常结束、帧号连续，四种 lost 计数及末尾 pending 均为 0。行为轮次没有独立进程采样，且可能同时运行信号窗口／读取证据，全部不作正式预算样本。

## 确定性覆盖

Store／Attach UI 模型／ExportJob 的直接测试覆盖 A～D 范围、显式空录制不回退、固定请求不跟随焦点、取消／失败保留旧轮、成功 Attach 才替换、全局六十四条 pump、冻结引用、保存忙、阻塞写入后的取消、退出等待／取消及各文件阶段失败。具体红绿证据和测试数见 implementation-log.md。它们能稳定复现错误边界，但不冒充上述真实 GUI 演示。

波形整组失败、无新增、源／方向／声道身份隔离由 Chart Data Pipeline 回归验证；生产纹理持有到延迟消费读回由 WARP 验证，包含同尺寸／同 revision、同帧不同来源、移动／删除／替换／隐藏释放、失败恢复和调试层。不能用这些设备无关结果外推所有真实音频硬件。
