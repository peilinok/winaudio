# WASAPI Loopback 多声道与 WAV 格式研究

**日期：** 2026-09-17  
**性质：** API 事实与实现建议，不是已批准的产品规格，也不代表已完成代码修改。

## 结论

1. Process/Application Loopback 返回的 `IAudioClient` 来自
   `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`，不是某个 G431 render endpoint。
   当前机器上对它调用 `GetMixFormat()` 返回 `E_NOTIMPL`，说明这个虚拟客户端的该方法
   在当前系统实现中不可用。Microsoft 的 `GetMixFormat` 文档没有把 `E_NOTIMPL`
   列为 Process Loopback 的特定契约，因此不能把这个实测结果扩大为所有 Windows
   版本都必然如此。
2. 换成 `IAudioClient2` 或 `IAudioClient3` 不能解决这个问题。二者继承
   `IAudioClient::GetMixFormat`；`IAudioClient2` 新增的是 offload 和 client properties，
   `IAudioClient3` 新增的是共享引擎周期性查询/初始化，不是另一个 mix-format
   查询实现。
3. Process Loopback 应允许用户显式给出捕获格式，并按 Microsoft 官方样例的方式用
   `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` 初始化。当前仓库在未指定格式且
   `GetMixFormat` 失败时回退 `44100/16/2`，与官方样例的固定格式一致，但它只是
   兼容默认值，不会自动发现 G431 的 8 声道格式。
4. System Loopback 绑定具体 render endpoint，可以正常查询该 endpoint 的 Mix、
   Device、OEM 格式，也可以将同一套设备能力和自定义格式控件用于 System Loopback。
   这部分当前 GUI 已经实现：System Loopback 会加载 render endpoint capabilities；
   Application Loopback 只有 System default 和 Custom，因为它不绑定具体 endpoint。
5. 多于 2 声道的 WAV 应写 `WAVE_FORMAT_EXTENSIBLE`。当前 `WavWriter` 固定写
   16 字节 `fmt ` chunk，只记录声道数，不能可靠保存 7.1 speaker mapping。

## 1. Process Loopback 的 `GetMixFormat`

### 1.1 官方明确的范围

Microsoft 对 `IAudioClient::GetMixFormat` 的定义是：

> retrieves the stream format that the audio engine uses for its internal
> processing of shared-mode streams.

同一文档还说明：对一个 **audio endpoint device** 建立 shared-mode stream 时，
`Initialize` 总会接受从同一设备 `GetMixFormat` 得到的格式；成功时返回
`WAVEFORMATEXTENSIBLE`，失败时 `*ppDeviceFormat` 为 `NULL`。

Process Loopback 则通过
`ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, ...)` 激活。
官方样例 README 明确说它“不绑定到具体 audio endpoint”，因此不存在一个可直接等同
于 G431 的 endpoint mix format。当前系统返回 `E_NOTIMPL` 与这个架构一致，但这是
结合实测作出的解释，不是 `GetMixFormat` 文档对 Process Loopback 的专门保证。

### 1.2 官方样例如何处理格式

Microsoft 的 `ApplicationLoopback` 样例仍使用 `IAudioClient`。样例源码注释写道：

> The app can also call m_AudioClient->GetMixFormat instead to get the capture
> format.

但实际代码没有依赖它，而是构造 `44100 Hz / 16-bit PCM / 2 channels` 的
`WAVEFORMATEX`，然后调用：

```text
Initialize(
  AUDCLNT_SHAREMODE_SHARED,
  AUDCLNT_STREAMFLAGS_LOOPBACK |
  AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
  ...,
  &m_CaptureFormat,
  nullptr)
```

这给出的可靠兼容策略是：优先接受用户指定格式；如果产品需要一个无需选择即可工作的
默认值，可以使用已验证的保守格式；不要把升级 COM interface 当成格式发现方案。

### 1.3 为什么 `IAudioClient2/3` 不改变结果

- `IAudioClient2` 的新增方法只有 `GetBufferSizeLimits`、`IsOffloadCapable`、
  `SetClientProperties`。
- `IAudioClient3` 的新增方法用于查询/选择 shared-mode engine period：
  `GetCurrentSharedModeEnginePeriod`、`GetSharedModeEnginePeriod`、
  `InitializeSharedAudioStream`。
- 两者的 `GetMixFormat` 都是继承自 `IAudioClient` 的同一个方法。对同一个已激活的
  Process Loopback 对象做 `QueryInterface`，不会把它变成真实 endpoint client。

因此，可以继续只以 `IAudioClient` 完成 Process Loopback capture。只有确实需要
`SetClientProperties` 或共享周期控制时，才分别查询 `IAudioClient2/3`。

## 2. Application 与 System Loopback 的格式策略

### 2.1 Application Loopback

- 与物理 endpoint 解耦，同一 process tree 的音频可能来自多个 endpoint。
- System default 不能可靠解释为“当前 G431 的 mix format”。当前实现的 System
  default 实际是：先调用 Process Loopback client 的 `GetMixFormat`，失败后回退
  `44100/16/2`。
- Custom 格式由 `Initialize` 最终验证。仓库在指定格式时加入
  `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`。
  Microsoft 对 `AUTOCONVERTPCM` 的定义是按需插入 channel matrixer 和 sample-rate
  converter，将应用格式与 audio-engine mix format 相互转换。
- 因而显式请求 8 声道并成功，只能证明 Process Loopback 输出 8 声道交错 buffer；
  是否每个通道有独立有效内容，还取决于目标应用实际渲染的 channel layout 以及
  Windows 的矩阵转换。

### 2.2 System Loopback

Microsoft 对传统 System Loopback 的要求是：取得一个 render endpoint 的
`IMMDevice`，以 Shared mode 和 `AUDCLNT_STREAMFLAGS_LOOPBACK` 初始化，然后从该
render object 取得 `IAudioCaptureClient`。Loopback 只支持 Shared mode。

因此 System Loopback 可以而且应复用 endpoint 格式能力：

- 默认：使用该 render endpoint 的 `GetMixFormat()`。
- 自定义：用 `IsFormatSupported(SHARED)` 展示候选，启动时仍以 `Initialize` 的实际
  成败为准。
- `AUTOCONVERTPCM` 开启时，可以请求不同的 PCM sample representation、采样率或
  声道布局；这表示引擎转换后的 capture stream，不等于设备原生格式。

当前仓库 `WasapiSystemLoopbackCaptureStream` 已经走普通 render endpoint 的
`streamInitShared`，System Loopback GUI 也已调用 `queryCapabilities(Render, id)`，
所以这个优化不是待做设计，而是现有行为。Application Loopback 页没有 endpoint
选择，故不应直接展示某个 G431 的 Mix/Device/OEM 作为其固有能力。

### 2.3 Mix / Device / OEM 的查询与修改

当前仓库使用以下方法：

| 名称 | 查询来源 | 含义 |
|---|---|---|
| Mix | `IAudioClient::GetMixFormat()` | audio engine 内部 shared-mode mix format |
| Device | `PKEY_AudioEngine_DeviceFormat` | 用户为 endpoint 选择的 shared-mode device format |
| OEM | `PKEY_AudioEngine_OEMFormat` | 驱动/OEM 提供的原始默认 property；诊断参考，不是当前运行格式 |

三者的代码入口是 `DeviceEnumerator::queryCapabilities`；CLI 可运行：

```powershell
.\build\bin\Debug\WinAudioCli.exe caps --render --device <endpoint-id>
```

GUI 普通 endpoint 页面通过 `Device caps...` 查看三种来源与候选矩阵；System
Loopback 的 create recipe 已使用同一 render endpoint 能力生成格式下拉列表。

用户能通过 Windows 控制面板修改的是 Device/Default Format：

```text
control mmsys.cpl
  -> Playback
  -> 选择 G431
  -> Properties
  -> Advanced
  -> Default Format
```

Microsoft 文档说明这个选择写入 `PKEY_AudioEngine_DeviceFormat`。Mix 通常会随它
保持相同的采样率和声道数，但 sample representation 可能是 engine 使用的 float；
OEM property 不应由本工具修改。

## 3. WAV writer 的兼容性方案

### 3.1 当前问题

`WavWriter::open` 当前总是写 16 字节 PCM/IEEE-float `fmt ` chunk：

- PCM 使用 tag `1`，float 使用 tag `3`；
- 只写 `nChannels`，不写 `dwChannelMask`；
- header offset 固定为 RIFF size `4` 和 data size `40`；
- `dataBytes_` 是 `uint32_t`，只能表达经典 RIFF/WAVE 的 4 GiB 内大小。

对于 8 声道文件，样本数据仍能按帧原样保存，但读取方不知道 8 个声道分别对应
FL/FR/FC/LFE/BL/BR/SL/SR，兼容行为取决于播放器猜测。

### 3.2 最小、可验证的改法

第一阶段只支持仓库已有的未压缩 PCM 和 IEEE float，不扩展到压缩编码：

1. 写入前把 `AudioFormat` 转成完整格式描述，不能只保留
   sample rate/bits/channels/isFloat。至少还要有 `validBitsPerSample` 和
   `channelMask`；如果 capture client 给出了 `WAVEFORMATEXTENSIBLE`，应保留其
   原始 mask，而不是仅按声道数猜测。
2. Mono/stereo 且没有额外 layout/valid-bits 信息时，可继续写 16 字节传统
   `WAVEFORMATEX`，保持旧文件兼容。
3. `channels > 2`、valid bits 与 container bits 不同，或必须保留 speaker layout
   时，写 40 字节 `fmt ` chunk：
   - `wFormatTag = WAVE_FORMAT_EXTENSIBLE (0xFFFE)`
   - `cbSize = 22`
   - `Samples.wValidBitsPerSample`
   - `dwChannelMask`
   - `SubFormat = KSDATAFORMAT_SUBTYPE_PCM` 或
     `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT`
4. 8-channel 7.1 常见 mask 是
   `FL|FR|FC|LFE|BL|BR|SL|SR = 0x63F`。交错样本的通道顺序必须按 mask bit 的
   递增顺序排列。
5. 保存 `riffSizeOffset`、`dataSizeOffset` 和实际 header size，不再硬编码 36/40；
   所有 `fwrite`、`fseek`、`fflush`、`fclose` 都要检查错误，并只按完整 block
   写入，拒绝不能被 `nBlockAlign` 整除的数据。
6. 对奇数字节 chunk 写 pad byte，size 字段不包含 pad。未知 chunk 的 reader
   也应继续按 word alignment 跳过。
7. 经典 RIFF 达到 4 GiB 前应返回明确错误并停止写入；如果需求确实包含长时间、
   高采样率 8 声道录制，再单独设计 RF64，不要让 32-bit size 静默回绕。

### 3.3 建议测试

- 1ch/2ch PCM 与 float：保持旧 16-byte `fmt ` header。
- 6ch/8ch PCM 与 float：`fmt ` size=40、tag=`0xFFFE`、`cbSize=22`、SubFormat 正确。
- 8ch 7.1：mask=`0x63F`，写入已知每通道常量，读取后验证 channel order。
- `validBits=24/containerBits=32`：两个字段分别保持。
- 任意 data 长度：RIFF/data size 与 pad 规则正确。
- 接近 4 GiB：可测试的 size-policy helper 拒绝溢出，不需要真正创建大文件。
- reader 能读传统 PCM/float、Extensible PCM/float 和包含未知 chunk 的文件；对
  截断 `fmt `、无效 `cbSize`、未知 SubFormat、mask bit 数与 channels 不一致给出
  明确错误。

## 官方资料

- [Application loopback audio capture sample](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)
- [ApplicationLoopback official source](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/ApplicationLoopback/cpp)
- [IAudioClient](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioclient)
- [IAudioClient::GetMixFormat](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-getmixformat)
- [IAudioClient2](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioclient2)
- [IAudioClient3](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioclient3)
- [Loopback Recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
- [IAudioClient::IsFormatSupported](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-isformatsupported)
- [Device Formats](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-formats)
- [AUDCLNT_STREAMFLAGS constants](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audclnt-streamflags-xxx-constants)
- [WAVEFORMATEXTENSIBLE](https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatextensible)
- [Multiple Channel Audio Data and WAVE Files](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/design/dn653308(v=vs.85))
