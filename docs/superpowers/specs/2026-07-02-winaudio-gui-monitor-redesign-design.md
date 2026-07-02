# WinAudio GUI 重构 — 默认监听 + 实时播放开关 + 两列可重排布局 设计文档

**日期**：2026-07-02
**目标**：GUI 去掉三 mode 切换，恒为监听模式；"是否同步播放"由 checkbox 实时控制（勾选=经延迟播放，取消=停播并释放渲染设备）；界面改为两列——左列功能/设备/日志，右列 waveform/spectrum/spectrogram 图表且**可拖拽重排**。

## 1. 范围

**做：**
- AppUi：移除 Capture/Playback/Monitor mode 选择器与单流 UI；恒为监听；两列布局；右列 6 图表可拖拽纵向重排；新增"同步播放" checkbox（实时）。
- MonitorEngine：新增运行期播放开关 `setPlaybackEnabled(bool)`——采集恒运行，渲染在运行期开/停并释放设备（用户选定的 A2 语义）。
- CLI `monitor` 行为保持不变（`start` 增一个默认参数，默认值 = 现行为）。

**不做（明确排除 / 收窄）：**
- **GUI 变为 monitor-only**：移除从 GUI 录制到 WAV / 播放 WAV 文件（连同 `Engine` 成员、`drawSingleStream`、单流状态）。这些能力仍在 CLI（`capture`/`play`/`probe`）。
- 不引入 ImGui docking（保持非-docking 分支）；重排用手写拖拽。
- 图表布局默认不持久化（不写 imgui.ini 自定义状态）。
- 不改 DSP / 分析算法（波形/FFT/声谱图逻辑照旧，仅移动/条件化其绘制）。

**约束（保持）：** Core 纯 Win32+STL、不抛异常跨 API、状态用原子、音频路径无 mutex、帧对齐 ring I/O、无重采样器、采集==渲染采样率（渲染开启时校验）。/W4 clean。CMake 构建（`build.bat`/`test.bat`）。

## 2. MonitorEngine：运行期播放开关（A2）

### 2.1 语义
- **采集恒运行**：`start` 成功后，采集流 + captureScope + capLevel 始终工作，与播放开关无关。
- **渲染按需**：渲染流仅在"播放开启"时存在——打开渲染设备、校验采样率==采集、预填 DelayFifo、进入 capture→FIFO→pop→renderRing+renderScope 全监听；"播放关闭"时停止并**关闭渲染后端、释放设备**，回到采集-only，renderScope/renderLevel 归零。
- **采样率校验时机**：从 `start` 时移到**渲染开启时**（start-with-playback 或运行期 toggle-on）。不匹配 → `renderState=Error`、`errorCode=RateMismatch`、播放保持关、采集继续。

### 2.2 接口
```cpp
// 新增运行期开关（GUI 线程调用；仅设意图，实际开/停由 pump 线程执行，结果经 poll() 反映）
void setPlaybackEnabled(bool enabled);

// start 增一个默认参数（默认 true = 现行为，CLI 不用改）
Result start(BackendKind kind, const DeviceId& capId, const DeviceId& renderId,
             uint32_t delayMs, bool playbackEnabled = true);
```
- `MonitorStatus` 不变；`renderState` 现表达真实渲染态（Idle=播放关 / Running=播放开且在跑 / Error=开启时采样率不匹配）。`renderXruns`/`renderLevel` 在渲染关时保持 0。

### 2.3 线程模型（关键：避免竞争）
- 渲染后端（`renderBackend_`/`renderRing_`）的开/关**只由两个互不并发的时机触碰**：
  1. `start()` 内，在 **pump 启动之前**（若 `playbackEnabled=true`，同步开渲染+校验，失败则走现有 `rollback`：**停采集、返回 Fail、状态 Idle/Error**——保留 CLI 的"不匹配即启动失败、全不运行"语义）。⚠️ 注意与运行期 toggle 的**不对称**：start-时失败=整体回滚；运行期 `setPlaybackEnabled(true)` 失败=仅播放关、`renderState=Error`、**采集继续**（因采集已在稳定运行，不应因一次播放尝试失败而中断）。
  2. pump 线程内（运行期 toggle）。
- `setPlaybackEnabled` 只设原子 `wantPlayback_`。pump 每轮比较 `wantPlayback_` 与本地 `renderActive_`：
  - 需开未开 → `engageRender()`（开后端+采样率校验+预填 DelayFifo；失败 → 置 `renderState=Error`/`errorCode`、`wantPlayback_=false`、保持采集-only）。
  - 需关在开 → `disengageRender()`（停+关渲染后端、释放设备、renderScope/renderLevel 归零）。
- `engageRender`/`disengageRender` 为私有 helper，DRY 复用于 start 同步开启与 pump 运行期开启。pump 启动时 `renderActive_` 依 start 是否已开渲染初始化。GUI 线程从不直接碰 `renderBackend_`。
- pump 采集-only 时：仍读采集→captureScope+capLevel；不 push/pop FIFO、不写 renderRing（DelayFifo 空闲）。engage 时重新预填 FIFO，故 toggle-on 后延迟从头建立、无脏数据。

### 2.4 测试（fake backend，无硬件）
- `PlaybackStartsDisabled`：`start(...,playbackEnabled=false)` → 采集 Running、渲染 Idle、fake 渲染后端未 open。
- `EnablePlaybackEngagesRender`：上述后 `setPlaybackEnabled(true)` + 推数据驱动 pump → renderState 变 Running、fake 渲染后端已 open、renderScope 有数据。
- `DisablePlaybackStopsRender`：运行中 `setPlaybackEnabled(false)` → renderState Idle、fake 渲染后端 stop 标志置位（设备释放）、采集仍 Running。
- `EnablePlaybackRateMismatch`：fake 采集 48000 / 渲染 44100，`setPlaybackEnabled(true)` → renderState Error、errorCode=RateMismatch、播放保持关、采集继续。
- 回归：现有 MonitorEngine 测试（4 参 `start` 默认 playbackEnabled=true）语义不变仍通过（含 `RateMismatchFails`：start-with-playback 同步校验失败 → start Fail）。

## 3. AppUi：两列布局 + 拖拽重排

### 3.1 移除（monitor-only）
- 删 `Engine engine_`、`drawSingleStream`、`refreshDevices`(单流)、`modeIdx_/prevMode_`、`deviceIdx_/devices_/devicesLoaded_`、`wavPath_`、`rateIdx_/bitsIdx_/chIdx_/isFloat_` 及 Probe。保留 `backendIdx_`（监听后端选择）。`stopAll()` 只停 `monitor_`。`main.cpp` 不变（仍 `ui.draw()` / `ui.stopAll()`）。

### 3.2 状态新增
```cpp
bool playbackEnabled_ = false;              // "同步播放" checkbox；改变时调用 monitor_.setPlaybackEnabled
std::vector<int> chartOrder_ = {0,1,2,3,4,5}; // 6 图表面板顺序，可拖拽重排
// 图表 id: 0=采集波形 1=播放波形 2=采集频谱 3=播放频谱 4=采集声谱图 5=播放声谱图
```
（分析缓冲 `capWave_/renderWave_/workCap_/magCap_/capSpec_...` 等保留不变。）

### 3.3 布局
- `draw()`：`ImGui::Begin("WinAudio")` 内左右两个并排 `BeginChild`：
  - **左列**（固定宽 ~360px，`BeginChild("left", ImVec2(360,0))`），分区（用 `SeparatorText` 分隔）：
    - 设备：采集设备 listbox + 渲染设备 listbox + "Refresh devices"
    - 控制：Backend combo、Delay 滑块、Start/Stop、**"同步播放" checkbox**（`ImGui::Checkbox`，变更即 `monitor_.setPlaybackEnabled(playbackEnabled_)`；未 Start 时禁用）
    - 状态：overall/cap/ren、sr、fifo/drift/xrun c-r、cap/ren 电平条
    - 日志：`BeginChild` 滚动区
  - `ImGui::SameLine();`
  - **右列**（`BeginChild("charts", ImVec2(0,0))` 填满）：按 `chartOrder_` 顺序渲染 6 个图表面板。
- 每个图表面板 = 一个带拖拽手柄的小节：手柄用 `Selectable`/`Button`（如 "☰ 采集波形"），`BeginDragDropSource` 携带其在 `chartOrder_` 的索引，其它面板 `BeginDragDropTarget` 接收并交换顺序（经典 ImGui list-reorder）。面板体调用对应绘图（复用现有 ImPlot 波形/频谱/声谱图代码，抽成 6 个小函数或按 id switch）。
- 播放关（renderState≠Running / snapshotRender 空）时，3 个播放面板照常绘制但显示空图（复用现有 snapshot-false 处理）。

### 3.4 交互
- Start：`monitor_.start(kind, capId, renId, delayMs, playbackEnabled_)`（把 checkbox 初始态传入）；成功后重置分析游标（`nextCapEnd_/...=0`）。
- 运行中切 checkbox → `monitor_.setPlaybackEnabled(...)`，实时生效。
- 采样率不匹配（勾选播放时）：状态区显示 `ren=Error` + errorCode 文案；采集继续。

## 4. 成功标准（可验证）
- GUI 无 mode 选择器；启动即监听 UI；采集恒可视化。
- 两列布局；右列 6 图表可拖拽重排、顺序稳定；采集/播放同类图可被拖到相邻。
- "同步播放" checkbox 实时生效：勾选→出声 + 播放侧图表有信号 + `ren=Running`；取消→停声 + 释放渲染设备（`ren=Idle`）+ 播放侧图表空；反复切换不重启、不崩溃、不死锁。
- 采样率不匹配时勾选→`ren=Error`、播放保持关、采集继续。
- MonitorEngine 新增 4 项 `setPlaybackEnabled` 单测通过；现有测试回归通过；CLI `monitor` 行为不变。
- `/W4` 零告警；`build.bat`/`test.bat` 双配置构建 + 全测试通过；GUI 存活性 OK。（波形运动 / 声音 / 拖拽等目视项在有硬件桌面时人工确认，否则记为待验证。）

## 5. 风险与对策
- **运行期开/停渲染的并发**：渲染后端只由"pump 启动前的 start"与"pump 线程"触碰，二者时序不重叠；GUI 只设原子意图 + 读状态。→ 无跨线程竞争。
- **toggle 抖动**（快速反复勾选）：pump 每轮按 `wantPlayback_` 收敛到目标态；中间态最多一轮延迟；开/停各自幂等。
- **CLI 回归**：`start` 第 5 参默认 true → 4 参调用语义不变（同步开渲染+校验+失败即 Fail）。现有 MonitorEngine 测试不受影响。
- **拖拽重排复杂度**：用 ImGui 标准 drag-drop（源/目标 + 索引交换），YAGNI 不做持久化。
- **左列固定宽**：窄窗口时用 `SetNextWindowSizeConstraints` 保证最小宽（沿用现有 460 下限，或调大以容两列）。

## 6. 交付物
修改：`src/core/MonitorEngine.h/.cpp`（setPlaybackEnabled + 运行期渲染生命周期 + start 增参）、`src/tests/test_monitorengine.cpp`（+4 测试）、`src/gui/AppUi.h/.cpp`（去单流 + 两列 + 拖拽重排 + 播放 checkbox）。
`src/cli/main.cpp`、`src/gui/main.cpp`：预期无需改（start 默认参数保 CLI；main.cpp 仍 draw/stopAll）。若编译需要，最小调整。
