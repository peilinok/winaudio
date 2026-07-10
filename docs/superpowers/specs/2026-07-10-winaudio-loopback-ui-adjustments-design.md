# WinAudio Loopback UI 调整设计

## 范围

本阶段只做两个聚焦调整：

- 将 `Monitor` 和 `Loopback` 两个 tab 内的日志面板从左侧控制栏移动到当前 tab
  底部，让日志使用整个 tab 宽度。
- 将 System Loopback 的 silent render 辅助链路暴露为可见 demo 控件，默认开启，
  同时保留现有 idle zero-fill 兜底。

本设计不拆分每个 tab 的独立日志，不增加可拖拽 splitter，不改变普通 Monitor
capture/render 监听链路。

## 当前上下文

GUI 当前把每个 tab 画成左右两个横向 child：

- `Monitor`：`left` 固定 360 px，`charts` 占剩余宽度。
- `Loopback`：`loopbackLeft` 固定 320 px，`loopbackCharts` 占剩余宽度。

两个左侧面板都会调用 `drawLogPanel(...)`，所以日志历史被限制在很窄的控制栏里。
`logLines_` 是 GUI 全局日志历史，当前由两个 tab 共用。

System Loopback 现在使用 `WasapiSystemLoopbackCaptureStream`，在 render endpoint 上用
`AUDCLNT_STREAMFLAGS_LOOPBACK` 打开 capture client。当前实现还会在 capture 等待超时后
向 ring 写零帧，让 UI 在没有 packet 到达时仍能继续推进。

## WASAPI 事实依据

Microsoft 文档把 loopback recording 描述为从 render endpoint 的 output stream 捕获系统混音，
并说明针对较旧 Windows 行为，应用可以通过初始化一个 render stream，并使用 render stream
的事件通知来规避 event-driven loopback 信号问题。

`IAudioRenderClient::ReleaseBuffer` 支持 `AUDCLNT_BUFFERFLAGS_SILENT`，用于告诉 audio
engine 将提交的 render packet 视为静音。这是创建 silent render stream 的合适基础，
可以保持 render path 活跃而不产生可听样本。

参考：

- https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording
- https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiorenderclient-releasebuffer

endpoint mute、system mute、没有活跃应用播放、不同驱动 endpoint 的具体行为仍需要在目标
Windows 机器上实机验证。

## UI 布局设计

每个 tab 改成纵向页面结构：

1. 上方内容区：占用除日志区外的剩余高度。
2. 底部日志区：全宽显示日志。

上方内容区继续保留现有左右两列：

- 左列保留控制和状态。
- 右列保留 waveform/spectrogram 图表。

`drawLeftPanel()` 和 `drawLoopbackLeftPanel()` 不再绘制日志，只保留设备选择、控制和状态。
`drawMonitorPage()` 与 `drawLoopbackPage()` 负责 tab 级纵向布局，并在上方内容 row 之后调用
`drawLogPanel()`。

底部日志高度先使用固定值 200 px。这样改动小且行为可预测。可拖拽高度调整本阶段不做。

日志历史继续共享。切换 tab 只切换页面内容，不切换底层日志 buffer。

## Silent Render 设计

System Loopback 增加一个可见 silent render 控件：

- GUI：`Loopback` 页放一个 checkbox，默认开启。
- CLI：`--loopback` 默认开启 silent render；新增 `--no-silent-render` 用于关闭。

这个控件需要暴露出来，因为 WinAudio 是 demo 工具。用户应能对比开启和关闭 helper render
stream 后的 loopback 行为差异。

System Loopback 启动时，如果 silent render 开启，WinAudio 会在同一个 loopback render
endpoint 上启动一个 shared-mode render stream，并用 `AUDCLNT_BUFFERFLAGS_SILENT` 提交静音
buffer。该 stream 跟随 loopback session 一起停止和关闭。

Silent render 只作用于 System Loopback capture，不改变普通 endpoint capture、普通 playback，
也不改变 Monitor tab 的延迟播放路径。

UI 和日志需要让状态可观察：

- 是否请求 silent render。
- silent render 是否启动成功。
- silent render 启动失败原因。
- idle zero-fill fallback 是否发生过。

## Idle Zero-Fill 兜底

现有 idle zero-fill 路径保留，但角色收窄：

- Silent render 是保持 WASAPI render path 活跃的主要机制。
- Idle zero-fill 只是没有 capture packet 到达时的 UI 连续性兜底。

这个区分对 demo 很重要。Silent render 展示 Windows audio 链路保活做法；idle zero-fill 只是在
capture 侧空闲或平台行为不同的时候避免可视化完全冻结。

## 错误处理

如果 silent render 开启但启动失败，只要 loopback capture stream 本身启动成功，System Loopback
仍允许继续运行。失败需要体现在日志或状态中，因为这是 demo 细节，不是 capture 的致命错误。

如果 loopback capture stream 启动失败，沿用现有 fatal start failure 行为。

如果 silent render 关闭，则不创建 helper render stream。用户可以观察当前 idle/no-packet 行为，
但现有 zero-fill fallback 仍保留。

## 测试与验证

自动化检查覆盖：

- CLI 解析：loopback 默认开启 silent render，`--no-silent-render` 能关闭。
- 核心链路：System Loopback 启动时，开启状态会请求 silent render，关闭状态不会请求。
- 失败行为：helper render 启动失败会被记录或暴露，但不会让成功的 capture start 失败。
- 现有 idle zero-fill helper 测试继续有效。

GUI 验证覆盖：

- Debug 构建 `WinAudioGui`。
- 可用时运行现有 GUI smoke。
- 手动确认两个 tab 都显示底部全宽日志区，上方内容仍保持现有左右两列结构。

手动音频验证覆盖：

- 有正常系统播放时启动 System Loopback。
- 没有活跃播放时开启 silent render 启动 System Loopback。
- 没有活跃播放时关闭 silent render 启动 System Loopback。
- 在目标 Windows 机器上验证 endpoint mute 和 system mute 场景。

## 非目标

- 每个 tab 独立日志历史。
- 可拖拽日志高度或持久化布局设置。
- Application/process loopback。
- 改变 Monitor tab playback 语义。
- 把 silent render 隐藏成纯内部默认行为。
