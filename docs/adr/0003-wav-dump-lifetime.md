---
status: accepted
---

# WAV Dump 由原子准入协议保护，文件收尾由 writer 完成

WAV Dump 可以在 Track 持续运行时开始或结束。我们在现有 WavSink module 内集中准入、在途写入、排空、文件收尾与回收：使用带代次的原子准入门，保留单生产者环形缓冲和专用 writer；控制线程独占启动、等待与回收，pump 只提交数据或请求停止。这个选择允许独立结束 Dump，同时让音频热路径保持无阻塞锁、无堆分配、无文件操作。

## 考虑过的方案

- **先停止整个 Track，再结束 Dump**：回收容易证明，但破坏独立停止 Dump 的产品契约。
- **用 mutex 保护每次 push，或每块加载 shared_ptr**：会给生产路径引入阻塞或共享所有权开销，也没有自行解决 writer 提前判空的问题。
- **让 pump 执行完整的启动、停止与回收**：控制归属集中，但文件打开、排空和关闭会拖住 Monitor 的另一侧。
- **只加一个在途计数或保留原子 running 标志**：分开的检查与登记不能形成原子准入；快速重启还可能接受上一轮的迟到操作。

## 影响

- 正常结束的顺序固定为关闭准入、在途退出、排空、关闭文件、writer 退出、回收。错误结束仍须等待在途退出和 writer 退出，但不承诺故障后的数据完整。
- #234 把 render 自动停用改为只请求结束 Render Dump，并让界面单独呈现“正在收尾”。显式停止 Dump 与 Monitor stop 仍等待文件关闭；后者先 join pump，再完成两个 sink。
- Dump 代次与来源代次分别校验；来源停止请求能取消正在启动的 Dump。Monitor capture 按 session、render 按 engage 换代，实际 render 格式使用六个原子字段与前后有效版本检查，重新启用 render 不续写旧文件。
- 文件状态、代次、来源、路径与最终结果经同一元数据锁读取为同轮快照；pump 不取该锁。GUI 按代次消费成功完成，失败与取消不揭示文件。
- 控制操作可以等待文件系统；不为 start 或 stop 承诺磁盘无关的固定完成时限。
- 详细契约和实施验收见[设计文档](../superpowers/specs/2026-10-24-wav-dump-lifecycle-design.md)。#233 的 WavSink 协议与 Capture Track 已实施，记录见[#233 执行记录](../measurements/wav-dump-233/execution.md)；#234 的 Monitor 来源与 GUI 接线已实施，当前验证边界见[#234 执行记录](../measurements/wav-dump-234/execution.md)。
