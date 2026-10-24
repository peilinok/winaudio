# 图表优化前基线

2026-10-24，Release 源码 `bcc5c71`。这是 #221 的旧路径基线，不能代替 #223／#224 修正后的等负载验收；旧纹理仍按图槽共享，快照失败仍可能污染旧波形。即使旧路径已在帧预算内，这两项正确性修复仍须完成。

## 机器与场景

机器见 machine.json：i7-10875H、64 GiB、RTX 2060（实际 DX11 adapter；驱动 31.0.15.1700）、Windows 26200、平衡电源、2560×1440／59 Hz。客户区实测为应用逻辑坐标 1264×761，DPI API 返回 96；程序未启用 DPI awareness，不能将此数值当截图物理像素。日志展开，窗口、布局和显示条件固定。

使用 CABLE Input 端点 `{0.0.0.00000000}.{09793ccf-e246-4dbf-800b-a7cf44856994}`，mix 为 48000 Hz／8ch／Float32；生产 RenderTrack 使用实际 48000 Hz 和既有 PCM16 转换规则。两条真实 Track 各八声道，循环播放生产 Channel ident，音量分别为 65%／25%。所有 CPU 历史持续维护；视口约四张波形或三张声谱。固定 FFT 2048、Hop 1024、catchup 8、每声道 491520 样本、声谱 128×480。

每轮预热 30 秒，再采 120 秒。双 Track 四阶段各 30 秒；单 Track 波形／声谱各 60 秒。滚动延迟生效的 transition 帧单列；预热和过渡不计入阶段预算。未满历史的启动路径曾明显更慢，不能缩短预热。每组双 Track 后运行单 Track，重复三组。

## 阶段结果

单位为 ms，每个单元格为 p95／p99；原始分位数采用 ceil(p×n)。各次各阶段分别判断，不合并稀释。

| 场景 | 第 1 次 | 第 2 次 | 第 3 次 |
| --- | --- | --- | --- |
| 双 Track：Track 1 波形 | 6.3937／7.7924 | 6.3557／7.2244 | 6.2932／6.9562 |
| 双 Track：Track 1 声谱 | 4.7397／5.2956 | 4.7065／5.3939 | 4.6489／5.2924 |
| 双 Track：Track 2 波形 | 6.3368／6.9546 | 6.1770／6.6622 | 6.0843／6.6609 |
| 双 Track：Track 2 声谱 | 4.8170／5.3746 | 4.6583／5.4689 | 4.6725／5.2773 |
| 单 Track 波形 | 4.5308／5.0175 | 4.3888／4.8119 | 4.3260／4.7704 |
| 单 Track 声谱 | 2.9456／3.3018 | 2.8622／3.1691 | 2.7573／3.0700 |

双 Track 阶段约 1797～1799 帧，单 Track 阶段约 3596～3597 帧。双 Track 探针 I/O p95 为 0.1314～0.1471 ms，单 Track 为 0.1009～0.1116 ms。主线程从 NewFrame 前计至 RenderDrawData 返回；Present 和探针落盘另列。完整结束标记、丢样、每帧工作量、实际可见数、Present 与帧间隔分布保留在各轮摘要，不能将上传调用时间当 GPU 执行时间。

当前所有阶段在 8／16 ms 目标内。尚不能宣称图表优化完成或加速；若首批正确路径也达标，停止环形历史、增量包络和局部上传等扩大改造。

## 采集干扰与归档

完整负载的 off／buffered／flush 顺序对照均已完成。预热后外部进程 CPU 分别为 333.790／328.326／327.061 ms/s，Private Bytes 峰值分别为 203530240／204496896／204546048 字节。各模式一次，顺序和系统波动不能解释为探针带来负开销；也不能将外部 CPU 当主线程经过时间。

buffered 四阶段主线程 p95 为 4.7286～6.4009 ms、p99 为 5.2050～7.0269 ms；flush 分别为 4.5329～6.4930 ms、5.0751～7.0894 ms，均在预算内。buffered I/O p95 为 0.1366～0.1497 ms、p99 为 0.4876～2.0005 ms，反映缓冲集中落盘；flush I/O p95 为 0.2377～0.2602 ms、p99 为 0.3171～0.3416 ms。原始帧间隔保留全部等待，不把缓冲的尾部 I/O 藏入主线程切片。目前没有观察到采集模式改变预算判断，不能据此外推其他机器或更重负载。

六轮基线位于 [charts-before/runs.json](charts-before/runs.json)，三模式对照位于 [charts-overhead-before/runs.json](charts-overhead-before/runs.json)。目录各自附 manifest.json，登记原文件、归档内容和压缩文件 SHA256；每份 gzip 已实际解压逐字节核对。每轮配置、进程 CSV、原始帧和应用日志均保留。旧路径直接写展示数组，波形额外事务暂存载荷为 0；#223 的共享暂存容量须单独登记。

复跑（先显式构建 WinAudioPerformanceGui；正式计时期间不编译）：

```powershell
& scripts/performance/run-charts.ps1 -EndpointId '{0.0.0.00000000}.{09793ccf-e246-4dbf-800b-a7cf44856994}' -OutputDirectory build/performance-vs18/charts-before -Plan baseline
& scripts/performance/run-charts.ps1 -EndpointId '{0.0.0.00000000}.{09793ccf-e246-4dbf-800b-a7cf44856994}' -OutputDirectory build/performance-vs18/charts-overhead-before -Plan overhead
```

脚本拒绝覆盖既有运行目录；复跑请更换输出目录。配置逐轮记录 exe SHA256 与源码提交；计量程序的测试场景与生产 GUI 分开构建。
