# WinAudio Phase 3 设计：延迟监听 + 实时可视化（波形/频谱/声谱图）

- 日期：2026-06-29
- 状态：已两轮多 agent + codex 评审并修订（v3），待用户评审 → writing-plans
- 依赖：Phase 1/2 已合并到 master（WASAPI Shared/Exclusive 单流 Engine）

## 1. 目标
GUI 内实时、分别绘制「采集」与「(延迟)播放」两路信号的 **时域波形** + **频域（FFT 频谱曲线 + 滚动声谱图）**，对标 Adobe Audition。运行模式为**带可配延迟的监听直通**（capture → 延迟 → render）。Phase 4 再做延迟/glitch 数值测量（本期不做）。

## 2. 已定决策
- 同时跑两路（采集+渲染并排对比）；频谱曲线+声谱图全套；引入 **ImPlot**（GUI-only 第三方）；渲染源 = 回放采集数据，带可配延迟（默认 100ms）。
- 时钟漂移：**主动水位控制（单样本级 drop/dup + 短 crossfade）**。
- 交付：**一个大计划**，但按 §9 的 9 个 gate 顺序落地以局部化失败。

## 3. 架构与数据流
```
[Capture I/O 线程] 设备字节 → captureRing(SPSC,帧对齐) ─┐
  (WasapiCaptureStream;每次写 ring 后 SetEvent(pumpReadyEvent_))
                                                        │  pump 线程(唯一做重活;预分配;事件驱动)
   ├─ pcmToFloat→downmixMono → captureScope(seqlock; {endIdx,window})  → GUI 波形/分析
   ├─ pcmToFloat(保留声道) → DelayFifo(帧域 float32; 目标占用=delayMs) ── drift 控制器(低通占用+deadband+单样本drop/dup+crossfade)
   └─ DelayFifo 出帧 → floatToPcm(render 设备格式) → renderRing(SPSC,帧对齐) → [Render I/O 线程] 仅 GetBuffer+memcpy/zero-fill
        └─(同一份 float32 mono)→ renderScope(seqlock; pump 单生产者) → GUI 回放波形/分析
```
- **pump 是唯一做转换/降混/切帧/drift 的线程**；capture/render I/O 线程只搬字节（回调只做 memcpy/zero-fill）。
- **pump 唤醒（关键修订）**：**不**共享 capture 的 WASAPI auto-reset 事件（双 waiter 会偷唤醒）。`IAudioBackend` 新增 `virtual void* dataReadyEvent() const { return nullptr; }`；`WasapiCaptureStream` 自建一个独立 auto-reset 事件 `pumpReadyEvent_`，其 I/O 线程每次写完 captureRing 后 `SetEvent(pumpReadyEvent_)`；pump `WaitForSingleObject(backend->dataReadyEvent(), timeoutMs)`。该事件也让**假 backend 可驱动 pump → 无硬件单测成立**。
- **帧对齐（机制而非口号）**：pump 经 `readFrames()` 助手读 ring——仅当 `availableRead() >= N*frameBytes` 才读，且读写尺寸恒为 blockAlign 整数倍，杜绝半帧错位。
- **DelayFifo 与 renderRing 分离**：`delayMs` **只绑定 DelayFifo 一个延迟域**；renderRing 仅设备字节传输（容量 ≥ 3× render 设备周期）；设备自身 buffering 在 status 中单独展示（UI 的 delayMs = 额外监听延迟，非端到端）。
- **drift 控制器（修订）**：观测**低通/时间平均**后的 DelayFifo 占用（窗口 ≫ 1 个设备周期）；deadband **必须 > 一个周期的 ring 锯齿幅度**；越界时按**单 sample-frame** 粒度 drop/dup，并用很短的 crossfade/ramp 避免咔哒。**控制变量 = 验收变量（统一为 DelayFifo 占用）**。每次校正计数。
- **采样率**：要求采集=渲染采样率，否则 `start()` 直接 Fail（无重采样器）。
- **启动顺序（修订）**：`start capture → pump 预填 DelayFifo 到 delayMs(+≥1 render 周期) → 再 start render(出声)`；预填阶段不计 renderXruns。
- **拆卸顺序（防死锁/UAF，修订）**：`stopFlag=true → SetEvent(pumpReadyEvent_) 唤醒 → join pump → stop capture backend → stop render backend → 释放 rings/scopes/fifo`。pump 循环非阻塞（`while(!stop){ wait(event,timeout); if(stop)break; readFrames; convert; pushScopes; fifo.push; drift; renderRing.tryWrite }`），renderRing 写用 tryWrite（满则计 overrun，不阻塞）。**join pump 在关闭事件句柄之前**。

## 4. Core 新增/改动

### 4.0 AudioFormat 拆分（前置重构，保零依赖边界）
新增 `src/core/AudioFormatDef.h`：纯 POD `AudioFormat{sampleRate,channels,bitsPerSample,isFloat}` + `blockAlign()`/`avgBytesPerSec()`/`operator==`，**不含任何 windows 头**。`AudioFormat.h` 改为 `#include "AudioFormatDef.h"` + windows 头 + `toWaveFormatExtensible/fromWaveFormat`。现有包含点（WasapiStream.h/DeviceEnumerator.h/Engine.h 等）按需改 include。→ `Fft/ScopeBuffer/SampleConvert/DelayFifo` 仅含 `AudioFormatDef.h`，保持 windows-free 可纯单测。

### 4.1 Fft（src/core/Fft.h/.cpp，纯 STL）
```cpp
void fftRadix2(std::complex<float>* data, size_t n);   // 原地基-2,n=2^k(低层原语,主要供测试)
void applyHann(float* inout, size_t n);                // 低层原语(供测试/换窗)
// 一站式频谱(GUI 唯一调用):内部 Hann + 零填充到 2^k + FFT + 单边幅度(dBFS)
// 归一化按【窗长 L=count】(非补零后的 N_fft);Hann 相干增益补偿;单边 ×2(DC/Nyquist 不×2);floorDb 下限
// 命名为 magnitude(20·log10),非 power
void magnitudeSpectrumDb(const float* samples, size_t count,
                         std::complex<float>* workBuf /*尺寸≥补零后的2^k*/,
                         std::vector<float>& magDbOut, float floorDb = -120.f);
```
**GUI 频谱路径只调 `magnitudeSpectrumDb`**（不得再单独 applyHann/fftRadix2，避免 Hann²/双 FFT）。单测：**on-bin** 满幅正弦(如 1007.8125Hz=43·48000/2048)→峰值 0 dBFS±0.1；**非 2^k**（如 count=1500 补零到 2048）满幅 on-bin 正弦仍 0 dBFS±0.5（验证按窗长归一化）；冲激近平坦；floor 生效。

### 4.2 ScopeBuffer（src/core/ScopeBuffer.h/.cpp，单生产者 pump / 单消费者 GUI）
```cpp
class ScopeBuffer {
  explicit ScopeBuffer(size_t capacitySamples);     // 默认 ~2s@48k=96000(可配)
  void push(const float* mono, size_t n);           // pump 写,覆盖最旧
  uint64_t totalWritten() const;                    // 单调样本计数(atomic acquire)
  // 一致快照:同一 seq epoch 内同时校验游标+拷贝最近 n 个样本;返回实际 endSampleIndex
  // 窗口已滚出(超 capacity)→返回 false。要求 n ≤ capacity/2。
  bool snapshotLatest(size_t n, float* out, uint64_t& endSampleIndexOut) const;
};
```
**seqlock 内存序（明确）**：写者写数据→`release` 递增 seq；读者 `acquire` 读 seq、读数据、再 `acquire` 校验 seq，不等则重试（重试上限兜底，配合滚出 false）。滚出校验在 seq 段内完成。`snapshotLatest` 原子返回窗口及其 endIndex（游标与数据同 epoch）。约定：out[0]=最旧，值域[-1,1]。

### 4.3 SampleConvert（src/core/SampleConvert.h/.cpp，纯函数，仅含 AudioFormatDef.h）
```cpp
void pcmToFloat(const uint8_t* in, size_t frames, const AudioFormat&, float* outInterleaved); // int16(÷32768)/int24(符号扩展)/int32/float32
void floatToPcm(const float* inInterleaved, size_t frames, const AudioFormat&, uint8_t* out); // 缩放后【对整数 clamp 到[MIN,MAX]】
void downmixMono(const float* interleaved, size_t frames, uint16_t ch, float* monoOut);       // 取平均(÷ch)
void adaptChannels(const float* in, uint16_t inCh, float* out, uint16_t outCh, size_t frames);// 仅 1↔2 明确;>2 降混
float peakLevel(const float* mono, size_t n);  // 由 Engine 私有 computeLevels 抽出复用(顺带支持 int24/int32 路径)
```
单测：int16 INT16_MIN/MAX 边界、int24 负值符号扩展 round-trip、float→int 越界 clamp（含 round-to-nearest 进位）、降混平均(满幅立体声仍≤0dBFS)、声道适配。

### 4.4 DelayFifo + DriftController（src/core/DelayFifo.h/.cpp，纯逻辑可单测）
帧域 float32 FIFO；目标占用=delayMs；占用低通；deadband>1 周期；单样本 drop/dup + 短 crossfade；计数 driftFixes。单测（注入偏速喂数据）：低通占用稳定在目标±deadband、drop/dup 计数与净漂移一致、crossfade 不产生越界样本。

### 4.5 MonitorEngine（src/core/MonitorEngine.h/.cpp，新类）
```cpp
enum class StreamState { Idle, Running, Error };
struct MonitorStatus {
  StreamState overall, capState, renderState;     // 读取来源为 atomic<StreamState>
  uint32_t sampleRate, delayMs; float fifoFillMs; // fifoFillMs=低通后占用
  uint32_t renderBufMs;                            // render 设备 buffering(与 delayMs 分开展示)
  uint64_t driftFixes, capXruns, renderXruns; float capLevel, renderLevel; // atomic 来源
  uint32_t errorCode;                              // 替代 std::string(lock-free poll 不竞争);GUI 静态映射文案
};
class MonitorEngine {
  using BackendFactory = std::function<std::unique_ptr<IAudioBackend>(DataFlow)>; // 默认造 WASAPI;测试注入假 backend
  explicit MonitorEngine(BackendFactory = {});
  Result start(BackendKind, const DeviceId& capId, const DeviceId& renderId, uint32_t delayMs); // rate 校验+预填序+严格回滚
  void   stop();                                   // §3 拆卸顺序
  MonitorStatus poll();                            // atomic relaxed 读,音频路不上锁
  bool snapshotCapture(size_t n, float* out, uint64_t& endIdxOut);
  bool snapshotRender (size_t n, float* out, uint64_t& endIdxOut);
  uint64_t capWritten() const; uint64_t renderWritten() const;
};
```
- 复用 WasapiCaptureStream/WasapiRenderStream（经 BackendFactory）+ 2 RingBuffer + 2 ScopeBuffer + DelayFifo。
- `renderXruns = renderRing.overruns()`（预填阶段不计）。状态字段全部 atomic；errorCode 取代 message。
- **测试 seam**：假 backend 实现 `IAudioBackend`（含 `dataReadyEvent()`），可向其 RingBuffer 注入字节并 SetEvent → pump 逻辑（预填、rate-fail、回滚、drift、scope 推进、xrun）全部无硬件可测。

> Engine/MonitorEngine 仍是并列两类（pump 语义不同，不强行共享基类）。仅把 `peakLevel` 提到 SampleConvert 复用；生命周期样板各自保留属可接受重复。

## 5. GUI（src/gui，ImPlot + DX11）
- **ImPlot vendored** 于 `third_party/implot/`（依赖 ImGui）；GUI vcxproj 加 `$(SolutionDir)third_party\implot` include 目录（Debug/Release 两处），并对 `implot.cpp`/`implot_items.cpp` 加 per-file `TurnOffAllWarnings`（同 ImGui 处理）。
- **AppUi 持有 Engine 与 MonitorEngine 两者为成员**（AppUi 拥有生命周期）；`draw()` 不再外部传 Engine&。顶层模式选择 Capture/Playback(用 Engine) | Monitor(用 MonitorEngine)，只 start 当前活动者。
- **Monitor 视图**（ImPlot，两组上下：采集 / 延迟播放，每组 3 图）：
  - 顶栏：采集设备▼ 渲染设备▼ 延迟[100]ms slider [▶开始][■停止]；状态行：cap/render state、sr、fifoFillMs、renderBufMs、drift、xrun c/r。
  - 波形：ImPlot Line，时域，x=秒（取最近~50ms 窗口）。
  - 频谱曲线：ImPlot Line，**x=对数频率**(`SetupAxisScale(Log10)`)，y=dBFS（−96..0）。
  - 声谱图：ImPlot Heatmap，**先把线性 FFT bin 重采样到对数间隔的行**再填（Heatmap 假定均匀网格；DC bin 不入 log 轴）；x=时间、y=对数频率、色=dBFS（色板固定如 viridis，范围 −96..0，历史≈5s≈469 列）。
- **分析驱动（FPS 无关，抽成可单测自由函数）**：`advanceAnalysis(written, &nextEndIdx, hop, fn)`——每渲染帧按 hop=512 推进 `nextEndIdx`（**持久化**，不每帧重置；极端落后则**快进**而非逐 hop 空转），对每个 hop 调 `snapshotLatest(2048)`→`magnitudeSpectrumDb`→(a)更新频谱曲线(b)向声谱图滚动缓冲(freqBins×historyCols)追加一列；窗口滚出(false)跳过并计数。
- 预分配：每路一份 `workBuf`(2^k)+复用 `magDbOut`，无每帧堆分配。

## 6. CLI（src/cli）
新增 `monitor` 子命令（无 GUI 冒烟）：`WinAudioCli monitor [--cap <id>] [--render <id>] [--delay-ms N] [--backend ...] [--seconds N]`，定时打印 cap/render state、fifoFillMs、drift、xrun。用于在 GUI 前验证 MonitorEngine 端到端。

## 7. 可测验收指标
- FFT n=2048、hop=512、75% 重叠；23.4375 Hz/bin、10.667ms/列、469 列/5s（自洽）。
- 频谱标定：**on-bin**(1007.81Hz) 满幅正弦 = 0 dBFS±0.1；零填充路径(窗长归一化) on-bin 满幅 = 0 dBFS±0.5。
- 分析 cadence 由 `advanceAnalysis` 自由函数驱动，单测喂假 `written` 计数断言 hop 推进/快进/滚出，与 GUI FPS 解耦。
- drift：注入 ±100ppm 偏速，低通占用稳定在 delayMs±deadband(>1 周期)，长跑(≥30 分钟)不自然 xrun。
- 支持格式 int16/int24/int32/float32，1–2 声道，采集=渲染采样率。
- 无每帧堆分配：分析循环跑 1e4 次用跟踪分配器断言首帧后 0 次分配（否则降级为 code-review 约束并注明）。

## 8. 测试矩阵
- 纯逻辑单测（无硬件）：Fft（on-bin/非2^k 标定、冲激、floor）、SampleConvert（边界/int24/clamp/平均降混）、ScopeBuffer（一致快照同 epoch、滚出 false、totalWritten 推进）、DelayFifo+drift（偏速→占用稳定、drop/dup 计数）、advanceAnalysis（hop/快进/滚出）、MonitorEngine pump（注入假 backend：预填、rate-fail、回滚、scope/xrun）。
- 硬件/集成（手动）：CLI `monitor` 冒烟（state/fifo/drift/xrun）；GUI 监听跑通，波形/频谱/声谱图实时刷新，关闭不挂（拆卸顺序），长跑观察 drift 稳定。

## 9. 实施 gate 顺序（单计划内）
1. **Core DSP + 单测**：AudioFormat 拆分 → Fft → SampleConvert → ScopeBuffer → DelayFifo（各自单测过）。
2. **IAudioBackend.dataReadyEvent() + WasapiCaptureStream pumpReadyEvent_**（不破坏现有单流路径；现有 23 测试 + Shared 冒烟回归）。
3. **MonitorEngine + 假 backend 单测**（生命周期/预填/回滚/drift/scope）。
4. **CLI monitor 冒烟**（真实设备，无 GUI）。
5. **ImPlot vendoring + vcxproj 接入 + 空编译**。
6. **AppUi 模式切换重构**（持双 engine，模式选择，无图）。
7. **波形**（ImPlot Line + snapshotLatest）。
8. **频谱曲线**（magnitudeSpectrumDb + log-X，校验 0 dBFS）。
9. **声谱图**（log 重采样 + Heatmap 滚动缓冲）——最复杂，放最后。

## 10. 已知限制（写入文档/CLAUDE.md）
- 分析降混 mono（反相立体声会低读/归零；逐声道 L/R 分析列入 Phase 4）。
- 要求采集=渲染采样率（无重采样器）。
- renderScope = 送往渲染的延迟数据，非对实际声学输出的二次采集。
- 监听音频用单样本 drop/dup+crossfade 做 drift 吸收，非录音级；启动瞬态/异常漂移下仍可能有极轻微 artifact。
