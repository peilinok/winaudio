# WinAudio 数据格式功能设计

**日期**: 2026-07-04
**状态**: 待实现（brainstorming 定稿）

## 目标

为 WinAudio 增加音频**数据格式**的显示、枚举、能力查询与设置能力，贯穿 core / CLI / GUI。核心理念 **"默认就好，但允许修改"**：每个后端模式给一个开箱能跑的默认格式，并允许用户从候选下拉或手动输入覆盖；不引入本工具自己的重采样/复杂格式引擎。

## 架构

- **core 新增格式能力层**（三来源查询 + 能力矩阵 + 默认格式生成 + open 路径改动），CLI 与 GUI 共用。
- 两个功能簇（一体交付，一个 spec）：
  - **簇 A 格式设置**（交互）：GUI 左栏格式选择区 + CLI `--format` 扩展。
  - **簇 B 格式诊断**（只读）：CLI `caps` 命令 + GUI 能力浏览弹窗。

## 全局约束

- **无重采样**：本工具不做采样率转换。Shared 模式下把用户格式传给 `IAudioClient::Initialize`，格式转换由 **WASAPI 音频引擎**完成（非本工具代码）；Exclusive 模式无转换，格式必须硬件原生支持。
- **monitor 采集/渲染采样率必须一致**（现有约束，改格式时仍须守）。
- **全默认路径行为不变**：用户不选/不改格式时，Shared 走 `GetMixFormat`、Exclusive 走探测默认，与现状字节级一致。
- 静态 CRT（`/MT`）、`/W4` 零告警双配置、C++17、仅 Win32 + STL（core 零外部依赖）。

## 背景：Windows 三层格式（设计依据）

| 层 | 格式 | 谁用 | 来源 / API | 典型 |
|---|---|---|---|---|
| A | 硬件原生 | Exclusive（绕过引擎直连驱动） | 驱动声明；`IsFormatSupported(EXCLUSIVE)` 逐个探测 | 16/24-bit **int** |
| B | 引擎混音格式 (mix) | Shared（引擎混音所有流） | `GetMixFormat()` | 常为 **32-bit float** |
| C | 控制面板"默认格式" | 用户设的共享默认 | `PKEY_AudioEngine_DeviceFormat`（OEM 默认 `PKEY_AudioEngine_OemFormat`） | int PCM |

关键：同一设备 `DeviceFormat`（C，int）与 `GetMixFormat`（B，float）**不同** —— 这是"系统推荐格式不在控制面板列表里"的根因。WASAPI **无**"列出全部支持格式"的 API，只能遍历候选逐个 `IsFormatSupported`。

## core 能力层

### 三来源查询（`DeviceEnumerator` 扩展）

```cpp
Result deviceFormat(const DeviceId& id, AudioFormat& out);  // PKEY_AudioEngine_DeviceFormat
Result oemFormat(const DeviceId& id, AudioFormat& out);     // PKEY_AudioEngine_OemFormat
// 已有：mixFormat(id, out) → GetMixFormat
```
读法与现有取设备名（`PKEY_Device_FriendlyName`）相同：`OpenPropertyStore(STGM_READ)` → `GetValue(key)` → `VT_BLOB` 里的 `WAVEFORMATEX` → `fromWaveFormat`。某来源缺失/读取失败 → 返回失败，调用方标记 invalid（不视作硬错误）。

### 能力矩阵 + 候选空间

```cpp
struct FormatSupport {
    AudioFormat fmt;
    bool sharedOk;      // IsFormatSupported(SHARED)    引擎可接受/转换
    bool exclusiveOk;   // IsFormatSupported(EXCLUSIVE) 硬件原生支持
};
struct DeviceCapabilities {
    AudioFormat mixFormat, deviceFormat, oemFormat;   // 三来源
    bool hasMix = false, hasDevice = false, hasOem = false;
    std::vector<FormatSupport> matrix;                // 候选逐个双探测
};
Result queryCapabilities(DataFlow flow, const DeviceId& id, DeviceCapabilities& out);
```

**候选空间**（`FormatSpec` 提供，矩阵与下拉共用）：
- 采样率：`44100, 48000, 88200, 96000, 176400, 192000`
- 位深/类型：`16-int, 24-int, 32-int, 32-float`（`isFloat` 标记）
- 声道：`1, 2`
- = 48 组合 × 2 探测 = 96 次 `IsFormatSupported`（微秒级，一次查完缓存于 `DeviceCapabilities`）。

探测复用现有 `selectSupportedFormat` 的谓词形态（`IsFormatSupported(SHARED/EXCLUSIVE)`）。

### 默认格式生成

```cpp
Result defaultFormatFor(BackendKind kind, DataFlow flow, const DeviceId& id, AudioFormat& out);
// Shared    → mixFormat(id)
// Exclusive → selectSupportedFormat(候选: {deviceFormat 优先} + defaultExclusiveCaptureCandidates(),
//                                    谓词: IsFormatSupported(EXCLUSIVE))
```
Exclusive 默认在**切到 Exclusive 时后台生成**（保证开箱能跑），非用户可见的 probe 按钮。

### open 路径改动（Shared 用指定格式）

`WasapiStream::prepareClient` 的 Shared 分支当前忽略传入格式、固定 `GetMixFormat`。改为：**`hasRequested_` 为真 → 用 `requestedFormat_` 调 `Initialize`（引擎转换）；否则回落 `GetMixFormat`**。Exclusive 分支不动（已用指定格式 + 对齐重试）。`bufferMs` 等既有 `StreamParams` 逻辑不受影响。

## 簇 A：格式设置

### GUI（左栏 Control 区格式选择区）

紧靠 `Backend` 下拉下方，新增：
- **当前格式行**：`48000 Hz · 16-bit int · 2ch` 文本；
- **随 Backend 联动**：切 Shared/Exclusive 时调 `defaultFormatFor` 重算并填入当前格式；
- **下拉候选**：列当前模式下"支持"的候选（Shared 取 `sharedOk`、Exclusive 取 `exclusiveOk`，读 `queryCapabilities` 缓存）+ 末项 `自定义…`；
- **自定义输入**：选 `自定义…` 显示输入框，接受 `R/B/C[f]` 字符串（走 `parseFormatSpec`）；
- **无实时探测**：Start 时以当前格式 `open`；成功即跑；失败 → 停止 + 日志打印 `Result.message`（含规范化 HRESULT，如 `AUDCLNT_E_UNSUPPORTED_FORMAT`）。

`MonitorEngine::start` 增采集格式参数（可选，默认空=用 `defaultFormatFor`）；渲染沿用采集采样率（守一致约束）。

### CLI（`--format` 扩展）

- `capture --format R/B/C[f]` 现状仅 Exclusive 有效（shared 传报错）；改为 **Shared 也接受**（传给 `Initialize` 引擎转换），语义与 GUI 一致。
- `monitor` 增可选 `--format`（采集格式；渲染须同采样率）。
- `play` 从 WAV 头读格式，不变。

## 簇 B：格式诊断

### CLI `caps` 命令

```
WinAudioCli caps [--device <id>] [--render|--capture]
```
调 `queryCapabilities`，打印：三来源格式（Mix/Device/OEM，标注 Mix 多为引擎 float）+ 能力矩阵（文本表，每候选一行，Shared/Exclusive 两列 ✓/✗）。

### GUI 能力浏览弹窗

左栏加 `Device caps…` 按钮 → 打开**独立弹窗**（矩阵 48 行，弹窗比左栏合适）：
- 顶部：三来源格式并列（缺失项显示 `—`）；
- 主体：**一维列表**矩阵（每候选一行，`Format | Shared ✓/✗ | Exclusive ✓/✗`，滚动）；
- 打开时调 `queryCapabilities` 查一次并缓存；换设备/刷新时重查。

布局参考：
```
Capture: Microphone
Mix 48000/32f/1   Device 48000/16/1   OEM 48000/16/1
Format          Shared   Exclusive
44100/16/1        ✓         ✓
48000/24/1        ✓         ✗
...  (48 rows, scroll)
```

## 错误处理

- 三来源某项缺失/读取失败 → `has*=false`，UI 显示 `—`，非硬错误。
- 矩阵格子探测失败 → 该格 `false`（不支持）。
- Start 用指定格式失败 → `Result` 携规范化 HRESULT message → GUI 日志 + 停止 / CLI 打印并非零退出。
- 手输解析失败（`parseFormatSpec` 返回 false）→ 拒绝启动，提示"无效格式"。

## 测试

- **core（无硬件依赖）**：
  - `queryCapabilities`：注入 fake 探测谓词，验证矩阵按 Shared/Exclusive 结果正确填充、候选空间完整（48 项）。
  - `defaultFormatFor`：Shared 返回 mixFormat；Exclusive 在 fake 谓词下返回首个支持候选（首选 deviceFormat）。
  - 三来源解析：`WAVEFORMATEX` blob → `AudioFormat`（含 float/int 判定）经 `fromWaveFormat`，复用现有测试模式。
  - `WasapiStream` Shared 选格式辅助（hasRequested → requestedFormat 否则 mix）若可抽为纯函数则单测。
- **既有**：`FormatSpec`（parse/select/candidates）测试保留。
- **真实 `IsFormatSupported` / 三来源 property store**：无硬件不单测，靠手动 smoke（CLI `caps` + GUI 弹窗核对控制面板）。

## 已知约束 / 非目标

- 不做本工具自有重采样；Shared 转换依赖 WASAPI 引擎。
- monitor 采集/渲染采样率须一致。
- 分析仍单声道降混（现状）。
- 候选空间固定 48 组合；更大范围/更多声道留作后续。
- 能力矩阵一次性探测（不做增量/异步 UI，96 次探测足够快）。
