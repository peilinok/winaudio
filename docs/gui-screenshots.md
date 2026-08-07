# GUI Screenshots

这些截图来自 WinAudio GUI 的真实运行状态，用于展示主要工作模式下的波形、声谱图、状态与日志区域。

## Monitor

双流延迟监听模式同时展示采集端与渲染端的波形和声谱图。

![Monitor running](images/winaudio-monitor-running.png)

## Loopback

系统 loopback 模式回采当前 render endpoint 的系统输出。

![Loopback running](images/winaudio-loopback-running.png)

## Application Loopback

Application Loopback 按 PID 做 process loopback：默认 IncludeTree（目标进程树），可选 Exclude（ExcludeTree，混音去掉该进程树）。详见 README Application Loopback 小节。

![Application Loopback running](images/winaudio-application-loopback-running.png)
