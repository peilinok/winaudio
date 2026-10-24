# GUI 计量链路验收

2026-10-24，源码 `227abba`，Release，机器硬件见 machine.json；本次重新确认平衡电源方案、RTX 2060 驱动 31.0.15.1700、显示 2560×1440／59 Hz。未更改系统设置。初始程序窗口沿用 1280×800 的创建参数，展开日志，无运行中的音频流。Computer Use 确认当前桌面解锁、窗口在前台，三个模式使用相同初始 Monitor 布局。DPI 虚拟化下的实际客户区将由后续图表工作量登记补齐；本票不把窗口创建参数当客户区实测值。

## 真实 GUI 关联

正常启动、切换 Render／Pipeline／System Loopback，再用 Alt+F4 正常退出。2629 帧、无丢失，完整结束标记存在；汇总验证分段掩码、帧号、频率和操作结束 tick 与该帧提交一致。三个操作各自结束于正确的 scene，响应为 675.2915／81.6915／655.7215 ms。输入消息时间包含进入处理前的排队，因此没有把慢操作删掉。这是链路烟测，不是固定负载／100 次操作的性能签收。

## 采集干扰

外部 `sample-process.ps1` 每秒读取进程累计 CPU、Private Bytes、Working Set，预热 10 秒后采样 60 秒；没有在被测进程添加关闭模式计时。启用探针的内部统计各自预热 30 秒、采样 60 秒。外部 CPU 量化和系统调度会带来波动；三模式各运行一次，用于确认开销数量级，不提供统计显著性结论。

| 模式 | 外部 CPU ms/s | 内部帧数 | 主线程 p95/p99 ms | I/O p95/p99 ms | 帧间隔 p95 ms |
| --- | ---: | ---: | --- | --- | ---: |
| 关闭 | 46.374 | 无内部计量 | 无内部计量 | 无内部计量 | 无内部计量 |
| 缓冲写入 | 51.003 | 3597 | 0.3162 / 0.4004 | 0.0318 / 0.0601 | 17.3685 |
| 每帧 flush | 50.755 | 3597 | 0.3039 / 0.3720 | 0.0988 / 0.1589 | 17.3981 |

启用后进程 CPU 比关闭增加约 4.4～4.6 ms/s；本次顺序采样不能把全部差值确定归因于探针。flush 增加 I/O 切片尾部，主线程差异落在此次调度波动范围。后续仍使用缓冲写入，并同时报告 probe-io 与完整帧间隔。空闲场景不足以证明 16 图工作量记录没有干扰，完整负载需再核对；若足以改变性能结论，先修采集再签收。

## 原始证据与复跑

`meter-manifest.json` 登记入库文件 SHA256；三份 `.txt.gz` 无损保存原始字节及 raw_sha256。`gui-meter-smoke.json`、`meter-summary.json` 和进程 CSV 提供派生统计。可用 Python 标准库 `gzip.decompress` 解压后，按 README 的 `summarize.py --require-complete` 命令复核。

三模式分别设置 `WINAUDIO_FRAME_PROBE=0`、`WINAUDIO_FRAME_PROBE=1` 且 `WINAUDIO_FRAME_PROBE_FLUSH=0`、`WINAUDIO_FRAME_PROBE=1` 且 `WINAUDIO_FRAME_PROBE_FLUSH=1`，用不同 `WINAUDIO_FRAME_PROBE_PATH`，正常启动／关闭同一 Release。启用模式用 `--warmup-seconds 30 --duration-seconds 60 --require-complete` 汇总。外部运行 `sample-process.ps1 -ProcessId <本次PID> -Mode <模式> -OutputPath <CSV>`，最后 `summarize_meter.py <目录> --output <JSON>`。

关闭模式不推算零帧耗时；探针默认关闭且不取时由已通过的确定性测试证明。本票完成本地计量链路验收，图表／Inspector 预算和远端 CI 另列。
