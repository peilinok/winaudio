# Inspector 优化前基线

2026-10-24。固定负载十五轮、两轮十分钟内存观察、三模式探针干扰和十分钟真实会话均已完成。#222 本地基线收齐；旧算法结果不作为优化后的验收。

## 来源与可复跑输入

生产算法与驱动固定为 `2f0b37ce0c63bd97cc265c2b436e2270154fce3e`。独立源码目录为 `build/performance-inspector-source-v2`，构建目录为 `build/performance-inspector-before-v2`；主工作区随后完成的图表修复没有混入。spdlog gitlink 与实际源码均为 `6fa36017cfd5731d617e1a934f0e5ea9c4445b13`。

正式运行目录为 `build/performance-vs18/inspector-baseline-before-final`。程序 SHA256 为 `5F8D0C8A7B16CE406EFB436874B151711B0457F7A2FFD5B6965F7B040F753640`。固定输入每条包含十六个结构化参数，保留多层、枚举、指针、HRESULT、多行 Pre／Post；参数树不为空。输入散列如下：

| 输入 | SHA256 |
| --- | --- |
| 一千条、双 PID | `6BD6105FE896C39D681B083614F3C4BCFB822594794474879BDB2AFDBB62549F` |
| 一万条、双 PID | `B7006AEFFB8AB782F37C4B8E960910750E81C2B5FF636D12F24B40A8E8AE3D4F` |
| 一万条、单 PID | `9A77A9859421770A48C67A2565A997ADE688CBAA5A65C0FDB6D178B48876C71F` |

机器与图表基线相同，见 machine.json 与 [图表基线报告](charts-baseline-report.md)。每轮预热三十秒、测量一百二十秒，共一百二十次预定操作；响应从预定 QPC 起点到对应状态实际完成绘制提交，包含等待。固定上下文明确标为合成 Live session，未冒充真实注入。追加模式由九千逐步增加到一万条；滚动目标固定在动作发出时的位置。

## 固定场景结果

单位 ms。每格为主线程 p95／p99；括号内是全部操作合并后的 p95，低频保存另列，不能以合并分位数代替保存体验。

| 场景 | 第一次 | 第二次 | 第三次 |
| --- | --- | --- | --- |
| 一千条、双 PID | 6.4215／7.5419（37.5826） | 6.4384／7.5079（37.5291） | 6.3768／7.2505（37.6507） |
| 一万条、双 PID | 65.1228／74.5743（176.6685） | 63.9034／71.5945（183.7959） | 69.2509／81.4768（178.9809） |
| 一万条、单 PID | 65.6912／73.5825（176.1569） | 64.6328／71.1327（177.7329） | 73.4882／82.8726（184.8316） |
| 持续追加到一万条 | 102.0262／105.7958（254.2354） | 102.6653／111.6446（258.8233） | 104.5511／117.9655（278.1496） |
| 一万条、录制和保存 | 70.4208／79.2758（190.7989） | 69.9610／78.2702（187.8097） | 68.2992／77.0901（188.8202） |

一千条在目标预算内；一万条的静态和追加场景都超出主线程 8／16 ms、操作 100 ms 的预算。第二次双 PID 一万条中，call-sort p95 为 33.646 ms，call-rows 为 10.0796 ms。旧路径每帧复制完整参数树、重新整理全量展示和提交全部匹配行，是首批应消除的实测成本。完整切片和工作量保留在逐轮摘要，不能将相互嵌套切片相加为整帧。

| 保存场景 | 第一次 | 第二次 | 第三次 |
| --- | --- | --- | --- |
| 保存单次响应 | 354.0377 | 329.0287 | 323.6857 |
| 开录制响应 p95 | 138.1536 | 129.3118 | 128.5171 |
| 关录制响应 p95 | 100.0750 | 119.4309 | 109.8774 |
| 最慢主线程帧 | 314.4817 | 324.5082 | 321.7588 |

保存的三次独立响应均超过 100 ms。全部操作合并的分位数会掩盖一次保存，因此汇总同时输出 `operations_by_target_ms`。尚未实施的单份 Store、索引缓存／可见行、摘要缓存和后台导出须分别完成行为验证后，再使用同输入复测。

## 长时与采集干扰

固定一万条的十分钟重复浏览已完成，共 9106 个计量帧、600 次操作。主线程 p95／p99 为 79.9023／96.6848 ms，操作 p95 为 204.6115 ms；每种操作一百次，参数滚动 p95 为 256.963 ms。空闲对照 9604 帧，主线程 p95／p99 为 67.9502／76.8820 ms。两轮预热后各 595 个进程样本，间隔约一秒。

| 指标 | 重复浏览 | 固定历史空闲 |
| --- | --- | --- |
| CPU，ms／s | 1002.3610 | 1003.5051 |
| Private Bytes，最小／最大，字节 | 167718912／204623872 | 141656064／203169792 |
| Working Set，最小／最大，字节 | 159547392／197136384 | 137039872／196935680 |
| Private Bytes，首／末一分钟均值，字节 | 198746453／199253402 | 195906014／200978842 |
| Working Set，首／末一分钟均值，字节 | 190323985／191109666 | 188471296／194366396 |

浏览的 Private Bytes 每分钟上沿在 194.25～195.14 MiB 间波动；空闲到第六分钟后，上沿在 202948608～203169792 字节间波动，均值亦进入平台。没有发现相对空闲对照持续抬升的浏览额外增长；不以首末单点差值诊断泄漏。旧算法即使空闲也占用约一个 CPU 核，符合每帧全量整理的成本。追加曲线保留在逐秒进程样本及控制总数工作量中；完整历史随新增记录增长被接受，不能把固定记录平台误称为长会话总内存有界。

off／buffered／flush 各一次相同一万条浏览对照已完成，各有一百二十个预热后进程样本。关闭探针时没有内部帧计量，不能补造该模式的帧分位数。

| 探针模式 | CPU，ms／s | Private Bytes，最小／最大，字节 | Working Set，最小／最大，字节 | 主线程 p95／p99，ms | probe-io p95／p99，ms |
| --- | --- | --- | --- | --- | --- |
| off | 963.4413 | 171982848／200851456 | 167940096／192712704 | 无内部采样 | 无内部采样 |
| buffered | 1001.9374 | 191168512／202473472 | 180510720／193830912 | 75.3114／82.9698 | 0.0242／0.0362 |
| flush | 1003.1960 | 189239296／202362880 | 180760576／194355200 | 68.5669／78.9598 | 0.0882／0.1165 |

buffered／flush 分别有 1840／1937 个浏览帧和各一百二十次操作，操作 p95 为 194.0561／180.7687 ms。相对 off 的进程 CPU 分别增加约 4.0%／4.1%；逐帧刷新增加写出切片，但这次单组对照不能证明三轮稳定收益，模式间的帧差异也不能归因于探针优化。正式比较继续使用 buffered。

真实会话使用两个新建的 x64 HookAudioProbe 目标 PID 28144／27680，实际端点为 `{0.0.0.00000000}.{46f4256f-bc60-4865-b5f3-4c6eef7ee966}`，48000 Hz／2ch／32-bit。两进程均实际载入旧构建目录的 WinAudioHook-5.dll，SHA256 为 `487A3DD90F08AA7DB5725E505AA25DD5D25076174B8033B55F87A28AA61CB93A`，与同目录 WinAudioHook.dll 一致。

默认 pump 关闭的十分钟共 35970 帧，新接收控制调用 576 条，合计速率约 0.96 条／秒；预热末累计二十四条，正式阶段末累计六百条。主线程 p95／p99 为 1.3535／1.9624 ms，最慢 12.8579 ms。随后二十秒开启检查有 1199 帧，新增十八条控制调用，主线程 p95／p99 为 2.0219／3.7332 ms。旧适配器当时读取双 PID 合计一百二十八条 pump，最终展示依既有 shapeCallLog 截到六十四条；新实现须保持全局六十四条且补来源身份，不能把旧适配器数量当最终显示数量。

正式关闭阶段的 595 个外部进程样本中，CPU 为 79.3429 ms／s，Private Bytes 为 85708800～87617536 字节，Working Set 为 88268800～90628096 字节；这段随着新增历史的正常增长与固定一万条的稳定性验收分别判断。控制环覆盖累计和 pump xrun 均为零。目标退出统计：43627／43622 次处理循环，调用失败均零，各一次 discontinuity，捕获帧数分别 32857920／32854080。长期模式的计时蓄水池只有 4096 个槽，timing_overflow 39531／39526 是额外计时样本未保留，不是音频或 IPC 溢出，不据此计算完整十分钟目标尾延迟。

真实窗口已核对双 PID、关闭／开启状态和控制列表。ETW 因权限返回 0x80070005，界面明确显示不可用。连续目标在 Attach 前已经 Initialize，之后主要产生 GetBufferSize／GetService；低速场景达帧预算不能代替一万条固定负载，也不能证明重新 Initialize 的图字段覆盖。

## 废弃记录与复跑

`inspector-baseline-before` 缺少合成进程树接线；`inspector-baseline-before-verified` 的追加滚动目标追赶持续增长的 ScrollMax。两者保留诊断，不混入正式结果。正式十五轮均使用修正后的相同二进制，并已验证结束标记、帧连续性、无丢样及逐轮操作数量。

```powershell
& scripts/performance/build-inspector-baseline.ps1 -SourceDirectory build/performance-inspector-source-rerun -BuildDirectory build/performance-inspector-before-rerun -Generator 'Visual Studio 18 2026'
& scripts/performance/run-inspector.ps1 -OutputDirectory build/performance-vs18/inspector-baseline-rerun -BuildDirectory build/performance-inspector-before-rerun -SourceRevision 2f0b37c -Plan baseline
& scripts/performance/run-inspector.ps1 -OutputDirectory build/performance-vs18/inspector-baseline-rerun -BuildDirectory build/performance-inspector-before-rerun -SourceRevision 2f0b37c -Plan memory
& scripts/performance/run-inspector.ps1 -OutputDirectory build/performance-vs18/inspector-baseline-rerun -BuildDirectory build/performance-inspector-before-rerun -SourceRevision 2f0b37c -Plan overhead
& scripts/performance/run-inspector.ps1 -OutputDirectory build/performance-vs18/inspector-baseline-rerun -BuildDirectory build/performance-inspector-before-rerun -SourceRevision 2f0b37c -Plan live
```

正式采样期间未编译或运行其他重负载。脚本拒绝向已有同名运行目录追加；复跑请换输出目录。全部正式结果归档到 [inspector-before/runs.json](inspector-before/runs.json)，目录附原始／内容／压缩文件 SHA256 清单。固定输入去重，每份 gzip 均实际解压逐字节验证；废弃目录不混入。
