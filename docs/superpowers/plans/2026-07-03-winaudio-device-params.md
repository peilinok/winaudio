# WinAudio 设备下拉框 + StreamParams 高级流参数 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 设备选择改为默认选中系统默认设备的下拉框;新增 `StreamParams`(category/option/offload/ducking/bufferMs)经 `IAudioBackend::open` 显式传入 WASAPI 流,GUI 提供 Advanced 弹窗,全默认 = 零行为变化。

**Architecture:** 方案 C——`open` 签名增加 `const StreamParams&`(无默认实参,6 处调用点显式传参);`WasapiStream` 在 `threadMain` 的 Activate 之后/Initialize 之前注入 `IAudioClient2::SetClientProperties`,Initialize 之后设置 ducking;`MonitorEngine::start` 增两个带默认值的参数 + `setRenderParams`(互斥锁,engage 快照)。Spec:`docs/superpowers/specs/2026-07-02-winaudio-device-params-design.md`。

**Tech Stack:** C++17 / CMake(VS2022 生成器)/ WASAPI(IAudioClient2、IAudioSessionControl2)/ Dear ImGui / gtest。

## Global Constraints

- **零行为变化铁律**:`StreamParams{}`(全默认)= 一个额外 Windows 调用都不加,打开路径与当前 master 逐字节一致;CLI 永远传 `StreamParams{}`。
- 虚函数 `open` **不用默认实参**(默认实参绑定静态类型是陷阱);所有调用点显式传参。
- 高级参数(除 `bufferMs`)仅 WASAPI-**Shared** 有效;Exclusive + 任一非默认(category/option/offload/ducking)→ `open` 同步报错。**显式设置绝不静默降级**(QI 失败、IsOffloadCapable 不过、SetDuckingPreference 失败都返回 Fail)。
- 构建:`.\build.bat Debug` / `.\build.bat Release`,/W4 **零告警**;测试 `.\build\bin\Debug\WinAudioTests.exe`(或 `.\test.bat Debug`)。`cmake` 不在 PATH 时用 `D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`(build.bat 已处理)。
- 提交信息英文,回复中文;代码注释英文,与现有风格一致。

## File Structure

| 文件 | 动作 | 职责 |
|---|---|---|
| `src/core/StreamParams.h` | 新建 | windows-free 参数类型 + 谓词(唯一的参数定义处) |
| `src/core/IAudioBackend.h` | 修改 | `open` 签名 + include |
| `src/core/WasapiStream.h/.cpp` | 修改 | 签名、`params_` 存储、注入(SetClientProperties/bufferMs/ducking)、映射 free function |
| `src/core/Engine.cpp` | 修改 | 2 处调用点传 `StreamParams{}` |
| `src/core/MonitorEngine.h/.cpp` | 修改 | start 增参、`setRenderParams`、engage 快照 |
| `src/tests/test_streamparams.cpp` | 新建 | 谓词 + 映射数值 + Exclusive 拒绝测试 |
| `src/tests/test_monitorengine.cpp` | 修改 | fake 记录参数;端到端传递 + re-engage 测试 |
| `src/tests/CMakeLists.txt` | 修改 | 加 `test_streamparams.cpp` |
| `src/gui/AppUi.h/.cpp` | 修改 | 设备 Combo + 默认设备规则 + Advanced 弹窗 |
| `CLAUDE.md` | 修改 | GUI 段落补一句 |

---

### Task 1: StreamParams 类型 + open 签名变更(全默认零行为变化)

**Files:**
- Create: `src/core/StreamParams.h`
- Create: `src/tests/test_streamparams.cpp`
- Modify: `src/core/IAudioBackend.h:32`(open 签名)
- Modify: `src/core/WasapiStream.h:22`、`src/core/WasapiStream.cpp:16-20`(签名 + 存 params_)
- Modify: `src/core/Engine.cpp:92`、`src/core/Engine.cpp:130`(传 `StreamParams{}`)
- Modify: `src/core/MonitorEngine.cpp:97`、`src/core/MonitorEngine.cpp:157`(传 `StreamParams{}`,T3 换真值)
- Modify: `src/tests/test_monitorengine.cpp:37-40`(fake 签名 + 记录)
- Modify: `src/tests/CMakeLists.txt:2`(加测试文件)

**Interfaces:**
- Produces: `wa::StreamParams`(字段 `category/option/offload/ducking/bufferMs`,谓词 `anyClientProps()/isDefault()`);`IAudioBackend::open(const DeviceId&, const AudioFormat&, RingBuffer*, const StreamParams&)`;fake 的 `StreamParams lastOpenParams_;`(T3 断言用)。

- [ ] **Step 1: 写失败测试** — 新建 `src/tests/test_streamparams.cpp`:

```cpp
#include <gtest/gtest.h>
#include "StreamParams.h"
using namespace wa;

TEST(StreamParams, DefaultIsAllDefault) {
    StreamParams p;
    EXPECT_TRUE(p.isDefault());
    EXPECT_FALSE(p.anyClientProps());
    EXPECT_EQ(p.bufferMs, 0u);
}

TEST(StreamParams, Predicates) {
    StreamParams a; a.category = AudioCategory::Communications;
    EXPECT_TRUE(a.anyClientProps()); EXPECT_FALSE(a.isDefault());
    StreamParams b; b.option = StreamOption::Raw;
    EXPECT_TRUE(b.anyClientProps()); EXPECT_FALSE(b.isDefault());
    StreamParams c; c.offload = OffloadMode::Force;
    EXPECT_TRUE(c.anyClientProps()); EXPECT_FALSE(c.isDefault());
    StreamParams d; d.ducking = DuckingMode::OptOut;      // ducking 不属于 client-props
    EXPECT_FALSE(d.anyClientProps()); EXPECT_FALSE(d.isDefault());
    StreamParams e; e.bufferMs = 50;                       // bufferMs 也不属于
    EXPECT_FALSE(e.anyClientProps()); EXPECT_FALSE(e.isDefault());
}
```

并在 `src/tests/CMakeLists.txt` 的 `add_executable(WinAudioTests` 文件列表加一行 `    test_streamparams.cpp`(放 `test_delayfifo.cpp` 之后即可)。

- [ ] **Step 2: 构建确认失败**

Run: `.\build.bat Debug`
Expected: 编译错误 `Cannot open include file: 'StreamParams.h'`(测试先行,头文件还不存在)。

- [ ] **Step 3: 新建 `src/core/StreamParams.h`**(spec §2 原文):

```cpp
#pragma once
#include <cstdint>
namespace wa {

// Advanced WASAPI stream parameters. ALL defaults mean "follow system": when a field is
// Default/0 the corresponding Windows call is NOT made at all, so StreamParams{} keeps the
// open path byte-for-byte identical to the pre-StreamParams behavior.
enum class AudioCategory : uint8_t { Default, Other, Communications, Media, Movie,
                                     GameChat, Speech, SoundEffects, GameMedia };
enum class StreamOption  : uint8_t { Default, Raw, MatchFormat };  // AUDCLNT_STREAMOPTIONS
enum class OffloadMode   : uint8_t { Default, Force };             // render-only
enum class DuckingMode   : uint8_t { Default, OptOut };            // render-only

struct StreamParams {
    AudioCategory category = AudioCategory::Default;  // Default = no SetClientProperties
    StreamOption  option   = StreamOption::Default;
    OffloadMode   offload  = OffloadMode::Default;
    DuckingMode   ducking  = DuckingMode::Default;
    uint32_t      bufferMs = 0;                       // 0 = current behavior (Shared 100 ms / Excl minPer)

    bool anyClientProps() const {   // category/option/offload need IAudioClient2::SetClientProperties
        return category != AudioCategory::Default || option != StreamOption::Default
            || offload  != OffloadMode::Default;
    }
    bool isDefault() const {
        return !anyClientProps() && ducking == DuckingMode::Default && bufferMs == 0;
    }
};

} // namespace wa
```

- [ ] **Step 4: 改 `IAudioBackend.h`** — 顶部 include 区加 `#include "StreamParams.h"`,签名改为:

```cpp
    virtual Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring,
                        const StreamParams& params) = 0;
```

- [ ] **Step 5: 同步 6 处**(此步后全仓可编译):

`src/core/WasapiStream.h:22`:
```cpp
    Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring,
                const StreamParams& params) override;
```
并在 private 成员区(`DeviceId deviceId_;` 之后)加:`    StreamParams params_{};`

`src/core/WasapiStream.cpp:16`:
```cpp
Result WasapiStream::open(const DeviceId& id, const AudioFormat& /*fmt*/, RingBuffer* ring,
                          const StreamParams& params) {
    deviceId_ = id;
    ring_ = ring;
    params_ = params;
    return Result::Ok(); // real activation happens on the worker thread (its own COM apt)
}
```

`src/core/Engine.cpp:92` 与 `:130`(两处相同改法):
```cpp
        Result r = backend_->open(id, AudioFormat{}, ring_.get(), StreamParams{});
```

`src/core/MonitorEngine.cpp:97`:
```cpp
    if (Result r = capBackend_->open(capId, AudioFormat{}, captureRing_.get(), StreamParams{}); !r)
```
`src/core/MonitorEngine.cpp:157`:
```cpp
    if (Result r = renderBackend_->open(renderId_, AudioFormat{}, renderRing_.get(), StreamParams{}); !r) {
```

`src/tests/test_monitorengine.cpp:37`(fake,同时记录参数供 T3 断言):
```cpp
    Result open(const DeviceId&, const AudioFormat&, RingBuffer* ring,
                const StreamParams& params) override {
        ring_ = ring;
        lastOpenParams_ = params;
        return Result::Ok();
    }
```
并在 FakeBackend 成员区(`RingBuffer* ring_ = nullptr;` 附近)加:`    StreamParams lastOpenParams_{};`

- [ ] **Step 6: 构建 + 全量测试通过**

Run: `.\build.bat Debug` 然后 `.\build\bin\Debug\WinAudioTests.exe`
Expected: 构建 0 错 0 警,`62 tests ... PASSED`(60 旧 + 2 新)。

- [ ] **Step 7: 提交**

```bash
git add src/core/StreamParams.h src/core/IAudioBackend.h src/core/WasapiStream.h src/core/WasapiStream.cpp src/core/Engine.cpp src/core/MonitorEngine.cpp src/tests/test_streamparams.cpp src/tests/test_monitorengine.cpp src/tests/CMakeLists.txt
git commit -m "feat(core): StreamParams type + explicit open(params) signature (all-default = zero behavior change)"
```

---

### Task 2: WasapiStream 参数注入

**Files:**
- Modify: `src/core/WasapiStream.h`(映射函数声明 + 2 个私有方法)
- Modify: `src/core/WasapiStream.cpp`(open 模式约束、threadMain 注入、prepareClient bufferMs、映射实现)
- Test: `src/tests/test_streamparams.cpp`(追加映射 + Exclusive 拒绝测试)

**Interfaces:**
- Consumes: T1 的 `StreamParams`/`params_`。
- Produces: `wa::mapCategory(AudioCategory) -> AUDIO_STREAM_CATEGORY`、`wa::mapStreamOption(StreamOption) -> AUDCLNT_STREAMOPTIONS`(free function,声明于 WasapiStream.h);Exclusive+高级参数 → `open` 同步 Fail 的行为。

- [ ] **Step 1: 写失败测试** — `test_streamparams.cpp` 追加(文件顶部加 `#include "WasapiStream.h"`;该头已含 windows/audioclient 头):

```cpp
TEST(StreamParams, CategoryMapping) {
    EXPECT_EQ(mapCategory(AudioCategory::Other),          AudioCategory_Other);
    EXPECT_EQ(mapCategory(AudioCategory::Communications), AudioCategory_Communications);
    EXPECT_EQ(mapCategory(AudioCategory::Media),          AudioCategory_Media);
    EXPECT_EQ(mapCategory(AudioCategory::Movie),          AudioCategory_Movie);
    EXPECT_EQ(mapCategory(AudioCategory::GameChat),       AudioCategory_GameChat);
    EXPECT_EQ(mapCategory(AudioCategory::Speech),         AudioCategory_Speech);
    EXPECT_EQ(mapCategory(AudioCategory::SoundEffects),   AudioCategory_SoundEffects);
    EXPECT_EQ(mapCategory(AudioCategory::GameMedia),      AudioCategory_GameMedia);
    EXPECT_EQ(mapCategory(AudioCategory::Default),        AudioCategory_Other); // placeholder when props set w/o category
}

TEST(StreamParams, OptionMapping) {
    EXPECT_EQ(mapStreamOption(StreamOption::Default),     AUDCLNT_STREAMOPTIONS_NONE);
    EXPECT_EQ(mapStreamOption(StreamOption::Raw),         AUDCLNT_STREAMOPTIONS_RAW);
    EXPECT_EQ(mapStreamOption(StreamOption::MatchFormat), AUDCLNT_STREAMOPTIONS_MATCH_FORMAT);
}

TEST(StreamParams, ExclusiveRejectsAdvancedParams) {
    // open() validates synchronously (no hardware touched: activation is deferred to start()).
    WasapiCaptureStream s(WasapiMode::Exclusive, nullptr);
    StreamParams raw; raw.option = StreamOption::Raw;
    EXPECT_FALSE(s.open(L"", AudioFormat{}, nullptr, raw));
    StreamParams duck; duck.ducking = DuckingMode::OptOut;
    EXPECT_FALSE(s.open(L"", AudioFormat{}, nullptr, duck));
    StreamParams onlyBuf; onlyBuf.bufferMs = 50;           // bufferMs alone IS allowed in exclusive
    EXPECT_TRUE(s.open(L"", AudioFormat{}, nullptr, onlyBuf));
    WasapiCaptureStream sh(WasapiMode::Shared, nullptr);   // shared accepts advanced params
    EXPECT_TRUE(sh.open(L"", AudioFormat{}, nullptr, raw));
}
```

- [ ] **Step 2: 构建确认失败**

Run: `.\build.bat Debug`
Expected: 链接/编译错误(`mapCategory` 未声明)。

- [ ] **Step 3: 实现** —

`WasapiStream.h`:namespace 内(class 之前)加声明:
```cpp
// StreamParams -> WASAPI enum mapping (free functions, exposed for unit tests).
AUDIO_STREAM_CATEGORY mapCategory(AudioCategory c);
AUDCLNT_STREAMOPTIONS mapStreamOption(StreamOption o);
```
`WasapiStream` private 区(`Result prepareClient(...)` 旁)加:
```cpp
    Result applyClientProperties();  // Activate 之后、Initialize 之前;全默认时零调用
    Result applyDucking();           // Initialize 之后;OptOut 时设置会话 ducking 偏好
```

`WasapiStream.cpp`:顶部 `#include <audiopolicy.h>`(IAudioSessionControl2)。映射实现(namespace 内,类实现之前):
```cpp
AUDIO_STREAM_CATEGORY mapCategory(AudioCategory c) {
    switch (c) {
    case AudioCategory::Communications: return AudioCategory_Communications;
    case AudioCategory::Media:          return AudioCategory_Media;
    case AudioCategory::Movie:          return AudioCategory_Movie;
    case AudioCategory::GameChat:       return AudioCategory_GameChat;
    case AudioCategory::Speech:         return AudioCategory_Speech;
    case AudioCategory::SoundEffects:   return AudioCategory_SoundEffects;
    case AudioCategory::GameMedia:      return AudioCategory_GameMedia;
    case AudioCategory::Other:
    case AudioCategory::Default:
    default:                            return AudioCategory_Other;
    }
}

AUDCLNT_STREAMOPTIONS mapStreamOption(StreamOption o) {
    switch (o) {
    case StreamOption::Raw:         return AUDCLNT_STREAMOPTIONS_RAW;
    case StreamOption::MatchFormat: return AUDCLNT_STREAMOPTIONS_MATCH_FORMAT;
    case StreamOption::Default:
    default:                        return AUDCLNT_STREAMOPTIONS_NONE;
    }
}
```

`open()` 开头加模式约束(在赋值之前):
```cpp
    if (mode_ == WasapiMode::Exclusive &&
        (params.anyClientProps() || params.ducking != DuckingMode::Default)) {
        return Result::Fail(-1,
            "WasapiStream: advanced stream params (category/option/offload/ducking) require "
            "WASAPI-Shared; only bufferMs applies to exclusive mode");
    }
```

两个私有方法:
```cpp
Result WasapiStream::applyClientProperties() {
    if (!params_.anyClientProps()) return Result::Ok();   // follow system: no calls at all
    ComPtr<IAudioClient2> client2;
    HRESULT hr = client_->QueryInterface(__uuidof(IAudioClient2),
                     reinterpret_cast<void**>(client2.GetAddressOf()));
    if (FAILED(hr) || !client2.Get())
        return HrToResult(FAILED(hr) ? hr : E_NOINTERFACE,
                          "WasapiStream: IAudioClient2 unavailable; cannot apply advanced stream params");
    const AUDIO_STREAM_CATEGORY cat = mapCategory(params_.category);
    if (params_.offload == OffloadMode::Force) {
        BOOL capable = FALSE;
        hr = client2->IsOffloadCapable(cat, &capable);
        if (FAILED(hr)) return HrToResult(hr, "WasapiStream: IsOffloadCapable");
        if (!capable)
            return Result::Fail(-1, "WasapiStream: device/category does not support hardware offload");
    }
    AudioClientProperties p{};
    p.cbSize     = sizeof(p);
    p.bIsOffload = (params_.offload == OffloadMode::Force) ? TRUE : FALSE;
    p.eCategory  = cat;
    p.Options    = mapStreamOption(params_.option);
    hr = client2->SetClientProperties(&p);
    if (FAILED(hr)) return HrToResult(hr, "WasapiStream: SetClientProperties");
    return Result::Ok();
}

Result WasapiStream::applyDucking() {
    if (params_.ducking != DuckingMode::OptOut) return Result::Ok();
    ComPtr<IAudioSessionControl> sc;
    HRESULT hr = client_->GetService(__uuidof(IAudioSessionControl),
                     reinterpret_cast<void**>(sc.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "WasapiStream: GetService(IAudioSessionControl)");
    ComPtr<IAudioSessionControl2> sc2;
    hr = sc->QueryInterface(__uuidof(IAudioSessionControl2),
                     reinterpret_cast<void**>(sc2.GetAddressOf()));
    if (FAILED(hr) || !sc2.Get())
        return HrToResult(FAILED(hr) ? hr : E_NOINTERFACE, "WasapiStream: IAudioSessionControl2 unavailable");
    hr = sc2->SetDuckingPreference(TRUE);
    if (FAILED(hr)) return HrToResult(hr, "WasapiStream: SetDuckingPreference");
    return Result::Ok();
}
```
(注:若 `ComUtil.h` 的 ComPtr 无 `.Get()`,按其实际 API 判空——先读该头。)

`threadMain()`:Activate 成功后、prepareClient 之前插:
```cpp
    if (Result ap = applyClientProperties(); !ap) { signalReady(ap); return; }
```
prepareClient 成功后紧跟着插:
```cpp
    if (Result dk = applyDucking(); !dk) { signalReady(dk); return; }
```

`prepareClient()` bufferMs 两处:
Shared(原 `REFERENCE_TIME dur = 10'000'000 / 10; // 100 ms buffer`):
```cpp
        REFERENCE_TIME dur = params_.bufferMs
            ? static_cast<REFERENCE_TIME>(params_.bufferMs) * 10'000
            : 10'000'000 / 10; // default: 100 ms buffer
```
Exclusive(原 `REFERENCE_TIME dur = minPer;`):
```cpp
    REFERENCE_TIME dur = params_.bufferMs
        ? static_cast<REFERENCE_TIME>(params_.bufferMs) * 10'000
        : minPer;
```
(对齐重试逻辑保持不变;注:Exclusive 重试路径会重新 Activate `client_`,但高级 client-props 在 Exclusive 已被 open 拒绝,无丢失问题——在 realign 处加一行注释说明。)

- [ ] **Step 4: 构建 + 测试通过**

Run: `.\build.bat Debug` 然后 `.\build\bin\Debug\WinAudioTests.exe --gtest_filter=StreamParams.*` 再全量。
Expected: StreamParams 5/5,全量 65 PASSED,0 警告。

- [ ] **Step 5: 提交**

```bash
git add src/core/WasapiStream.h src/core/WasapiStream.cpp src/tests/test_streamparams.cpp
git commit -m "feat(core): inject StreamParams into WASAPI open path (SetClientProperties/bufferMs/ducking)"
```

---

### Task 3: MonitorEngine 参数插管

**Files:**
- Modify: `src/core/MonitorEngine.h`(start 签名、setRenderParams、成员)
- Modify: `src/core/MonitorEngine.cpp`(存参、capture open 传参、engage 快照)
- Test: `src/tests/test_monitorengine.cpp`(2 个新测试)

**Interfaces:**
- Consumes: T1 fake 的 `lastOpenParams_`;`StreamParams`。
- Produces: `MonitorEngine::start(kind, capId, renderId, delayMs, playbackEnabled = true, const StreamParams& capParams = {}, const StreamParams& renderParams = {})`;`void MonitorEngine::setRenderParams(const StreamParams& p)`(线程安全,下次 engage 生效)——T4 GUI 依赖这两个签名。

- [ ] **Step 1: 写失败测试** — `test_monitorengine.cpp` 追加(复用该文件既有的等待/驱动模式,参考 `DisablePlaybackStopsRender` 的写法;`FakeRig` 已有 `renderOpenCount`/`renderPtr`):

```cpp
TEST(MonitorEngine, StreamParamsReachBackends) {
    FakeRig rig;
    MonitorEngine eng(rig.factory());
    StreamParams cap; cap.bufferMs = 30;
    StreamParams ren; ren.category = AudioCategory::Media; ren.ducking = DuckingMode::OptOut;
    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, L"", L"", 100, true, cap, ren));
    ASSERT_NE(rig.capPtr, nullptr);
    ASSERT_NE(rig.renderPtr, nullptr);
    EXPECT_EQ(rig.capPtr->lastOpenParams_.bufferMs, 30u);
    EXPECT_EQ(rig.capPtr->lastOpenParams_.category, AudioCategory::Default);
    EXPECT_EQ(rig.renderPtr->lastOpenParams_.category, AudioCategory::Media);
    EXPECT_EQ(rig.renderPtr->lastOpenParams_.ducking,  DuckingMode::OptOut);
    eng.stop();
}

TEST(MonitorEngine, SetRenderParamsAppliesOnReengage) {
    FakeRig rig;
    MonitorEngine eng(rig.factory());
    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, L"", L"", 100, true));   // params 全默认
    ASSERT_NE(rig.renderPtr, nullptr);
    EXPECT_EQ(rig.renderPtr->lastOpenParams_.option, StreamOption::Default);

    StreamParams np; np.option = StreamOption::Raw; np.bufferMs = 20;
    eng.setRenderParams(np);                    // 运行中改参数
    eng.setPlaybackEnabled(false);              // 关播放(disengage)
    // 等 pump 完成 disengage(复用文件里现成的轮询等待写法):
    // waitFor([&]{ return rig.renderStopped.load(); })
    eng.setPlaybackEnabled(true);               // 重新勾上 -> re-engage 用新参数
    // waitFor([&]{ return rig.renderOpenCount.load() >= 2; })
    EXPECT_EQ(rig.renderPtr->lastOpenParams_.option,   StreamOption::Raw);
    EXPECT_EQ(rig.renderPtr->lastOpenParams_.bufferMs, 20u);
    eng.stop();
}
```
(两处 `waitFor` 注释:用该文件既有等待 helper/写法替换,若无则加本地 `waitFor` 轮询模板,2s 超时 5ms 步进。)

- [ ] **Step 2: 构建确认失败**

Run: `.\build.bat Debug`
Expected: 编译错误(start 无 7 参重载 / setRenderParams 不存在)。

- [ ] **Step 3: 实现** —

`MonitorEngine.h`:`#include "StreamParams.h"`(include 区);签名改为:
```cpp
    Result start(BackendKind kind, const DeviceId& capId, const DeviceId& renderId,
                 uint32_t delayMs, bool playbackEnabled = true,
                 const StreamParams& capParams = {}, const StreamParams& renderParams = {});
    void   setRenderParams(const StreamParams& p);   // 运行中可调;下次 engage 取快照生效
```
成员区(`std::atomic<bool> wantPlayback_{false};` 附近)加:
```cpp
    StreamParams capParams_{};      // start 时消费(采集参数改动需 Stop/Start)
    StreamParams renderParams_{};   // paramsMtx_ 保护;engageRender 取快照
    std::mutex   paramsMtx_;
```
(头部需 `#include <mutex>`。)

`MonitorEngine.cpp`:
`start` 签名同步,`kind_ = kind; renderId_ = renderId; delayMs_ = delayMs;` 行后加:
```cpp
    capParams_ = capParams;
    { std::lock_guard<std::mutex> lk(paramsMtx_); renderParams_ = renderParams; }
```
capture open(T1 的 `StreamParams{}`)改为:
```cpp
    if (Result r = capBackend_->open(capId, AudioFormat{}, captureRing_.get(), capParams_); !r)
```
`engageRender()` 开头取快照,open 传快照:
```cpp
    StreamParams rp;
    { std::lock_guard<std::mutex> lk(paramsMtx_); rp = renderParams_; }
```
```cpp
    if (Result r = renderBackend_->open(renderId_, AudioFormat{}, renderRing_.get(), rp); !r) {
```
新方法:
```cpp
void MonitorEngine::setRenderParams(const StreamParams& p) {
    std::lock_guard<std::mutex> lk(paramsMtx_);
    renderParams_ = p;
}
```

- [ ] **Step 4: 构建 + 测试通过**

Run: `.\build.bat Debug` 然后 `.\build\bin\Debug\WinAudioTests.exe --gtest_filter=MonitorEngine.*` 再全量。
Expected: MonitorEngine 12/12(10 旧 + 2 新),全量 67 PASSED,0 警告。

- [ ] **Step 5: 提交**

```bash
git add src/core/MonitorEngine.h src/core/MonitorEngine.cpp src/tests/test_monitorengine.cpp
git commit -m "feat(core): plumb StreamParams through MonitorEngine (start params + setRenderParams re-engage)"
```

---

### Task 4: GUI(设备下拉框 + Advanced 弹窗)+ 文档

**Files:**
- Modify: `src/gui/AppUi.h`(成员 + drawAdvancedModal 声明)
- Modify: `src/gui/AppUi.cpp`(refreshMonitorDevices 选择规则、ListBox→Combo、Advanced 按钮 + 弹窗、Start 传参)
- Modify: `CLAUDE.md`(GUI 段落一句)

**Interfaces:**
- Consumes: T3 的 `start(..., capParams, renderParams)` 与 `setRenderParams`;`wa::StreamParams` 及其枚举(经 `MonitorEngine.h` 已可见)。

- [ ] **Step 1: AppUi.h** — private 方法区加 `void drawAdvancedModal();`;成员区(`int delayMs_ = 100;` 附近)加:

```cpp
    wa::StreamParams capParams_{};    // Advanced 弹窗编辑;Start 时传入
    wa::StreamParams renParams_{};
    bool             advCapDirty_ = false;  // 运行中改过采集参数 -> 弹窗关闭时日志提示
```

- [ ] **Step 2: refreshMonitorDevices 选择规则**(替换现函数体,保持 ComInitGuard):

```cpp
void AppUi::refreshMonitorDevices() {
    wa::ComInitGuard com;   // REQUIRED: GUI thread has no COM; DeviceEnumerator needs it
    const wa::DeviceId prevCap = (capDevIdx_ >= 0 && capDevIdx_ < (int)capDevices_.size())
                                     ? capDevices_[(size_t)capDevIdx_].id : L"";
    const wa::DeviceId prevRen = (renderDevIdx_ >= 0 && renderDevIdx_ < (int)renderDevices_.size())
                                     ? renderDevices_[(size_t)renderDevIdx_].id : L"";
    capDevices_.clear();
    renderDevices_.clear();
    enumerator_.enumerate(wa::DataFlow::Capture, capDevices_);
    enumerator_.enumerate(wa::DataFlow::Render,  renderDevices_);
    // Keep the previously selected device if it still exists; else the system default; else 0.
    auto pick = [](const std::vector<wa::DeviceInfo>& devs, const wa::DeviceId& prev) -> int {
        if (!prev.empty())
            for (int i = 0; i < (int)devs.size(); ++i)
                if (devs[(size_t)i].id == prev) return i;
        for (int i = 0; i < (int)devs.size(); ++i)
            if (devs[(size_t)i].isDefault) return i;
        return 0;
    };
    capDevIdx_    = pick(capDevices_, prevCap);
    renderDevIdx_ = pick(renderDevices_, prevRen);
    monitorDevicesLoaded_ = true;
}
```
(首次调用 prev 为空 → 直接落在系统默认设备 = 需求"启动默认选中系统默认设备"。)

- [ ] **Step 3: ListBox→Combo + Advanced 按钮** — 替换 drawLeftPanel 的两段 `BeginListBox`:

```cpp
    auto deviceCombo = [&](const char* caption, const char* comboId,
                           const std::vector<wa::DeviceInfo>& devs, int& idx) {
        ImGui::TextUnformatted(caption);
        std::string preview = "(no devices)";
        if (!devs.empty() && idx >= 0 && idx < (int)devs.size())
            preview = (devs[(size_t)idx].isDefault ? "* " : "") + wtou(devs[(size_t)idx].name);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo(comboId, preview.c_str())) {
            for (int i = 0; i < (int)devs.size(); ++i) {
                std::string l = (devs[(size_t)i].isDefault ? "* " : "  ") + wtou(devs[(size_t)i].name);
                if (ImGui::Selectable((l + "##" + std::to_string(i)).c_str(), idx == i))
                    idx = i;
            }
            ImGui::EndCombo();
        }
    };
    deviceCombo("Capture device", "##capdev", capDevices_, capDevIdx_);
    deviceCombo("Render device",  "##rendev", renderDevices_, renderDevIdx_);
    if (ImGui::Button("Advanced...")) ImGui::OpenPopup("Audio parameters (advanced)");
    drawAdvancedModal();
```

- [ ] **Step 4: drawAdvancedModal**(新方法,放 drawLeftPanel 之后;需 `#include <algorithm>` 已有):

```cpp
void AppUi::drawAdvancedModal() {
    if (!ImGui::BeginPopupModal("Audio parameters (advanced)", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("All values default to system-recommended (no override is injected unless "
                       "you change them). Category/option/offload/ducking require WASAPI-Shared; "
                       "only Buffer applies to Exclusive.");
    ImGui::Separator();
    static const char* kCats[] = {"System default", "Other", "Communications", "Media", "Movie",
                                  "Game chat", "Speech", "Sound effects", "Game media"};
    static const char* kOpts[] = {"System default", "Raw (bypass APO)", "Match format"};
    bool renChanged = false;

    ImGui::BeginGroup();                              // ---- Capture column
    ImGui::SeparatorText("Capture");
    ImGui::PushID("capP");
    ImGui::PushItemWidth(190);
    int v = (int)capParams_.category;
    if (ImGui::Combo("Category", &v, kCats, 9)) { capParams_.category = (wa::AudioCategory)v; advCapDirty_ = true; }
    v = (int)capParams_.option;
    if (ImGui::Combo("Stream option", &v, kOpts, 3)) { capParams_.option = (wa::StreamOption)v; advCapDirty_ = true; }
    v = (int)capParams_.bufferMs;
    if (ImGui::InputInt("Buffer (ms)", &v)) { capParams_.bufferMs = (uint32_t)std::clamp(v, 0, 2000); advCapDirty_ = true; }
    if (ImGui::Button("Reset to system defaults")) { capParams_ = wa::StreamParams{}; advCapDirty_ = true; }
    ImGui::PopItemWidth();
    ImGui::PopID();
    ImGui::EndGroup();

    ImGui::SameLine(0, 24);

    ImGui::BeginGroup();                              // ---- Render column
    ImGui::SeparatorText("Render");
    ImGui::PushID("renP");
    ImGui::PushItemWidth(190);
    v = (int)renParams_.category;
    if (ImGui::Combo("Category", &v, kCats, 9)) { renParams_.category = (wa::AudioCategory)v; renChanged = true; }
    v = (int)renParams_.option;
    if (ImGui::Combo("Stream option", &v, kOpts, 3)) { renParams_.option = (wa::StreamOption)v; renChanged = true; }
    bool off = renParams_.offload == wa::OffloadMode::Force;
    if (ImGui::Checkbox("Hardware offload", &off)) { renParams_.offload = off ? wa::OffloadMode::Force : wa::OffloadMode::Default; renChanged = true; }
    bool duck = renParams_.ducking == wa::DuckingMode::OptOut;
    if (ImGui::Checkbox("Ducking opt-out", &duck)) { renParams_.ducking = duck ? wa::DuckingMode::OptOut : wa::DuckingMode::Default; renChanged = true; }
    v = (int)renParams_.bufferMs;
    if (ImGui::InputInt("Buffer (ms)", &v)) { renParams_.bufferMs = (uint32_t)std::clamp(v, 0, 2000); renChanged = true; }
    if (ImGui::Button("Reset to system defaults")) { renParams_ = wa::StreamParams{}; renChanged = true; }
    ImGui::PopItemWidth();
    ImGui::PopID();
    ImGui::EndGroup();

    if (renChanged && monitorStarted_) {
        monitor_.setRenderParams(renParams_);
        logLines_.push_back("render params updated; re-toggle playback to apply");
    }
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0))) {
        if (advCapDirty_ && monitorStarted_)
            logLines_.push_back("capture params take effect on next Start");
        advCapDirty_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
```

- [ ] **Step 5: Start 传参** — drawLeftPanel 的 `monitor_.start(...)` 改为:

```cpp
            wa::Result r = monitor_.start(kind, capId, renId, (uint32_t)delayMs_,
                                          playbackEnabled_, capParams_, renParams_);
```

- [ ] **Step 6: CLAUDE.md** — `## 构建与运行` GUI 段落("选择采集设备后点击 Start"之前)加一句:

```
# 设备为下拉框选择,启动时默认选中系统默认设备;"Advanced..." 弹窗可配置高级流参数
# (category / stream option(RAW=绕过 APO)/ offload / ducking / buffer ms),
# 默认全部"跟随系统"(不注入任何覆盖);除 buffer 外仅 WASAPI-Shared 生效。
```

- [ ] **Step 7: 构建 + 全量测试 + GUI 存活**

Run: `.\build.bat Debug`;`.\build\bin\Debug\WinAudioTests.exe`;`.\build.bat Release`;GUI 存活(Start-Process → Sleep 3 → 未退出 → Stop-Process)。
Expected: 双配置 0 错 0 警,67 PASSED,"GUI started OK"。

- [ ] **Step 8: 提交**

```bash
git add src/gui/AppUi.h src/gui/AppUi.cpp CLAUDE.md
git commit -m "feat(gui): device dropdowns (default = system default) + Advanced stream-params modal"
```

---

## 验收对照(spec §9)

1. 全默认零行为变化:T1 即保证(60 旧测试全绿 + CLI 传 `StreamParams{}`)。
2. 启动即选中系统默认设备:T4 Step 2(`pick` 兜底 isDefault)。
3. Advanced 弹窗布局与初值:T4 Step 4。
4. SetClientProperties 于 Initialize 前 / Exclusive 拒绝:T2(代码位置 + `ExclusiveRejectsAdvancedParams` 单测)。
5. fake 端到端 + re-engage:T3 两个测试。
6. 手动冒烟(硬件):运行中改渲染参数→重勾播放生效;RAW/offload/ducking 实际效果依赖设备,人工验证。
