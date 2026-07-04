# WinAudio 数据格式功能 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 WinAudio 增加音频数据格式的显示/枚举/能力查询/设置能力，贯穿 core/CLI/GUI，理念"默认就好、允许修改"。

**Architecture:** core 新增纯函数能力层（`Capabilities.*`：候选空间 + 能力矩阵 + 默认格式选择，全部注入探测谓词故可无硬件单测）；`DeviceEnumerator` 扩展三来源查询 + 真实 `queryCapabilities`（复用 `IsFormatSupported` 探测模板）；`WasapiStream` Shared 路径改用指定格式（引擎转换）；`MonitorEngine`/CLI/GUI 把格式插管进来。

**Tech Stack:** C++17，MSVC v143，CMake（VS 2022 生成器），WASAPI（`IAudioClient`/`IMMDevice` property store），gtest，Dear ImGui + ImPlot。

## Global Constraints

- **无重采样**：Shared 转换交给 WASAPI 引擎（`Initialize` 传指定格式）；Exclusive 无转换、格式须硬件原生支持。
- **monitor 采集/渲染采样率必须一致**。
- **全默认路径字节级不变**：用户不选/不改格式时，Shared 走 `GetMixFormat`、Exclusive 走探测默认，与现状一致。
- 静态 CRT（`/MT`、`/MTd`）、`/W4` 零告警、C++17、core 仅 Win32 + STL（零外部依赖）。
- **每任务收尾验证**（下称"双验证"）：`.\build.bat Debug` 且 `.\build.bat Release` 均零告警；`.\test.bat Debug` 且 `.\test.bat Release` 全绿（67 现有 + 本计划新增）；触及 GUI 的任务额外做 GUI 存活（启动 3s 不崩、`Stop-Process`）。构建前若 GUI 在跑先 `Get-Process WinAudioGui | Stop-Process -Force`。

---

## File Structure

- **Create** `src/core/Capabilities.h` / `.cpp` — `FormatSupport`、`DeviceCapabilities` 结构；`allFormatCandidates()`（48 候选）；`buildCapabilityMatrix(cands, sharedPred, exclusivePred)`；`chooseDefaultFormat(kind, mix, deviceFmt?, exclCands, exclusivePred)`。纯函数，注入谓词。
- **Create** `src/tests/test_capabilities.cpp` — Task 1 的 TDD 测试。
- **Modify** `src/core/IAudioBackend.h` — `DeviceInfo` 加 `deviceFormat`/`oemFormat` + `hasDeviceFormat`/`hasOemFormat`。
- **Modify** `src/core/DeviceEnumerator.h` / `.cpp` — 加 `deviceFormat()`/`oemFormat()`/`queryCapabilities()`；`readInfo` 填三来源。
- **Modify** `src/core/WasapiStream.cpp` — `prepareClient` Shared 分支用指定格式。
- **Modify** `src/core/MonitorEngine.h` / `.cpp` — `start()` 加 `capFormat`；`BackendFactory` 加 requested 参数。
- **Modify** `src/tests/test_monitorengine.cpp` — 更新 `FakeRig::factory` 签名 + `FakeBackend` 记录 requested；加 capFormat 到达断言。
- **Modify** `src/cli/main.cpp` — `--format` 放行 shared；`monitor --format`；`caps` 子命令；`usage`。
- **Modify** `src/gui/AppUi.h` / `.cpp` — 左栏格式区 + `Device caps…` 弹窗 + 新成员。
- **Modify** `src/core/CMakeLists.txt`、`src/tests/CMakeLists.txt` — 注册新源。
- **Modify** `CLAUDE.md` — 文档。

---

### Task 1: core 能力层纯函数（Capabilities）

**Files:**
- Create: `src/core/Capabilities.h`, `src/core/Capabilities.cpp`, `src/tests/test_capabilities.cpp`
- Modify: `src/core/CMakeLists.txt`（`MonitorEngine.cpp` 后加 `    Capabilities.cpp`）、`src/tests/CMakeLists.txt`（`test_spectrogram.cpp` 后加 `    test_capabilities.cpp`）

**Interfaces:**
- Consumes: `AudioFormat`（`AudioFormatDef.h`）、`BackendKind`（`Engine.h`）、`selectSupportedFormat`（`FormatSpec.h`）。
- Produces:
  ```cpp
  struct FormatSupport { AudioFormat fmt; bool sharedOk; bool exclusiveOk; };
  struct DeviceCapabilities {
      AudioFormat mixFormat{}, deviceFormat{}, oemFormat{};
      bool hasMix=false, hasDevice=false, hasOem=false;
      std::vector<FormatSupport> matrix;
  };
  std::vector<AudioFormat> allFormatCandidates();           // 48
  std::vector<FormatSupport> buildCapabilityMatrix(
      const std::vector<AudioFormat>& cands,
      const std::function<bool(const AudioFormat&)>& sharedPred,
      const std::function<bool(const AudioFormat&)>& exclusivePred);
  AudioFormat chooseDefaultFormat(
      BackendKind kind, const AudioFormat& mixFormat,
      const AudioFormat* deviceFormat,
      const std::vector<AudioFormat>& exclusiveCandidates,
      const std::function<bool(const AudioFormat&)>& exclusivePred);
  ```

- [ ] **Step 1: 写失败测试** — `src/tests/test_capabilities.cpp`

```cpp
#include <gtest/gtest.h>
#include "Capabilities.h"
#include "FormatSpec.h"
using namespace wa;

TEST(Capabilities, CandidateSpaceIs48) {
    auto c = allFormatCandidates();
    EXPECT_EQ(c.size(), 48u);
    // 含 48000/16/2 int 与 48000/32/2 float
    EXPECT_NE(std::find(c.begin(), c.end(), AudioFormat{48000,2,16,false}), c.end());
    EXPECT_NE(std::find(c.begin(), c.end(), AudioFormat{48000,2,32,true}),  c.end());
}

TEST(Capabilities, MatrixReflectsPredicates) {
    std::vector<AudioFormat> cands = {{48000,2,16,false}, {96000,2,24,false}};
    auto m = buildCapabilityMatrix(cands,
        [](const AudioFormat& f){ return f.sampleRate == 48000; },          // sharedPred
        [](const AudioFormat& f){ return f.bitsPerSample == 24; });         // exclusivePred
    ASSERT_EQ(m.size(), 2u);
    EXPECT_TRUE (m[0].sharedOk);  EXPECT_FALSE(m[0].exclusiveOk);   // 48000/16
    EXPECT_FALSE(m[1].sharedOk);  EXPECT_TRUE (m[1].exclusiveOk);   // 96000/24
}

TEST(Capabilities, DefaultSharedIsMix) {
    AudioFormat mix{44100,2,32,true};
    auto d = chooseDefaultFormat(BackendKind::WasapiShared, mix, nullptr, {}, [](const AudioFormat&){return false;});
    EXPECT_EQ(d, mix);
}

TEST(Capabilities, DefaultExclusivePrefersDeviceFormat) {
    AudioFormat dev{48000,1,16,false};
    auto d = chooseDefaultFormat(BackendKind::WasapiExclusive, AudioFormat{48000,2,32,true},
                                 &dev, defaultExclusiveCaptureCandidates(),
                                 [](const AudioFormat&){ return true; });   // 一切支持
    EXPECT_EQ(d, dev);                                                      // 首选 deviceFormat
}

TEST(Capabilities, DefaultExclusiveFallsBackWhenDeviceUnsupported) {
    AudioFormat dev{12345,7,16,false};                                      // 不在候选、pred 拒
    auto cands = defaultExclusiveCaptureCandidates();                       // 首项 48000/2/16
    auto d = chooseDefaultFormat(BackendKind::WasapiExclusive, AudioFormat{},
                                 &dev, cands,
                                 [](const AudioFormat& f){ return f == AudioFormat{48000,2,16,false}; });
    EXPECT_EQ(d, (AudioFormat{48000,2,16,false}));
}
```

- [ ] **Step 2: 跑测试确认失败** — `.\build.bat Debug` 期望编译失败（`Capabilities.h` not found）。

- [ ] **Step 3: 写 `Capabilities.h`**

```cpp
#pragma once
#include <functional>
#include <vector>
#include "AudioFormatDef.h"
#include "Engine.h"   // BackendKind
namespace wa {
struct FormatSupport { AudioFormat fmt{}; bool sharedOk=false; bool exclusiveOk=false; };
struct DeviceCapabilities {
    AudioFormat mixFormat{}, deviceFormat{}, oemFormat{};
    bool hasMix=false, hasDevice=false, hasOem=false;
    std::vector<FormatSupport> matrix;
};
std::vector<AudioFormat> allFormatCandidates();
std::vector<FormatSupport> buildCapabilityMatrix(
    const std::vector<AudioFormat>& cands,
    const std::function<bool(const AudioFormat&)>& sharedPred,
    const std::function<bool(const AudioFormat&)>& exclusivePred);
AudioFormat chooseDefaultFormat(
    BackendKind kind, const AudioFormat& mixFormat, const AudioFormat* deviceFormat,
    const std::vector<AudioFormat>& exclusiveCandidates,
    const std::function<bool(const AudioFormat&)>& exclusivePred);
} // namespace wa
```

- [ ] **Step 4: 写 `Capabilities.cpp`**

```cpp
#include "Capabilities.h"
#include "FormatSpec.h"
namespace wa {
std::vector<AudioFormat> allFormatCandidates() {
    static const uint32_t rates[] = {44100,48000,88200,96000,176400,192000};
    struct D { uint16_t bits; bool isFloat; };
    static const D depths[] = {{16,false},{24,false},{32,false},{32,true}};
    static const uint16_t chans[] = {1,2};
    std::vector<AudioFormat> out;
    out.reserve(48);
    for (uint32_t r : rates)
        for (const D& d : depths)
            for (uint16_t c : chans)
                out.push_back(AudioFormat{r, c, d.bits, d.isFloat});
    return out;
}
std::vector<FormatSupport> buildCapabilityMatrix(
    const std::vector<AudioFormat>& cands,
    const std::function<bool(const AudioFormat&)>& sharedPred,
    const std::function<bool(const AudioFormat&)>& exclusivePred) {
    std::vector<FormatSupport> m;
    m.reserve(cands.size());
    for (const AudioFormat& f : cands)
        m.push_back(FormatSupport{f, sharedPred(f), exclusivePred(f)});
    return m;
}
AudioFormat chooseDefaultFormat(
    BackendKind kind, const AudioFormat& mixFormat, const AudioFormat* deviceFormat,
    const std::vector<AudioFormat>& exclusiveCandidates,
    const std::function<bool(const AudioFormat&)>& exclusivePred) {
    if (kind == BackendKind::WasapiShared) return mixFormat;
    std::vector<AudioFormat> cands;
    if (deviceFormat) cands.push_back(*deviceFormat);
    for (const AudioFormat& c : exclusiveCandidates) cands.push_back(c);
    int idx = selectSupportedFormat(cands, exclusivePred);
    if (idx >= 0) return cands[(size_t)idx];
    return exclusiveCandidates.empty() ? mixFormat : exclusiveCandidates.front();
}
} // namespace wa
```

- [ ] **Step 5: 注册 CMake** — `src/core/CMakeLists.txt` 的 `MonitorEngine.cpp` 后加 `    Capabilities.cpp`；`src/tests/CMakeLists.txt` 的 `test_spectrogram.cpp` 后加 `    test_capabilities.cpp`。`test_capabilities.cpp` 需 `#include <algorithm>`（`std::find`）。

- [ ] **Step 6: 跑测试确认通过** — `.\build.bat Debug` 后 `.\build\bin\Debug\WinAudioTests.exe --gtest_filter=Capabilities.*` 期望 5 例全 PASS。

- [ ] **Step 7: 双验证 + 提交**

```bash
git add src/core/Capabilities.h src/core/Capabilities.cpp src/tests/test_capabilities.cpp src/core/CMakeLists.txt src/tests/CMakeLists.txt
git commit -m "feat(core): format capability matrix + default-format chooser (pure, predicate-injected)"
```

---

### Task 2: DeviceInfo 三来源字段 + DeviceEnumerator 三来源读取

**Files:**
- Modify: `src/core/IAudioBackend.h:15-21`（`DeviceInfo`）、`src/core/DeviceEnumerator.h`、`src/core/DeviceEnumerator.cpp`（`readInfo` + 新方法）

**Interfaces:**
- Produces:
  ```cpp
  // DeviceInfo 追加：
  AudioFormat deviceFormat{}; bool hasDeviceFormat=false;
  AudioFormat oemFormat{};    bool hasOemFormat=false;
  // DeviceEnumerator 追加：
  Result deviceFormat(const DeviceId& id, AudioFormat& out);   // PKEY_AudioEngine_DeviceFormat
  Result oemFormat(const DeviceId& id, AudioFormat& out);      // PKEY_AudioEngine_OemFormat
  ```

**测试策略：** 读 property store 是 I/O，无硬件不可单测；blob→`AudioFormat` 由既有 `fromWaveFormat`（`test_audioformat.cpp`）覆盖。本任务靠 **Task 7 的 `caps` smoke** 核对控制面板。步骤只做实现 + 双验证（不新增单测），并确认 67 测试不回归。

- [ ] **Step 1: `IAudioBackend.h` 扩展 `DeviceInfo`** — 在 `AudioFormat mixFormat{};` 后追加：

```cpp
    AudioFormat  deviceFormat{}; bool hasDeviceFormat = false; // PKEY_AudioEngine_DeviceFormat
    AudioFormat  oemFormat{};    bool hasOemFormat    = false; // PKEY_AudioEngine_OemFormat
```

- [ ] **Step 2: `DeviceEnumerator.h` 加声明** — 在 `mixFormat` 声明后：

```cpp
    Result deviceFormat(const DeviceId& id, AudioFormat& out);
    Result oemFormat(const DeviceId& id, AudioFormat& out);
```

- [ ] **Step 3: `DeviceEnumerator.cpp` 加读 blob 辅助 + 在 `readInfo` 填充** — 文件顶部（匿名命名空间或静态）加：

```cpp
// 读一个 PKEY 的 WAVEFORMATEX blob -> AudioFormat；成功返回 true。
static bool readFormatKey(IPropertyStore* props, const PROPERTYKEY& key, AudioFormat& out) {
    PROPVARIANT pv; PropVariantInit(&pv);
    bool ok = false;
    if (SUCCEEDED(props->GetValue(key, &pv)) && pv.vt == VT_BLOB &&
        pv.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        out = fromWaveFormat(reinterpret_cast<const WAVEFORMATEX*>(pv.blob.pBlobData));
        ok = true;
    }
    PropVariantClear(&pv);
    return ok;
}
```
在 `readInfo` 的 `OpenPropertyStore` block 里（读完 FriendlyName 后、同一 `props` 上）追加：

```cpp
    info.hasDeviceFormat = readFormatKey(props.Get(), PKEY_AudioEngine_DeviceFormat, info.deviceFormat);
    info.hasOemFormat    = readFormatKey(props.Get(), PKEY_AudioEngine_OemFormat,    info.oemFormat);
```
（`PKEY_AudioEngine_DeviceFormat`/`PKEY_AudioEngine_OemFormat` 已随 `<functiondiscoverykeys_devpkey.h>` 引入，无需新 include。）

- [ ] **Step 4: 抽出 `openDevice` 辅助 + 实现 `deviceFormat`/`oemFormat`** — `DeviceEnumerator.cpp` 匿名命名空间加辅助（`mixFormat` 亦可改用它以 DRY）：

```cpp
static Result openDevice(DataFlow flow, const DeviceId& id, ComPtr<IMMDevice>& dev) {
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(e.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "CoCreateInstance");
    const EDataFlow ef = (flow == DataFlow::Capture) ? eCapture : eRender;
    hr = id.empty() ? e->GetDefaultAudioEndpoint(ef, eConsole, dev.GetAddressOf())
                    : e->GetDevice(id.c_str(), dev.GetAddressOf());
    if (FAILED(hr)) return HrToResult(hr, "GetDevice");
    return Result::Ok();
}
```
独立方法（默认按 render 端点，与现有 `mixFormat(id)` 语义一致）：

```cpp
Result DeviceEnumerator::deviceFormat(const DeviceId& id, AudioFormat& out) {
    ComInitGuard com;
    ComPtr<IMMDevice> dev;
    if (Result r = openDevice(DataFlow::Render, id, dev); !r) return r;
    ComPtr<IPropertyStore> props;
    HRESULT hr = dev->OpenPropertyStore(STGM_READ, props.GetAddressOf());
    if (FAILED(hr)) return HrToResult(hr, "OpenPropertyStore");
    if (!readFormatKey(props.Get(), PKEY_AudioEngine_DeviceFormat, out))
        return Result::Fail(1, "DeviceFormat not present");
    return Result::Ok();
}
Result DeviceEnumerator::oemFormat(const DeviceId& id, AudioFormat& out) {
    ComInitGuard com;
    ComPtr<IMMDevice> dev;
    if (Result r = openDevice(DataFlow::Render, id, dev); !r) return r;
    ComPtr<IPropertyStore> props;
    HRESULT hr = dev->OpenPropertyStore(STGM_READ, props.GetAddressOf());
    if (FAILED(hr)) return HrToResult(hr, "OpenPropertyStore");
    if (!readFormatKey(props.Get(), PKEY_AudioEngine_OemFormat, out))
        return Result::Fail(1, "OemFormat not present");
    return Result::Ok();
}
```
（`ComInitGuard`/`ComPtr`/`HrToResult` 已在 `DeviceEnumerator.cpp` 使用；`eCapture`/`eRender`/`eConsole` 来自 `mmdeviceapi.h`。）

- [ ] **Step 5: 双验证（无新增单测）** — 双配置 build 零告警 + 67 测试全绿（确认未回归）。提交：

```bash
git add src/core/IAudioBackend.h src/core/DeviceEnumerator.h src/core/DeviceEnumerator.cpp
git commit -m "feat(core): DeviceEnumerator reads device/oem formats (three-source)"
```

---

### Task 3: 真实 queryCapabilities

**Files:** Modify `src/core/DeviceEnumerator.h`（声明）、`src/core/DeviceEnumerator.cpp`（实现）

**Interfaces:**
- Consumes: `buildCapabilityMatrix`/`DeviceCapabilities`（Task 1）、`deviceFormat`/`oemFormat`（Task 2）、`toWaveFormatExtensible`（`AudioFormat.h`）。
- Produces: `Result queryCapabilities(DataFlow flow, const DeviceId& id, DeviceCapabilities& out);`

**测试策略：** Activate + `IsFormatSupported` 是硬件 I/O，靠 Task 7 `caps` smoke；矩阵组装逻辑已由 Task 1 覆盖。仅实现 + 双验证 + 不回归。

- [ ] **Step 1: `DeviceEnumerator.h` 加声明 + include** — 头部 `#include "Capabilities.h"`；类内加 `Result queryCapabilities(DataFlow flow, const DeviceId& id, DeviceCapabilities& out);`

- [ ] **Step 2: `DeviceEnumerator.cpp` 实现**（复用 Task 2 的 `openDevice`/`readFormatKey`）

```cpp
Result DeviceEnumerator::queryCapabilities(DataFlow flow, const DeviceId& id, DeviceCapabilities& out) {
    ComInitGuard com;
    ComPtr<IMMDevice> dev;
    if (Result r = openDevice(flow, id, dev); !r) return r;
    ComPtr<IAudioClient> client;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                     reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "Activate");
    auto probe = [&](const AudioFormat& f, AUDCLNT_SHAREMODE sm) -> HRESULT {
        WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(f);
        WAVEFORMATEX* closest = nullptr;
        HRESULT h = client->IsFormatSupported(sm, reinterpret_cast<WAVEFORMATEX*>(&wfx),
                        (sm == AUDCLNT_SHAREMODE_EXCLUSIVE) ? nullptr : &closest);
        if (closest) CoTaskMemFree(closest);
        return h;
    };
    // Shared 的 S_FALSE(可转换但非精确) 也算“可用”；Exclusive 仅严格 S_OK。
    out.matrix = buildCapabilityMatrix(allFormatCandidates(),
        [&](const AudioFormat& f){ HRESULT h = probe(f, AUDCLNT_SHAREMODE_SHARED); return h == S_OK || h == S_FALSE; },
        [&](const AudioFormat& f){ return probe(f, AUDCLNT_SHAREMODE_EXCLUSIVE) == S_OK; });
    // 三来源
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
        out.mixFormat = fromWaveFormat(mix); out.hasMix = true; CoTaskMemFree(mix);
    }
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.GetAddressOf()))) {
        out.hasDevice = readFormatKey(props.Get(), PKEY_AudioEngine_DeviceFormat, out.deviceFormat);
        out.hasOem    = readFormatKey(props.Get(), PKEY_AudioEngine_OemFormat,    out.oemFormat);
    }
    return Result::Ok();
}
```

- [ ] **Step 3: 双验证 + 提交**

```bash
git add src/core/DeviceEnumerator.h src/core/DeviceEnumerator.cpp
git commit -m "feat(core): DeviceEnumerator::queryCapabilities (dual IsFormatSupported over 48 candidates + three sources)"
```

---

### Task 4: WasapiStream Shared 用指定格式

**Files:** Modify `src/core/WasapiStream.cpp:153-169`（`prepareClient` Shared 分支）

**Interfaces:** Consumes 既有 `requestedFormat_`/`hasRequested_`（构造传入）、`toWaveFormatExtensible`。行为：`hasRequested_` → 用它 `Initialize`（引擎转换）；否则回落 `GetMixFormat`（现状）。

**测试策略：** `Initialize` I/O 无单测；靠 CLI `capture --format`(shared) smoke + monitor smoke。Exclusive 分支不动，现有测试不回归。

- [ ] **Step 1: 改写 Shared 分支** — 用 Explore 底稿 §5 的完整改写替换 `WasapiStream.cpp:154-168`（`if (mode_ == WasapiMode::Shared) { ... }` 整块）：`hasRequested_` 时 `CoTaskMemFree(mix)` 后用 `toWaveFormatExtensible(requestedFormat_)` 做 `Initialize`，`actualFormat_=requestedFormat_`；`else` 分支保持原 `GetMixFormat` 路径。`bufferMs` 逻辑两分支都保留。

- [ ] **Step 2: 双验证 + 提交** — 双配置零告警 + 67 全绿（Exclusive/monitor 现有测试未变）。

```bash
git add src/core/WasapiStream.cpp
git commit -m "feat(core): WASAPI shared honors a requested format (engine converts); default falls back to mix"
```

---

### Task 5: MonitorEngine capFormat 插管

**Files:** Modify `src/core/MonitorEngine.h`、`src/core/MonitorEngine.cpp`、`src/tests/test_monitorengine.cpp`

**Interfaces:**
- Produces: `start(...)` 末加 `const AudioFormat* capFormat = nullptr`；`BackendFactory` 改为 `std::function<std::unique_ptr<IAudioBackend>(DataFlow, const AudioFormat*)>`。
- Consumes: 无新外部。

- [ ] **Step 1: 改 `test_monitorengine.cpp` 的 fake 记录 requested（先让新断言失败）** — `FakeBackend` 加成员 `AudioFormat lastRequested_{}; bool sawRequested_=false;`；`FakeRig::factory()` 的 lambda 签名改为 `[this](DataFlow flow, const AudioFormat* req)`，构造 capture FakeBackend 时记录 `if (req) { fb->lastRequested_=*req; fb->sawRequested_=true; }`。新增测试：

```cpp
TEST(MonitorEngine, CaptureFormatReachesBackend) {
    FakeRig rig;
    MonitorEngine eng(rig.factory());
    AudioFormat want{96000, 2, 24, false};
    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, L"", L"", 50, false, {}, {}, &want));
    ASSERT_NE(rig.capPtr, nullptr);
    EXPECT_TRUE(rig.capPtr->sawRequested_);
    EXPECT_EQ(rig.capPtr->lastRequested_, want);
    eng.stop();
}
```

- [ ] **Step 2: 跑测试确认失败** — `start` 尚无第 8 参 + factory 旧签名 → 编译失败。

- [ ] **Step 3: 改 `MonitorEngine.h`** — `BackendFactory` typedef 加 `const AudioFormat*`；`start` 末加 `const AudioFormat* capFormat = nullptr`；私有加 `AudioFormat capRequestedFormat_{}; bool hasCapFormat_ = false;`。

- [ ] **Step 4: 改 `MonitorEngine.cpp`** — `start` 开头存 `hasCapFormat_ = (capFormat != nullptr); if (capFormat) capRequestedFormat_ = *capFormat;`；`makeBackend` 签名加 `const AudioFormat* requested`，`factory_` 分支改为 `factory_(flow, requested)`，真实分支把 `requested` 传给 `WasapiCaptureStream`/`WasapiRenderStream` 构造（render 传 `nullptr`）；采集创建处（`:97`）改为 `makeBackend(DataFlow::Capture, kind, hasCapFormat_ ? &capRequestedFormat_ : nullptr)`。

- [ ] **Step 5: 跑测试确认通过** — `--gtest_filter=MonitorEngine.*` 全绿（含新例 + 原有）。

- [ ] **Step 6: 双验证 + 提交**

```bash
git add src/core/MonitorEngine.h src/core/MonitorEngine.cpp src/tests/test_monitorengine.cpp
git commit -m "feat(core): MonitorEngine threads an optional capture format to the backend"
```

---

### Task 6: CLI — `--format` 放行 shared + `monitor --format`

**Files:** Modify `src/cli/main.cpp`

**测试策略：** CLI 无单测，靠 smoke。步骤含 smoke 命令与期望。

- [ ] **Step 1: capture 放行 shared** — 删除/改写 `main.cpp:97-101` 的 shared 拒绝 block（shared + `--format` 现在合法：直接把 `haveFmt ? &fmt : nullptr` 传给 capture 路径的 requested）。

- [ ] **Step 2: monitor 接受 `--format`** — 在 monitor block（`:153`）前 `AudioFormat capFmt{}; bool haveFmt = formatArg(argc, argv, capFmt);`，`mon.start(...)` 末传 `haveFmt ? &capFmt : nullptr`。

- [ ] **Step 3: `usage()` 同步** — capture 行标注 `--format` 现对两后端有效；monitor 行加 `[--format R/B/C[f]]`。

- [ ] **Step 4: smoke + 双验证** — 双配置 build 零告警、67 全绿；手动：`WinAudioCli.exe capture --out t.wav --seconds 1 --backend wasapi-shared --format 44100/16/1`（应成功、WAV 头为 44100/16/1）。提交：

```bash
git add src/cli/main.cpp
git commit -m "feat(cli): --format applies to shared (engine converts); monitor accepts --format"
```

---

### Task 7: CLI — `caps` 子命令

**Files:** Modify `src/cli/main.cpp`（新 block + usage）

**Interfaces:** Consumes `DeviceEnumerator::queryCapabilities`。

- [ ] **Step 1: 加 `caps` block**（仿 `probe`，`:143` 附近）

```cpp
if (cmd == L"caps") {
    DataFlow flow = has(argc, argv, L"--capture") ? DataFlow::Capture : DataFlow::Render;
    DeviceId id = arg(argc, argv, L"--device");
    DeviceEnumerator de;
    DeviceCapabilities caps;
    wa::Result r = de.queryCapabilities(flow, id, caps);
    if (!r) { std::printf("caps failed: %s\n", r.message.c_str()); return 2; }
    auto pf = [](const char* tag, bool has, const AudioFormat& f){
        if (has) std::printf("%s: %u/%u/%u%s\n", tag, f.sampleRate, f.bitsPerSample, f.channels, f.isFloat?"f":"");
        else     std::printf("%s: (none)\n", tag);
    };
    pf("Mix",    caps.hasMix,    caps.mixFormat);
    pf("Device", caps.hasDevice, caps.deviceFormat);
    pf("OEM",    caps.hasOem,    caps.oemFormat);
    std::printf("%-16s %-8s %-9s\n", "Format", "Shared", "Exclusive");
    for (const auto& s : caps.matrix) {
        char fmt[32];
        std::snprintf(fmt, sizeof fmt, "%u/%u/%u%s", s.fmt.sampleRate, s.fmt.bitsPerSample, s.fmt.channels, s.fmt.isFloat?"f":"");
        std::printf("%-16s %-8s %-9s\n", fmt, s.sharedOk?"yes":"-", s.exclusiveOk?"yes":"-");
    }
    return 0;
}
```
（`caps` 需 `#include "Capabilities.h"`、`DeviceEnumerator.h`；`<cstdio>` 已在。）

- [ ] **Step 2: `usage()` 加行** — `"WinAudioCli caps  [--device <id>] [--render|--capture]\n"`。

- [ ] **Step 3: smoke + 双验证** — build 零告警、67 全绿；手动 `WinAudioCli.exe caps --capture` 打印三来源 + 48 行矩阵，核对与控制面板一致。提交：

```bash
git add src/cli/main.cpp
git commit -m "feat(cli): caps command prints three-source formats + capability matrix"
```

---

### Task 8: GUI — 左栏格式区

**Files:** Modify `src/gui/AppUi.h`、`src/gui/AppUi.cpp`（`drawLeftPanel` Control 区）

**Interfaces:** Consumes `DeviceEnumerator::queryCapabilities`、`chooseDefaultFormat`、`parseFormatSpec`、`MonitorEngine::start(..., capFormat)`。

**测试策略：** GUI 无单测；GUI 存活 + 人工 smoke。

- [ ] **Step 1: `AppUi.h` 加成员** — `wa::AudioFormat selectedFmt_{}; bool haveFmt_ = false; int fmtChoiceIdx_ = 0; char fmtCustom_[32] = "48000/16/2"; int fmtBackendShown_ = -1; wa::DeviceCapabilities capsCache_; bool capsCacheValid_ = false;` 及方法声明 `void drawFormatRegion(); void recomputeDefaultFormat();`

- [ ] **Step 2: `recomputeDefaultFormat()` 实现** — 用当前 `backendIdx_`/`capDevIdx_` 调 `enumerator_.queryCapabilities(Capture, id, capsCache_)`（切设备/模式时刷新，缓存），再 `selectedFmt_ = wa::chooseDefaultFormat(kind, capsCache_.mixFormat, capsCache_.hasDevice?&capsCache_.deviceFormat:nullptr, wa::defaultExclusiveCaptureCandidates(), exclPred)`，其中 `exclPred` 用 `capsCache_.matrix` 里 `exclusiveOk` 查表。`haveFmt_ = false`（默认不覆盖，等于走 backend 默认；仅当用户手动选/输入才置 true）。

- [ ] **Step 3: `drawFormatRegion()` 实现 + 插入** — 在 `AppUi.cpp` `SliderInt("Delay (ms)"...)`（`:176`）之后、`const ImVec2 ctrlBtn`（`:178`）之前调 `drawFormatRegion();`。区内容：`if (backendIdx_ != fmtBackendShown_) { recomputeDefaultFormat(); fmtBackendShown_ = backendIdx_; }`；一行 `Text("Format: %u/%u/%u%s", ...)`；下拉列 `capsCache_.matrix` 中当前模式 `ok` 的候选 + 末项 `"Custom…"`；选具体候选 → `selectedFmt_=该候选; haveFmt_=true;`；选 `Custom…` → 显示 `InputText(fmtCustom_)` + `Apply` 按钮（`parseFormatSpec(fmtCustom_, selectedFmt_)` 成功则 `haveFmt_=true` 否则 log "invalid format"）。

- [ ] **Step 4: Start 传格式** — `AppUi.cpp:185` 的 `monitor_.start(...)` 末参加 `haveFmt_ ? &selectedFmt_ : nullptr`。

- [ ] **Step 5: GUI 存活 + 双验证 + 提交** — 双配置零告警；启动 GUI 3s 存活；人工确认切 Shared/Exclusive 时 Format 行随之变、下拉可选、Custom 可输、Start 失败打日志。

```bash
git add src/gui/AppUi.h src/gui/AppUi.cpp
git commit -m "feat(gui): capture format region (mode default + dropdown + custom), passed to monitor"
```

---

### Task 9: GUI — Device caps 弹窗

**Files:** Modify `src/gui/AppUi.h`、`src/gui/AppUi.cpp`

- [ ] **Step 1: `AppUi.h` 加** — `void drawCapsModal();`

- [ ] **Step 2: 入口按钮** — 在 Devices 段（`Refresh devices`/`Options` 同区）加 `if (ImGui::Button("Device caps…")) { enumerator_.queryCapabilities(wa::DataFlow::Capture, capId, capsCache_); capsCacheValid_ = true; ImGui::OpenPopup("Device capabilities"); }`；随后 `drawCapsModal();`

- [ ] **Step 3: `drawCapsModal()` 实现** — `BeginPopupModal("Device capabilities", ...)`；顶部三来源并列（缺则 `—`）；`BeginTable("caps", 3, ...)`（列 Format/Shared/Exclusive），遍历 `capsCache_.matrix` 每行一格式 + `✓/✗`（滚动区 `BeginChild` 固定高度）；`Close` 按钮 `CloseCurrentPopup`。

- [ ] **Step 4: GUI 存活 + 双验证 + 提交** — 人工确认弹窗显示三来源 + 48 行矩阵、可滚动、可关。

```bash
git add src/gui/AppUi.h src/gui/AppUi.cpp
git commit -m "feat(gui): Device capabilities modal (three sources + one-row-per-format matrix)"
```

---

### Task 10: 文档

**Files:** Modify `CLAUDE.md`

- [ ] **Step 1: 更新 CLI 章节** — `caps` 命令用法；capture/monitor `--format` 现对 shared 有效（引擎转换）。
- [ ] **Step 2: 更新 GUI 章节** — 左栏格式区（模式默认 + 下拉 + 自定义）、`Device caps…` 弹窗。
- [ ] **Step 3: 更新能力范围** — "数据格式：三来源查询 + 能力矩阵 + 每模式默认 + 可改（shared 经引擎转换）"。
- [ ] **Step 4: 提交** — `git add CLAUDE.md && git commit -m "docs: data-format feature (caps, --format on shared, GUI format region)"`

---

## Self-Review

- **Spec 覆盖**：三来源查询→T2/T3；能力矩阵+候选空间→T1/T3；`defaultFormatFor`→T1（`chooseDefaultFormat`）；open 改动→T4；簇 A GUI→T8、CLI→T6；簇 B CLI→T7、GUI→T9；错误处理散落各任务（Start 失败打日志=T8/T6，缺来源=T2，解析失败=T8）；测试→T1（纯函数 TDD）+ T5（fake 到达）+ 其余 smoke（与 spec 测试节一致）。无遗漏。
- **类型一致**：`FormatSupport`/`DeviceCapabilities`（T1 定义）在 T3/T7/T8/T9 一致使用；`BackendFactory` 新签名（T5）与 `test_monitorengine`（T5 Step1）一致更新；`chooseDefaultFormat`/`queryCapabilities` 签名跨任务一致。
- **占位符**：全部步骤含完整代码或精确改点（抽出 `openDevice`/`readFormatKey` 复用后，T3 早期的 `…` 已消除），无 TODO。
