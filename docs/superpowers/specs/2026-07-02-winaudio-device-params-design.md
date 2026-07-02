# WinAudio 设备下拉框 + 高级流参数(StreamParams)设计

日期:2026-07-02 · 状态:已与用户对齐(方案 C:改 `open` 签名) · 范围:Core + GUI(CLI 零行为变化)

## 1. 背景与目标

用户需求原话要点:
1. **设备选择改为下拉框**,启动时默认选中**系统默认设备**;
2. 除基础参数外,WASAPI 还有 **categories、policy、APO、bIsOffload、Options** 等流参数,需要有配置入口,但**主界面不能拥挤**——主界面只留基础参数,高级参数放独立弹窗;
3. **一切参数以系统推荐/系统默认为第一优先级**:不显式设置就**不注入任何调用**(零行为变化)。

对应 Windows API 事实:
- category / bIsOffload / Options(RAW=绕过 APO、MATCH_FORMAT)→ `IAudioClient2::SetClientProperties(AudioClientProperties)`,必须在 `IAudioClient::Initialize` **之前**调用;
- policy(通信时自动压低音量)→ 会话级 `IAudioSessionControl2::SetDuckingPreference(TRUE)`(opt-out),在 Initialize **之后**设置;
- 缓冲时长 → `Initialize` 的 duration 参数(现状:Shared 固定 100 ms,Exclusive 用 minPeriod + 对齐重试)。

## 2. StreamParams(新核心类型,windows-free)

`src/core/StreamParams.h`(纯 STL,不含 windows.h;映射到 Windows 枚举在 WasapiStream.cpp 内做):

```cpp
#pragma once
#include <cstdint>
namespace wa {

enum class AudioCategory : uint8_t { Default, Other, Communications, Media, Movie,
                                     GameChat, Speech, SoundEffects, GameMedia };
enum class StreamOption  : uint8_t { Default, Raw, MatchFormat };  // AUDCLNT_STREAMOPTIONS
enum class OffloadMode   : uint8_t { Default, Force };             // 仅 render 有意义
enum class DuckingMode   : uint8_t { Default, OptOut };            // 仅 render 有意义

struct StreamParams {
    AudioCategory category = AudioCategory::Default;  // Default = 不调 SetClientProperties
    StreamOption  option   = StreamOption::Default;
    OffloadMode   offload  = OffloadMode::Default;
    DuckingMode   ducking  = DuckingMode::Default;
    uint32_t      bufferMs = 0;                       // 0 = 现状(Shared 100ms / Excl minPer)

    bool anyClientProps() const {   // category/option/offload 任一非默认 → 需要 SetClientProperties
        return category != AudioCategory::Default || option != StreamOption::Default
            || offload  != OffloadMode::Default;
    }
    bool isDefault() const {
        return !anyClientProps() && ducking == DuckingMode::Default && bufferMs == 0;
    }
};

} // namespace wa
```

**语义铁律:`StreamParams{}`(全默认)= 一个额外 Windows 调用都不加**,打开路径与当前 master 逐字节一致。

## 3. `IAudioBackend::open` 签名变更(方案 C)

```cpp
virtual Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring,
                    const StreamParams& params) = 0;
```

- **不用默认实参**(虚函数默认实参绑定在静态类型上,是陷阱);所有调用点显式传参。
- 全部调用点(6 处):
  | 位置 | 传什么 |
  |---|---|
  | `IAudioBackend.h` 接口 | 签名变更 + `#include "StreamParams.h"` |
  | `WasapiStream.h/.cpp` override | 存 `params_`,open 路径消费(§4) |
  | `test_monitorengine.cpp` fake | 记录收到的 `params`(供断言) |
  | `Engine.cpp` capture(~L92) | `StreamParams{}`(CLI 不暴露,零变化) |
  | `Engine.cpp` playback(~L130) | `StreamParams{}` |
  | `MonitorEngine.cpp` capture(~L97) | `capParams_` |
  | `MonitorEngine.cpp` engageRender(~L157) | 渲染参数快照(§5) |

## 4. WasapiStream 注入实现

打开顺序(`open` → Activate `IAudioClient` → **[新] SetClientProperties** → `prepareClient`(Initialize)→ **[新] ducking**):

1. **模式约束(先校验)**:category/option/offload/ducking 仅 **Shared** 模式有效(Exclusive 本就绕过 APO/会话/类别处理);`mode==Exclusive && (anyClientProps() || ducking!=Default)` → `Result::Fail`("advanced stream params (category/option/offload/ducking) require WASAPI-Shared; only bufferMs applies to exclusive")。`bufferMs` 两模式都生效。
2. **SetClientProperties**(仅当 `anyClientProps()`):
   - `client_.As(&client2)` QI `IAudioClient2`;失败 → Fail("IAudioClient2 unavailable; cannot apply advanced stream params")——**显式设置绝不静默降级**。
   - `offload==Force`:先 `client2->IsOffloadCapable(mapCategory(category), &ok)`,`!ok` → Fail("device/category does not support hardware offload")。
   - `AudioClientProperties p{sizeof(p), offload==Force, mapCategory(category), mapOption(option)}` → `SetClientProperties(&p)`,失败 → HrToResult。
   - 映射表(`WasapiStream.cpp` 内 free function,供单测):
     - `Other→AudioCategory_Other`,`Communications→AudioCategory_Communications`,`Media→AudioCategory_Media`,`Movie→AudioCategory_Movie`,`GameChat→AudioCategory_GameChat`,`Speech→AudioCategory_Speech`,`SoundEffects→AudioCategory_SoundEffects`,`GameMedia→AudioCategory_GameMedia`;`Default→AudioCategory_Other`(仅在 offload/option 非默认而 category 默认时作为占位——SetClientProperties 必须填 category)。
     - `Raw→AUDCLNT_STREAMOPTIONS_RAW`,`MatchFormat→AUDCLNT_STREAMOPTIONS_MATCH_FORMAT`,`Default→AUDCLNT_STREAMOPTIONS_NONE`。
   - 已知说明:当前 Shared 路径恒用 mix format,`MATCH_FORMAT` 单独设置通常无实际效果(留作后续 shared 指定格式的伏笔);RAW 对带 APO 的设备效果显著(旁路增强/降噪)。
3. **bufferMs**:Shared:`dur = bufferMs ? bufferMs*10'000 : 100ms 现值`;Exclusive:`dur = bufferMs ? bufferMs*10'000 : minPer`,仍走 `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` 对齐重试(现逻辑不变)。
4. **ducking==OptOut**(Initialize 之后):`client_->GetService(IAudioSessionControl)` → QI `IAudioSessionControl2` → `SetDuckingPreference(TRUE)`;任一步失败 → Fail(显式设置不静默)。

## 5. MonitorEngine

```cpp
Result start(BackendKind kind, const DeviceId& capId, const DeviceId& renderId,
             uint32_t delayMs, bool playbackEnabled = true,
             const StreamParams& capParams = {}, const StreamParams& renderParams = {});
void   setRenderParams(const StreamParams& p);   // 运行中可调,下次 engage 生效
```

- `capParams_` 仅 start 时消费(采集参数改动需 Stop/Start,GUI 提示);
- `renderParams_` 由 `paramsMutex_` 保护;`setRenderParams` 写,`engageRender` 开头取快照传给 `renderBackend_->open(...)` ——兑现"渲染参数取消再勾选'同步播放'即生效,不重启采集"。pump 线程只在 engage 时读一次快照,不在实时路径上加锁。
- start 内部初次 engage 同样走快照路径(行为一致)。

## 6. GUI(AppUi)

### 6.1 设备下拉框
- 两个 `BeginListBox` → `ImGui::BeginCombo/EndCombo`(preview = 当前选中设备名);默认设备带 `* ` 前缀;保留 "Refresh devices" 按钮。
- 刷新/启动选择规则:**已选设备(按 DeviceId 匹配)仍存在则保持;否则选 `isDefault` 项;再否则 index 0**。首次启动即落在系统默认设备(替代现在的固定 index 0)。

### 6.2 Advanced 弹窗
- Devices 区加 `Advanced...` 按钮 → `OpenPopup` + `BeginPopupModal("Audio parameters (advanced)")`。
- 两列布局:**Capture | Render**(各自独立 `StreamParams`):
  - 两列均有:`Category` 下拉(首项 **"System default"** + 8 类)、`Stream option` 下拉(System default / Raw (bypass APO) / Match format)、`Buffer (ms)` InputInt(0 = auto,钳 [0,2000]);
  - Render 列额外:`Hardware offload` checkbox、`Ducking opt-out` checkbox;
  - 每列 `Reset to system defaults` 按钮;
  - 弹窗顶部固定说明文案:"All values default to system-recommended. Category/option/offload/ducking require WASAPI-Shared; only Buffer applies to Exclusive."
- **生效逻辑**:GUI 持有 `capParams_`/`renParams_` 成员;Start 传入;弹窗内渲染参数任一控件变更且 `monitorStarted_` → 立即 `monitor_.setRenderParams(renParams_)` + 日志一行("render params updated; re-toggle playback to apply");采集参数变更且运行中 → 弹窗关闭时日志一行("capture params take effect on next Start")。

## 7. 测试

- `test_streamparams.cpp`:`isDefault`/`anyClientProps` 谓词;类别/option 映射函数数值断言(测试工程本就在 Windows 上,直接比对 `AudioCategory_*`/`AUDCLNT_STREAMOPTIONS_*`)。
- `test_monitorengine.cpp`:fake 后端记录 `open` 收到的 `StreamParams` → 断言 start 传递 capture/render 参数正确;`setRenderParams` 后 disable→enable playback,fake render 在 re-engage 收到**新**参数。
- 既有 60 测试零回归(fake 签名同步更新);CLI 路径显式 `StreamParams{}`,行为零变化。
- RAW/offload/ducking 的真实音频效果依赖硬件与驱动 → 手动冒烟(不做自动化断言)。

## 8. 非目标(YAGNI)

- 参数持久化(重启回"跟随系统")。
- CLI 暴露高级参数。
- loopback、其余 `AUDCLNT_STREAMFLAGS_*`、会话音量/静音、通知回调。
- 运行中热改**采集**参数(必须 Stop/Start)。
- 弹窗内格式(采样率/位深)配置——Exclusive 格式仍由现有 CLI `--format`/候选协商逻辑负责。

## 9. 验收标准

1. 全默认路径:CLI 与 GUI 行为、日志、时序与 master 无差异;60+ 新增测试全绿,/W4 零告警。
2. GUI 启动即选中系统默认采集/渲染设备(下拉框,默认项带 `*`)。
3. Advanced 弹窗按 §6.2 布局;全部初值 "System default"。
4. Shared + 任一高级参数:`SetClientProperties` 在 Initialize 前被调用(代码审查 + 日志/手动验证);Exclusive + 高级参数 → 启动失败且报错文案明确。
5. fake 断言参数端到端传递 + re-engage 拿到新渲染参数。
6. 手动:运行中改渲染参数 → 重勾"同步播放"生效;改采集参数 → Stop/Start 生效。
