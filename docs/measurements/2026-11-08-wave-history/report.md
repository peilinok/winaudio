# GUI 波形环形历史与包络增量索引：实施及复测

日期：2026-11-08。范围：性能普查的问题 1。

四个八声道 Track 的全部轨道视图中，两轮 CPU 从 **68.72%～70.83% 单核降到 26.94%～29.40% 单核**；主线程 p95 从 **8.06～8.10 ms 降到 2.11～2.42 ms**。所有声道继续保留最近历史，窗口仍为每声道 491520 点。

## 实施

- `ChartDataPipeline` 的 GUI 历史改为原位覆盖最旧样本；跨缓冲末尾最多分成两次复制。`WaveRefreshState` 记录逻辑起点和累计写入量，`WaveHistoryView` 提供从旧到新的只读访问。
- 各声道仍先读入共享 scratch，所有读取成功后才一起提交数据与游标。绘制通道选择不参与历史刷新，隐藏声道继续更新。
- 包络索引保留物理位置固定的 64 点 min/max 块，仅重算被覆盖的块。隐藏声道再次显示时，按自上次查询以来的累计写入量补更新；覆盖达到一整窗口、来源代次或窗口改变时全量重建。
- 保留原来按逻辑窗口对齐的包络边界规则。物理块与逻辑块错开时，查询扫描两端的少量原始样本；放大视图直接按逻辑顺序读取，不把整个环形历史复制回连续数组。
- 冻结期间保留 GUI 自己的历史，恢复时按新增量或最新完整窗口追上；音频线程、ScopeBuffer 协议、FFT 和声谱历史算法未改变。

## 正确性与构建

- Debug / Release 的 `WinAudioGui`、`WinAudioTests` 构建成功。相关 8 组共 **62 项测试，各配置均 62/62 通过**。
- 修改前相关测试基线通过。将增量刷新搬移工作量要求改为零后，旧实现以 24 字节搬移失败，环形实现通过。新增索引测试先以全量扫描 1024 点失败，修改后只扫描被覆盖的 64 点块。
- 环形读取测试先在未适配的包络查询中失败，修复后验证多个起点、部分块、缩放和平移范围，与连续参考的 X、min、max 一致。
- 八声道组合用例覆盖初始补零、反复环绕、非整块追加、整窗口替换、超过窗口的追赶、冻结恢复、延迟查询 Ch 8 和缓存复用。参考历史与每个声道逐点比较；包络独立扫描规定范围的 min/max。既有逐声道失败、来源切换、计数回退、方向隔离测试保留。
- 现有冻结缓存计时门槛保持不变。开发中检测到缓存查询退化后，将连续存储查询单独编译为简短路径，最终 Release 原有门槛通过。
- `test_wave_envelope.cpp` 原有两处非 UTF-8 注释会产生 C4828；独立重编旧版文件也复现同样警告，未扩展本次修改去处理这些原有注释。
- 原有用户改动已保留：修改前完整 tracked diff 的 SHA256 与上一轮 profile manifest 一致；本轮未改其他原有修改文件。AppUi 相对本轮初始快照只调整波形访问、函数参数和一个工作量指标。身份见 `manifest.json`。

## 测量方法

复用 2026-11-06 性能普查的本机端点、窗口、视图夹具和统计口径；新产物放在独立目录，旧报告、原始数据和旧二进制保留。Release x64，48 kHz / 8ch，四个 Render Track；每次预热 10 秒、正式 35 秒，当前轨道与全部轨道各两轮，另测冻结和关闭探针。所有负载、构建和大 trace 汇总分开运行。

CPU 100% 表示占满一个逻辑核；本机有 16 个逻辑核。主线程耗时不含 Present 等待。全部轨道视图中其他 Track 部分位于滚动区外，实际可见的是首轨选中声道波形；历史刷新覆盖 4×8 个声道，不能称为同时绘制 32 个可见波形。GUI 自动负载正常退出及 trace 完整性检查通过；本轮未声称完成手工听音或全部 UI 操作验收。

## GUI 对照

| 场景 | 原 CPU，单核 | 新 CPU，单核 | 原主线程 p95 / p99，ms | 新主线程 p95 / p99，ms | 新峰值 Private Bytes，MiB |
|---|---:|---:|---:|---:|---:|
| 当前轨道，第 1 轮 | 29.31% | 18.98% | 2.829 / 3.028 | 1.224 / 1.443 | 240.09 |
| 当前轨道，第 2 轮 | 33.69% | 17.53% | 2.861 / 3.041 | 1.269 / 1.492 | 240.62 |
| 全部四轨，第 1 轮 | 70.83% | 29.40% | 8.101 / 8.642 | 2.111 / 2.586 | 296.24 |
| 全部四轨，第 2 轮 | 68.72% | 26.94% | 8.059 / 8.493 | 2.418 / 2.715 | 296.69 |
| 冻结当前图表 | 9.19% | 7.25% | 0.499 / 0.666 | 0.464 / 0.585 | 240.76 |

关闭探针时，当前轨道场景 CPU 从 25.49% 单核降到 11.78% 单核。探针仍有观测开销，因此该结果与开探针场景分列。

全部四轨第 1 轮：主线程平均 4.350 → 1.203 ms，chart-refresh 平均 3.846 → 0.872 ms，包络平均 0.258 → 0.058 ms。峰值 Private Bytes 为 297.15 → 296.24 MiB，历史容量和内存规模基本相同；这是减少重复工作的收益。

正式帧工作量：

| 指标，全部四轨第 1 轮 | 原实现 | 新实现 |
|---|---:|---:|
| 历史搬移 | 196188487680 字节，约 5.22 GiB/s | 0 |
| 新样本复制 | 215040000 字节 | 215040000 字节 |
| 包络索引全量扫描 | 1705574400 点 | 0 点 |
| 包络索引增量扫描 | 不适用 | 1790656 点 |
| 包络查询边缘原始样本扫描 | 0 点 | 50290368 点 |

新旧均记录到 32 组来源/方向/声道的新样本复制工作，且复制总量一致。保留原显示边界规则需要额外扫描包络查询边缘，但总耗时仍显著下降。正式窗口中未发生全量索引扫描，是因为预热期间已经完成首次建索引；首次显示或需要重置时仍会全量建立。

## 核心微基准

每组 3 轮，表中取每轮平均耗时的中位数；单位 μs。800 帧增量，历史每声道 491520 点。

| 场景 | 原实现 | 新实现 | 变化 |
|---|---:|---:|---:|
| 八声道 push + 历史刷新 + FFT / 声谱 | 1724.836 | 441.789 | -74.4% |
| 八声道 push + 仅波形刷新 | 1289.879 | 46.082 | -96.4% |
| 单声道移动包络 | 337.465 | 65.778 | -80.5% |
| 单声道冻结包络查询 | 21.460 | 34.318 | +59.9% |

移动包络新夹具实际覆盖 800 个样本并推进环形起点；旧夹具通过推进 revision 强制重建。冻结新夹具查询环形存储，旧夹具查询连续存储。冻结微基准约增加 12.9 μs，不能宣称每种查询都更快；现有连续缓存门槛通过，完整 GUI 冻结场景 p95 与 CPU 均下降。取舍是保留显示边界语义，并在物理边界做少量查询处理。

当前全部四轨 chart-refresh 的平均耗时中，FFT 约 0.459 ms，声谱历史约 0.208 ms，两者约占 77%。历史搬移消除后，剩余主要工作已转向这些原有计算；本次未扩大到 FFT 或声谱存储优化。

## 发布版本与现场测量的边界

上述 GUI 数值来自原工作区，包含当时尚未提交的界面调整；`manifest.json` 和 `metrics.json` 保留该现场的源码散列与原始统计。发布分支仅移植波形历史、包络索引及必要的绘制读取改动，未包含其他界面调整。因此这些 GUI 数值是该现场的前后对照，不能直接等同于发布分支所有界面布局的性能承诺。

发布分支的历史刷新、包络、历史视图和对应测试文件与被测实现逐字节相同；AppUi 只移植相关调用。原工作区的完整 GUI 复现夹具仍保存在本机 `.scratch/profile-ring-2026-11-08/` 对应的工作区中，本 PR 只携带可独立构建的核心微基准。发布版本源码身份见 `publication-manifest.json`，并在干净检出中重新构建 GUI、运行相关测试。

## 证据与复现

- 完整统计：`metrics.json`、`gui-summary.csv`、`core-summary.csv`；源码和二进制散列：`manifest.json`。
- 原始 trace、测试 XML、红灯记录、构建日志及修改前快照：仓库 `.scratch/profile-ring-2026-11-08/`。
- 旧基线的相关统计已嵌入 `metrics.json` 的 `baseline_core`、`baseline_gui`；完整旧报告和原始测量留在本机 `docs/measurements/2026-11-06-full-profile/`、`.scratch/profile-2026-11-06/`。

```powershell
cmake --build build --config Debug --target WinAudioTests WinAudioGui --parallel 8
cmake --build build --config Release --target WinAudioTests WinAudioGui --parallel 8
.\build\bin\Debug\WinAudioTests.exe --gtest_filter="WaveEnvelope.*:ChartDataPipeline.*:ChartsFreezePolicy.*:ChartsTimeZoomPolicy.*:CaptureChannelView.*:Spectrogram.*:ScopeBuffer.*:FakeScopeReader.*"
.\build\bin\Release\WinAudioTests.exe --gtest_filter="WaveEnvelope.*:ChartDataPipeline.*:ChartsFreezePolicy.*:ChartsTimeZoomPolicy.*:CaptureChannelView.*:Spectrogram.*:ScopeBuffer.*:FakeScopeReader.*"
cmake -S docs/measurements/2026-11-08-wave-history/tools -B .scratch/profile-ring-2026-11-08/profile-build
cmake --build .scratch/profile-ring-2026-11-08/profile-build --config Release --target CoreProfile --parallel 8
.scratch\profile-ring-2026-11-08\profile-build\Release\CoreProfile.exe .scratch/profile-ring-2026-11-08/core-raw.csv > .scratch/profile-ring-2026-11-08/core.csv
```

本报告保留发布前的现场测量结果；远程 CI 与合并结果由对应 PR 记录。
