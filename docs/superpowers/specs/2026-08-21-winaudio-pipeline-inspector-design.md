# WinAudio Pipeline Inspector 设计

日期: 2026-08-21 · 状态: 2026-08-25 grill 已锁定（ADR-0002）· 范围: Core + GUI 新 tab；**CLI is unchanged**

## 1. 产品定位

Pipeline Inspector 重建一条捕获或播放路径（hardware mic → app，或 app → hardware）。

- **Live session**（混音器行）用来发现谁在开麦/播放。
- **On-demand attach** 之后，观察单元细化为该进程里的 **Hooked stream**（一条 `IAudioClient`）。
- 在目标 **应用进程** 内拦截 **Core Audio intercept** 闭集；**不**拦截 audiodg 里的 APO COM。
- 呈现：**Pipeline 图 + Call log**。

它 **不是** 从本进程去 `GetService` 对方的 client。  
它 **不是** 枚举 audiodg 里的 APO 实例，也 **不是** 系统级 API Monitor。

架构取舍见 `docs/adr/0002-pipeline-core-audio-intercept.md`。用词见 `CONTEXT.md`。

每个处理图节点必须带一种 Observation kind：

| Kind | 含义 |
|---|---|
| Observed | 公开 API、已对齐 ETW，或 Hooked stream 上拦到的 COM 实参 |
| Probed | 本工具在同设备上另开 Shared 流测到的（不是对方那条流） |
| Inferred | 由模式规则推出（例如 RAW ⇒ SFX 不加载） |
| Skipped | 有证据表明这一段没跑（Exclusive、SysFx 关、RAW 的 SFX） |
| Unknown | 没有 API / 事件，禁止猜测填值 |

UI 不得把 Probed 画成「Chrome 实际走过的 APO」。**禁止把 Shared probe 的效果类型呈现为被观察 App 的实际 APO 实例。**

## 2.1 能拿到 vs 拿不到

**能拿到（v1）：**

- PID、进程名、data flow、session 音量 / 静音 / 状态（Live session）
- mix / device / OEM 格式；已注册 SFX / MFX / EFX CLSID；硬件 volume / mute / AGC（拓扑暴露时）
- Shared probe 广告的效果类型（未 attach 时，Probed）
- ETW 对得上时的 category / RAW / HRESULT（attach 前）
- **On-demand attach 之后**：Core Audio intercept 的控制路径实参（`Initialize` 的 format/share/RAW/category、`Start`/`Stop`、session/volume/clock、`IAudioEffectsManager`）为 Observed

**拿不到：**

- audiodg 里的 APO COM 实例、EQ 曲线、NS 强度、`APOProcess` 实参
- attach 之前的调用栈（只能靠 ETW/探针补洞）
- 跨位数目标、无调试权限时的挂钩
- XAudio2 / DirectSound / WinRT 入口（仅当它们在本 PID 落到 WASAPI 时才会出现在日志里）
- 应用私有 DSP；泵 PCM

## 2.2 v1 规则（硬约束）

- **On-demand attach**：用户选中 Live session 后注入该 PID；同位数；GUI 默认不抬权；缺调试权限 **fails closed**。不自动注入、不挂起启动目标。
- **Core Audio intercept**：Activate、`IAudioClient*`、capture/render client、`IAudioEffectsManager`、session control、stream/channel volume、clock。不钩 audiodg，不钩 XAudio2/DS/WinRT 入口。
- **Call log**：控制路径保留；泵默认关；打开后走小环形缓冲 + xrun 计数；永不记 PCM。
- **Shared probe**：未挂钩时用；Default / Communications / Raw；Initialize then `IAudioEffectsManager`；**no Start**；**never Exclusive probe**。
- **ETW Initialize**：attach 前补洞；PID + 短时间窗 + device id；对不上 Unknown。无 ETW 权限则该层缺席，其余仍工作。
- **CLI is unchanged**。

## 3. 非目标（v1）

- 钩 audiodg / `IAudioProcessingObject`；系统级 API Monitor；跨位数注入助手。
- 枚举 APO 运行实例、内部旋钮；Exclusive 探针；打断并重开对方的流。
- 热路径逐帧 ETW；CollectAudioLogs；空间音频对象级参数；CLI 子命令。

## 4. 五层数据，合成图 + 日志

缺一层时其余层仍工作：

1. **Live session watch**（capture and render）
2. **Endpoint graph**（格式、SysFx、SFX/MFX/EFX CLSID、硬件旋钮）
3. **Shared probe**（未 attach）
4. **ETW Initialize** hints（attach 前）
5. **Core Audio intercept**（On-demand attach → Hooked stream + Call log）

`assemblePipeline`（`src/core/PipelineGraph.*`，windows-free）把快照合成带 Observation kind 的节点。Call log 是旁路时间线，不替代图。COM / ETW / 注入适配器只负责填快照。

### 4.1 Live session（Observed）

对 **capture 与 render** 每个 active endpoint 注册 `IAudioSessionNotification`，并枚举已有 session。

每行：

- PID、进程名、device id / friendly name、data flow（capture | render）
- session 状态、session 音量 / 静音
- session instance id（关联 ETW 用，能拿到就 Observed）

本仓库现有 `AudioSessionEnumerator` 只服务 Application Loopback（render、按 PID 去重）。Inspector 需要 **按 session 实例、含 capture**，不得改坏 Application Loopback 的去重语义；新类型放在 Pipeline 模块。

### 4.2 Endpoint graph（设备级，与 session 无关）

对选中 session 的 endpoint：

- Mix / Device / OEM 格式（复用 Capabilities 的三来源，不新开采集流）
- `PKEY_AudioEndpoint_Disable_SysFx`
- FX store：`PKEY_FX_StreamEffectClsid` / `ModeEffectClsid` / `EndpointEffectClsid` 及支持的 processing modes
- `IDeviceTopology` 可 QI 到的控制：音量、静音、AGC、bass/treble（有则 Observed 参数；没有则不下发空旋钮）

硬件拓扑参数是该 endpoint 上所有共享流真正经过的，对「别人的麦」也成立。

### 4.3 Shared probe（Probed）

选中一个 session 后，本工具在 **同一 endpoint** 上依次打开短寿命 WASAPI Shared 流（Initialize + `GetService(IAudioEffectsManager)` + 关闭；**不 Start**，避免占缓冲抢时序）：

| 探针标签 | SetClientProperties |
|---|---|
| Default | 不启用 client properties |
| Communications | category = Communications |
| Raw | option = Raw（设备不支持则该切片失败，节点记 Unknown + 失败原因） |

约束：

- 仅 Shared；Exclusive 请求直接 Fail，禁止降级去抢设备。
- 探针失败（含对方 Exclusive 占设备）不得拆掉 session 雷达。
- 探针流必须能从会话列表里识别为自己，UI 默认隐藏本进程，可勾选显示。

得到的是 **效果类型 GUID + ON/OFF + canSetState**，不是 APO CLSID。

### 4.4 ETW Initialize（Observed，失败则整层缺席）

v1 订阅（实时 session，尽量不写 etl 文件）：

1. **Microsoft-Windows-Audio** `{AE4BD3BE-F36F-45B6-8D21-BDD6FB832853}`
   - PlaybackManager 事件 24/25：`AppId`, `PID`, `Category`
   - Performance 事件 123 / 125 / 127 文案含 `category=%2, raw=%3, matchformat=%4`（GetMixFormat / IsFormatSupported / Vadserver_CreateStream）
2. **Microsoft.Windows.Audio.Client** `{6E7B1892-5288-5FE5-8F34-E3B0DC671FD2}` TraceLogging  
   官方措施文档中的 **AudioClientInitialize**（HRESULT）。字段用 TDH 尽力解码；缺字段保持 Unknown。

关联规则（从严）：

- PID + 时间窗（默认 5 s）+ 能解析到的 device id 对齐到 Live session。
- 对不上就不贴，节点保持 Unknown。禁止「最近一条 Initialize 套到当前选中行」。

权限：`EnableTraceEx2` 失败（非管理员 / 非 Performance Log Users）→ Inspector 横幅「ETW unavailable」，其余层照常。不得要求用户重启以开这个功能。

### 4.5 Core Audio intercept（On-demand attach）

用户在雷达中选中一行并 Attach：

- 注入该 PID，位数必须与 GUI 相同；否则失败说明，不降级。
- GUI 默认普通权限；无 SeDebugPrivilege 则 fails closed，图/探针/ETW 仍可用。
- 闭集见 CONTEXT **Core Audio intercept**。控制路径进 Call log（完整参数）。泵默认不记；勾选后进小环 + xrun，无 PCM。
- attach 之后的 COM 实参覆盖 ETW 补丁，标 Observed。
- 不钩 audiodg；不卸载失败时假装已挂钩。

## 5. 图的节点顺序

采集（硬件麦克风 → 软件采集）节点顺序：

`Hardware → Driver/KS → EFX → MFX → SFX → Engine SRC → Session → App`

（mode tee 是 EFX 与 MFX 之间的分路，不单独占一个带参数的节点。）

播放（软件写数据 → 硬件播放）为反向顺序：

`App → Session → Engine SRC → SFX → MFX → EFX → Driver/KS → Hardware`

（mix / mode mix 是引擎混音步骤，合入 SRC/MFX 节点说明，不另开 COM 槽。）

规则：

- Exclusive（仅当 ETW 或其它 Observed 证据说是 Exclusive）：引擎 SFX/MFX/SRC 为 Skipped；硬件节点仍在；EFX 为 Unknown（软件 EFX 通常不在 Exclusive 路径，但不得假装读到了硬件 DSP）。
- RAW Observed：SFX = Skipped（官方：RAW 不跑 SFX）；MFX 在 Win10+ 仍可能加载 → Unknown，除非探针 Raw 切片有效果类型。
- SysFx disabled Observed：SFX/MFX = Skipped；EFX 仍画出注册 CLSID，kind Observed（注册表），是否执行标 Unknown。
- 无 ETW category：MFX/SFX 旁边挂 Default / Communications / Raw 三列探针结果，全部 Probed，标题写明「not this session」。

每个节点的 params 每条也带 kind。禁止把 Mix format 标成「该 App 的流格式」——那是引擎混音格式，除非 ETW 给出了 Initialize 的 WAVEFORMATEX。

## 6. GUI

新 tab：**Pipeline**（文案进 `AppUiText.h`，英文，带文案测试）。

布局：

- 左：Live sessions 表（flow、进程、PID、设备、状态）；Refresh；「Hide this process」默认开；ETW 状态一行。
- 中：选中行的处理图（垂直节点列表即可，v1 不用独立图形库）。节点颜色区分 kind。
- 右或节点展开：参数表（key / value / kind）。
- 「Probe this device」按钮：跑 4.3；运行中禁用该按钮。探针失败写日志，不弹阻塞框。
- 「Attach」：对选中 Live session 做 On-demand attach（4.5）。失败横幅，不伪装挂钩。
- 调用日志栏：控制路径；可选泵小环。与图并排，不替代图。

不改 Monitor / Loopback / Application Loopback 的开流行为。切到 Pipeline tab 不停止其它页的 Track。

## 7. 测试（必须无真实声卡）

- `assemblePipeline`：Exclusive / RAW / SysFx off / 无 ETW / 有 ETW 对齐 / 探针对不上 PID 等夹具。
- ETW 解码：用录好的 `EVENT_RECORD` 字节或字段字典，不 EnableTrace。
- Session 列表排序与 capture+render 共存（不与 Application Loopback 去重逻辑耦合）。
- GUI 文案常量测试。
- 既有测试零回归。COM 适配器可用 fake；CI runner 无音频硬件。

## 8. 日志

凡新增 WASAPI/COM/ETW Win32 调用，按现有 `wa::log` 插桩：EnableTrace / Initialize 探针 / OpenPropertyStore 为 Debug；探针生命周期 Info；热路径不在本功能。core 不 print。

## 9. 分层提交

1. `feat(core): add pipeline graph assembly`（纯模型 + 单测）
2. `feat(core): watch sessions and snapshot endpoint FX`
3. `feat(core): shared effects probe and ETW initialize hints`
4. `feat(gui): add Pipeline inspector tab`
5. `docs: Pipeline Inspector usage and limits`

## 10. 验收

1. 打开 Pipeline tab，任意 App 开麦或播放后出现对应 session（PID + 设备 + flow）。
2. 选中后看到有序节点，每节点有 kind；Probed 与 Observed 视觉可分。
3. Probe 在 Shared 下补出效果类型；对方 Exclusive 时探针失败但雷达仍在。
4. 有 ETW 权限时，能把 category/RAW/HRESULT 贴到对得上的 session；无权限时横幅说明且不崩溃。
5. 不声称列出了该 session 的全部 APO 实例或内部 DSP 参数。
6. `test.bat` Debug + Release 全绿。
