# WinAudio GUI 重构 — 默认监听 + 实时播放开关 + 两列可重排布局 设计文档

**日期**：2026-07-02 ｜ **v2**（经 agent-team 评审修订：并发视角 opus + 需求/UX 视角 sonnet；codex 超时未出）
**目标**：GUI 去掉三 mode 切换，恒为监听模式；"是否同步播放"由 checkbox 实时控制（勾选=经延迟播放，取消=停播并释放渲染设备）；界面改为两列——左列功能/设备/日志，右列 waveform/spectrum/spectrogram 图表且**可拖拽重排**。

> **v2 修订要点（评审发现）**：① `renderScope_`/`renderRing_` 必须**全会话存活**（start 建、仅 teardown 释放），engage/disengage 绝不动这两个指针——否则 GUI 每帧 `snapshotRender` 会 UAF（并发 Critical）。② `overall_`（引擎/采集态）必须与"渲染预填就绪"**解耦**——否则采集-only 时 `overall_` 永远卡 Idle。③ 渲染相关尺寸/`DelayFifo` 依赖渲染设备，须在 engage 时（重）推导/（重）建。④ `engageRender()` 返回 `Result` 且失败原子（全关渲染），start 失败→整体回滚、运行期失败→仅播放关采集续。⑤ AppUi 需自持 `DeviceEnumerator`（`Engine` 删除后设备枚举无来源）。

## 1. 范围

**做：** AppUi 去 mode/单流、恒监听、两列布局、右列 6 图可拖拽重排、"同步播放" checkbox（实时）；MonitorEngine 运行期播放开关 `setPlaybackEnabled(bool)`（A2：采集恒运行，渲染运行期开/停并释放设备）；CLI `monitor` 行为不变。

**不做 / 收窄：** GUI 变 **monitor-only**——移除 GUI 录 WAV / 播 WAV（连同 `Engine engine_`、`drawSingleStream`、单流状态）；这些能力仍在 CLI。不引入 ImGui docking（重排手写）；图表顺序不持久化；不改 DSP/分析算法。

**约束（保持）：** Core 纯 Win32+STL、不跨 API 抛异常、状态用原子、音频路径无 mutex、帧对齐 ring I/O、无重采样器、采集==渲染采样率（渲染开启时校验）。/W4 clean。CMake（`build.bat`/`test.bat`）。

## 2. MonitorEngine：运行期播放开关（A2）

### 2.1 语义与对象生命周期（关键）
- **采集恒运行**：`start` 成功后采集流 + `captureScope_` + `capLevel` 始终工作，与播放开关无关。
- **渲染按需**：渲染流仅在"播放开启"时存在——开渲染设备、校验采样率==采集、（重）建并预填 `DelayFifo`、进入 capture→FIFO→pop→`renderRing_`+`renderScope_` 全监听；"播放关闭"时停并**关渲染后端、释放设备**，回采集-only。
- **对象生命周期（防 UAF，评审 Critical）**：
  - **全会话存活**（在 `start()` 分配，**仅** `teardown()`（pump join 后）释放，engage/disengage **绝不** 触碰其指针）：`captureRing_`、`captureScope_`、`renderRing_`（固定 1 MiB，与格式无关）、`renderScope_`。理由：GUI 线程**每帧**调 `snapshotRender()`/`renderWritten()`（读 `renderScope_`）；若 disengage 释放/重建它就是数据竞争+UAF。播放关时"播放侧图表变空"由 **GUI 依 `renderState` 门控绘制**实现（见 §3.3），**不是**清空/释放 scope。
  - **每-engage（pump 线程私有，可重建）**：`delayFifo_`（尺寸依渲染设备周期，engage 时 `make_unique` 重建——`DelayFifo` 无 reset()）、渲染刮擦缓冲（`renderAdapt_/renderMono_/renderBytes_`）、`renderFmt_/renderCh_/renderFrameBytes_/renderBufMs_`（engage 打开渲染后推导）。仅 pump 触碰；`poll()` 只读 `fifoFillMs_` 等原子，不碰对象。
- **状态解耦（评审 Important）**：
  - `overall_` 以**采集**为准——采集起来即 `Running`（不再依赖 FIFO 预填；采集-only 时 FIFO 不推不弹，旧的"预填后才 Running"逻辑对采集-only 会永远卡 Idle）。
  - `renderState_` 表达渲染真实态：Idle（播放关）/ Running（该次 engage 预填完成、pop→render 已开始）/ Error（engage 时采样率不匹配）。用**每-engage 的独立预填标志**驱动，不再复用全局 `prefilled_`、不扰动 `overall_`。
  - 采集-only 期间：`renderState=Idle`，`renderXruns/renderLevel/renderBufMs=0`，`fifoFillMs` 在 `delayFifo_` 缺席时读 0（守卫）。
- **采样率校验时机**：从 `start` 移到**渲染开启时**。不匹配 → engage 失败。

### 2.2 接口
```cpp
void setPlaybackEnabled(bool enabled);   // GUI 线程；仅设原子意图 wantPlayback_，实际开/停由 pump 执行，结果经 poll() 反映
Result start(BackendKind kind, const DeviceId& capId, const DeviceId& renderId,
             uint32_t delayMs, bool playbackEnabled = true);  // 第5参默认 true = 现行为，CLI 不用改
// 私有：Result engageRender();  void disengageRender();
```
`MonitorStatus` 字段不变。

### 2.3 线程模型与 race-freedom（含 renderScope）
- **谁碰什么**：
  - `renderBackend_` + 每-engage 对象（`delayFifo_`、渲染刮擦、`renderFmt_` 等）：**只**由 (a) `start()` 内 pump 启动**之前**、(b) **pump 线程**触碰——二者时序不重叠（start 先开/建 → 启 pump；pump 初始化本地 `renderActive_` 匹配 start 是否已 engage；此后运行期 toggle 全在 pump）。GUI 线程从不碰这些。
  - `renderScope_`/`renderRing_`：**全会话存活**，唯一生产者=pump，唯一消费者=GUI（沿用 `ScopeBuffer` seqlock / `RingBuffer` SPSC 契约）。engage/disengage 不重分配它们，故 GUI 每帧 `snapshotRender()` 安全（对象恒在）。
  - `poll()`：只读原子，从不碰 backend/ring/fifo/scope 对象指针。
- **`setPlaybackEnabled`**：只设原子 `wantPlayback_`。pump 每轮比较 `wantPlayback_` 与本地 `renderActive_`：需开未开→`engageRender()`；需关在开→`disengageRender()`。toggle 在 ≤20ms 的 pump wait 内被观察到。
- **`engageRender()`（返回 Result，失败原子）**：开渲染后端 → 读 `renderFmt_`、校验 ==采集采样率 → 推导渲染刮擦/`renderBufMs_`、依渲染周期 `make_unique<DelayFifo>` 并预填 `delayMs`（+≥1 render period）静音 → 置 `renderState=Running`、`renderActive_=true`。**任一步失败**：完全 stop+close 渲染后端（释放设备）、`renderActive_=false`、不留半开态、返回 Fail。
  - **失败策略不对称**：`start()`（playbackEnabled=true）调用 engage 失败 → 走现有 `rollback()`（**停采集、返回 Fail、状态回 Idle/Error**，CLI 语义：不匹配即整体启动失败、`capState=Idle`）；**pump**（运行期 toggle）engage 失败 → `renderState=Error`+`errorCode`+`wantPlayback_=false`，**采集继续**（不因一次播放尝试失败中断稳定采集）。
- **`disengageRender()`**：stop+close 渲染后端（释放设备）、`renderActive_=false`、`renderLevel_=0`、`renderState=Idle`。**不**触碰 `renderScope_`/`renderRing_` 指针（保持存活，pump 停止向其推数据即可）。
- **pump 的 COM**：pump 现在承担渲染 COM 生命周期（close→Release），给 pump 线程自带 `ComInitGuard`（MTA）——明确而非依赖偶发（现状 teardown 亦从非-COM 线程 Release，但 pump 显式持有更稳妥）。
- **teardown / stop**：不变——`running_=false`→唤醒 pump→**join pump**→停采集→（若在）停渲染→释放全部（含全会话 scope/ring）。任何播放态下都安全；二次 stop 幂等；`if (renderBackend_)` 守卫处理已 disengage（null）态。

### 2.4 测试（fake backend，无硬件）
- `PlaybackStartsDisabled`：`start(...,false)` → `capState/overall=Running`（采集态，**非** Idle）、`renderState=Idle`、fake 渲染后端未 open。
- `EnablePlaybackEngagesRender`：其后 `setPlaybackEnabled(true)`+推数据 → `renderState=Running`、fake 渲染后端 open、`snapshotRender` 有数据。
- `DisablePlaybackStopsRender`：运行中 `setPlaybackEnabled(false)` → `renderState=Idle`、fake 渲染 stop 标志置位（设备释放）、`overall/capState` **仍 Running**（采集续）、且**此后 `snapshotRender()` 仍可安全调用**（对象存活，返回 false/空）。
- `EnablePlaybackRateMismatch`：fake 采集 48000 / 渲染 44100，`setPlaybackEnabled(true)` → `renderState=Error`、`errorCode=RateMismatch`、播放保持关、采集继续。
- 回归：现有测试（4 参 `start` 默认 true）语义不变。**`RateMismatchFails` 增断言：start 失败后 `capState=Idle`（采集已回滚）**。

## 3. AppUi：两列布局 + 拖拽重排

### 3.1 移除与新增成员
- **删**：`Engine engine_`、`drawSingleStream`、`refreshDevices`(单流)、`modeIdx_/prevMode_`、`deviceIdx_/devices_/devicesLoaded_`、`wavPath_`、`rateIdx_/bitsIdx_/chIdx_/isFloat_`、Probe；**死代码**：`utow()`（仅单流用；`wtou()` 保留）、文件顶 `kRates/kRatesS/kBits/kBitsS/kChans/kChansS` 数组；`AppUi.h` 的 `#include "Engine.h"`（`BackendKind` 经 `MonitorEngine.h` 传递可得）。
- **加**：`wa::DeviceEnumerator enumerator_;`（`Engine` 删后设备枚举来源；`refreshMonitorDevices()` 改用 `enumerator_.enumerate(flow, vec)`）+ `#include "DeviceEnumerator.h"`。`stopAll()` 只停 `monitor_`。`main.cpp` 不变。

### 3.2 状态新增
```cpp
bool playbackEnabled_ = false;               // "同步播放" checkbox；变更即 monitor_.setPlaybackEnabled(...)
std::vector<int> chartOrder_ = {0,1,2,3,4,5};// 面板顺序，可拖拽 splice 重排
// id: 0=采集波形 1=播放波形 2=采集频谱 3=播放频谱 4=采集声谱图 5=播放声谱图（默认同类相邻）
```
（分析缓冲 `capWave_/renderWave_/workCap_/magCap_/capSpec_/...` 保留。）

### 3.3 布局
- `draw()`：`SetNextWindowSizeConstraints(ImVec2(800, 400), ImVec2(FLT_MAX,FLT_MAX))`（两列最小宽，具体值）；`Begin("WinAudio")` 内左右两个并排 `BeginChild`：
  - **左列**（固定宽 ~360px），`SeparatorText` 分区：① 设备（采集 listbox + 渲染 listbox + Refresh；**两个 picker 始终可交互**，Start 前预选、勾选播放时生效）② 控制（Backend combo、Delay 滑块、Start/Stop、**"同步播放" checkbox**——未 Start 时禁用；运行中变更即 `setPlaybackEnabled`）③ 状态（overall/cap/ren、sr、fifo/drift/xrun、cap/ren 电平条）④ 日志（`BeginChild` 用**剩余高度填满**，不硬编码 120px）。
  - `SameLine();` **右列**（`BeginChild` 填满）：按 `chartOrder_` 顺序渲染 **6 个面板框架**（含拖拽手柄），**始终渲染**（未 Start / 未 engage 时面板体为空图，允许 Start 前预排序）。
- **拖拽重排**：每面板顶部手柄（`Selectable`/`Button`）作 `BeginDragDropSource`（payload=其在 `chartOrder_` 的索引），其余面板 `BeginDragDropTarget` 接收 → 对 `chartOrder_` 做 **splice/插入式移动**（把源项移到目标位、其余平移），**非** swap。面板体按 id 调用对应绘图（复用现有 ImPlot 波形/频谱/声谱图代码，抽成按 id 的小函数）。
- **播放侧图表门控**：波形/频谱/声谱图的"播放"三图仅在 `ms.renderState==Running` 时绘制数据线；否则显示空轴。且当 `renderState` 由 Running→非Running（disengage）时，GUI **重置播放侧分析态**（`magRender_` 清空、`renderSpec_` clear/重建、`nextRenderEnd_=0`），使播放图变空而非停留旧帧。
- 采集侧三图：只要 `overall==Running && sr>0` 即持续绘制（采集恒运行）。

### 3.4 交互
- Start：`monitor_.start(kind, capId, renId, delayMs, playbackEnabled_)`；成功后重置采集侧游标（`nextCapEnd_=0` 等）。
- 运行中切 checkbox → `monitor_.setPlaybackEnabled(...)`，实时生效；结果经 `poll()` 的 `renderState`/`errorCode` 反映（勾选出声、`ren=Running`；取消 `ren=Idle`；不匹配 `ren=Error`）。

## 4. 成功标准
- 无 mode 选择器；启动即监听；采集恒可视化（`overall=Running`，**非**卡 Idle）。
- 两列布局；右列 6 图可拖拽 **splice 重排**、顺序稳定；采集/播放同类图可拖到相邻（纵向相邻 + 共享 `waveX_` 同时间轴即满足"对照"，见 §5）。
- "同步播放" checkbox 实时生效：勾选→出声 + 播放侧图有信号 + `ren=Running`；取消→停声 + 释放渲染设备（`ren=Idle`）+ 播放侧图**变空**（非旧帧）；反复切换不重启、不崩溃、**不 UAF**、不死锁。
- 采样率不匹配时勾选→`ren=Error`、播放保持关、采集继续；start-with-playback 不匹配→整体启动失败（`capState=Idle`）。
- MonitorEngine 新增 4 项 `setPlaybackEnabled` 单测 + 回归（含 `RateMismatchFails` 的 `capState=Idle` 断言）通过；`/W4` clean；`build.bat`/`test.bat` 双配置构建 + 全测试通过；GUI 存活。（波形运动/声音/拖拽等目视项有硬件桌面时人工确认。）

## 5. 风险与对策
- **`renderScope_` UAF（Critical，已在 §2.1/§2.3 消除）**：全会话存活 + engage/disengage 不动其指针 + GUI 依 `renderState` 门控。这是本设计最关键的不变量。
- **`overall_` 卡 Idle（已消除）**：`overall_` 改以采集为准，与渲染预填解耦。
- **engage 阻塞采集 drain**：`engageRender` 的渲染 `start()` 会阻塞 pump 数十~数百 ms（独占模式对齐重试更久），期间不排空 `captureRing_`（1 MiB≈2.7~5.5s 余量，不 overrun），采集可视化会短暂冻结后跳变——可接受，记为已知。
- **engage 失败原子性**：`engageRender()` 失败必须全关渲染、无半开；start→rollback、pump→Error+采集续（§2.3）。
- **拖拽无自动滚动**：ImGui `BeginDragDropTarget` 在 child 内不自动滚动，目标须在可视区内——不实现自滚（ImGui 限制，可接受）。
- **CLI 回归**：`start` 第5参默认 true → 4 参调用语义不变；现有 MonitorEngine 测试不受影响。
- **对照需求**：右列纵向相邻 + 两波形同 50ms 窗口/同 `waveX_` 时间轴即可对照；水平 2-up 不在范围内。

## 6. 交付物
改：`src/core/MonitorEngine.h/.cpp`（setPlaybackEnabled + engage/disengageRender + 生命周期解耦 + start 增参 + pump ComInitGuard）、`src/tests/test_monitorengine.cpp`（+4 测试 + RateMismatch capState 断言）、`src/gui/AppUi.h/.cpp`（去单流+死代码、DeviceEnumerator 成员、两列、拖拽 splice 重排、播放 checkbox、renderState 门控 + disengage 重置播放侧分析）。`src/cli/main.cpp`、`src/gui/main.cpp` 预期不改（如编译需要作最小调整）。
