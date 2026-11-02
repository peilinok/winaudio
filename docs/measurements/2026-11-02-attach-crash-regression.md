# #243：运行中附加崩溃修复与回归证据

## 结论和范围

2026-11-02，在 `E:\WORK\vibecoding\winaudio`、基线 `f06e28557de07447249494656ce9914c07592a12` 上保留并验证六槽最小修复。前置 #253、#254 已合入；本票复验它们的资源寿命、不同实现转发和安装失败行为，没有重复实现。

x64 Debug／Release 直接相关回归各 **55/55**；x86 两配置无设备 Hook 回归各 **23/23**，包含本票六项 SDK 服务用例。增强后的自有播放探针在两配置、附加／未附加下共 **16 个独立目标正常退出 0**，四个 API 的结果和记录正确。两配置双目标七阶段音频回路均 PASS。

这证明已确认的音量／静音槽位缺陷在受控目标中修复，不证明未提供的用户应用所有崩溃原因都已解决。真实效果硬件的 Hook 行为、x86 实际注入和新逐方法监控功能未在本票验收。

## 实现边界

- `src/hook/WinAudioHook.cpp`：只修正 `ISimpleAudioVolume` 的 getter／setter 槽位为 4／3／6／5，`IAudioEffectsManager` 查询／设置为 5／6；通知注册／取消 3／4 不改。没有新增热路径分配或锁。
- `src/tests/test_hook_services.cpp` 及测试 CMake：复用既有六项 SDK 声明驱动的生产 Hook 回归。检查参数、输出、返回值、调用次数、记录方法、通知不被误拦截和关闭记录仍转发；不手写一套待验证的接口顺序。
- `src/probe/AttachVolumeProbe.cpp` 及探针 CMake：复用并增强自有真实播放目标。每项创建独立 session，附加前设置音量 0.625、静音 FALSE；目标通过共享结果报告真实 HRESULT 和输出，setter 后读回；父进程在目标退出后验证结果及一条匹配的原调用记录。放行前按内容身份核验实际加载 DLL，输出模块路径与真实子进程退出码。超时仅清理探针自己创建的子进程，确认退出前不读共享结果。
- 既有文档、研究、图像和其他未提交工作保留；未修改 `AGENTS.md`、CI、音频引擎、图、WAV 或无关测试。

## 保留的旧红灯证据

没有切换回旧产品代码、覆盖用户工作或重新制造客户应用崩溃。以下原始证据保留在原位置；哈希收入本次 `artifact-manifest.json`。

| 证据 | 内容 | SHA256 |
| --- | --- | --- |
| `.scratch/attach-volume-cdb-confirmed.log` | `HookMuteGet+0x65` 读取地址 `0x1`，访问异常 `0xC0000005` | `8B6E3457E747A6C0388B00C3B1DEB29C3F833C0FEBA536061F8B3A581611A87C` |
| `.scratch/attach-slot-fix/red-sdk-tests.txt` | 两项 SDK 用例失败：SetMute 被记为 GetMute；效果通知误入记录 | `349BCABF61ABD59DB690275A30DD6A923E36B921EDB3BC4B8581008A5452CF97` |
| `.scratch/attach-slot-fix/before-volume-probe.txt` | 原自有 SetMute 目标退出 `0xC0000005` | 见身份清单 |

原调查说明见 `docs/research/2026-10-27-attach-crash-startup-multiprocess.md`。旧 76/76 和 14/14 不用作本次新构建的通过数字。

## 环境、命令和自动回归

系统为 Windows 11 专业版，版本 `10.0.26200`。本机 `build`／`build-x86` 缓存使用 `Visual Studio 18 2026`，MSBuild `18.10.1-1.26427.6`；沿用缓存，没有改用 VS2022 preset。相关增量构建通过，最终探针构建零警告；最初增强探针产生的两条编译警告已修正，初次日志仍保留，不将它们写成最终警告。

```powershell
cmake --build build --config Debug --target WinAudioHookTests WinAudioTests AttachVolumeProbe HookAudioProbe AttachSmoke -j 6
cmake --build build --config Release --target WinAudioHookTests WinAudioTests AttachVolumeProbe HookAudioProbe AttachSmoke -j 6
cmake --build build-x86 --config Debug --target WinAudioHookTests WinAudioHook -j 6
cmake --build build-x86 --config Release --target WinAudioHookTests WinAudioHook -j 6
ctest --test-dir build -C Debug -R '^(Hook|VtablePatch|OnDemandAttach|RemoteThreadWait|RemoteInjectionTest|AttachLifecycleTest|PipelineAttachSet)' --output-on-failure
ctest --test-dir build -C Release -R '^(Hook|VtablePatch|OnDemandAttach|RemoteThreadWait|RemoteInjectionTest|AttachLifecycleTest|PipelineAttachSet)' --output-on-failure
ctest --test-dir build-x86 -C Debug -R '^(HookBuffer|HookServices|HookForwarding)' --output-on-failure
ctest --test-dir build-x86 -C Release -R '^(HookBuffer|HookServices|HookForwarding)' --output-on-failure
```

上述 x64 相关构建通过 `WinAudioTests` 依赖构建正式 Hook DLL。两项恢复页保护失败的 Hook 回归由 CTest 分别启动独立进程，未把会保留失败状态的测试合在一个 gtest 进程内运行。

| 场景 | Debug | Release |
| --- | --- | --- |
| 修改探针前 x64 直接相关基线 | 55/55 | — |
| x64 直接相关最终回归 | 55/55 | 55/55 |
| x86 Hook 无设备 ABI／转发回归 | 23/23 | 23/23 |
| 超时／取消后未读参数保活、迟到完成、目标退出、控制器销毁 | 纳入上述 55 项通过 | 同左 |
| 不兼容常驻 DLL、运行期安装故障要求重启、失败安装与不同实现路由 | 纳入上述 55 项通过 | 同左 |
| 关闭记录保留原调用和必要服务发现 | HookBuffer／HookServices 通过 | 同左 |

原始输出均位于 `.scratch/implement-pipeline-2026-11-02/243/`：`build-*.txt`、`tests-baseline-x64-debug.txt`、`tests-x64-{debug,release}.txt`、`tests-x86-{debug,release}.txt`。

## 四 API 的真实目标对照

每配置执行以下命令。父进程退出码见 `volume-{debug,release}-exits.json`，均为 0；完整目标命令行、PID、DLL、wait 和退出码见同名前缀的四份文本。

```powershell
.\build\bin\Debug\AttachVolumeProbe.exe --no-attach
.\build\bin\Debug\AttachVolumeProbe.exe
.\build\bin\Release\AttachVolumeProbe.exe --no-attach
.\build\bin\Release\AttachVolumeProbe.exe
```

| API | Debug 未附加／附加 PID | Release 未附加／附加 PID | 目标结果／记录参数 |
| --- | --- | --- | --- |
| GetMasterVolume | 32084／29564 | 15596／32236 | 输出 0.625；`level=0.625` |
| SetMasterVolume(0.5) | 32192／8320 | 32520／31728 | 读回 0.500；`level=0.500` |
| GetMute | 26456／24856 | 29680／31440 | 输出 FALSE；`false` |
| SetMute(TRUE) | 31368／14056 | 24492／32120 | 读回 TRUE；`true` |

16 个目标均 `wait=0 exit=0x00000000`，原调用和适用的读回均 `S_OK`，`behavior_match=1`。8 个附加目标各有 `matching_calls=1`，同时核对接口、方法、HRESULT 和参数；setter 的验证性 getter 另有记录，不冒充原 setter。未附加目标的调用记录数为 0。非空事件 GUID、`S_FALSE` 和失败返回的透明传递由无设备 SDK 用例覆盖。

放行前通过 `inspectHookModules` 核对内容身份，并枚举目标实际加载路径；模块名中的哈希与源 DLL 和磁盘暂存副本 SHA256 相等：

| 配置 | 实际已载文件（位于对应 `build\bin\<配置>\`）／SHA256 |
| --- | --- |
| Debug | `WinAudioHook-6-9483862d9d3add6ae5a94e75a32e4d013ce2385ced82d49c71bd0324c8104c81.dll` |
| Release | `WinAudioHook-6-7ed3377ae99b8d35e0e0b4f030572e30919df3e659d85c21f6de6fb406bbdf7b.dll` |

x86 构建 DLL SHA256 分别为 `D5A0E5F6A0644B3E4DCE759AD608C69D2D5CDE8A26D970C0E9D572FA1EE018C6`（Debug）、`EB4B29F5F1F5B09C43D809CEC19A166776D4885BFCCD63D2EF134A4FE9F3FA13`（Release）；它们未做实际注入。

## 既有双目标运行中附加回路

分别运行 `.\build\bin\Debug\HookAudioProbe.exe` 与 `.\build\bin\Release\HookAudioProbe.exe`，退出码均 0，`result=PASS`。完整日志为 `audio-{debug,release}.txt` 及退出文件。

两目标各有 silent render 和真实端点 loopback，设备 `{0.0.0.00000000}.{46f4256f-bc60-4865-b5f3-4c6eef7ee966}`，48000 Hz、2 声道、32 bit、格式标记 `0xFFFE`。Debug PID 5824／29800，Release PID 31416／24896；实际加载模块与上表一致。

七阶段依次为 no-attach、capture-off、pump-off、pump-on、detached、reattached、other-pid-detached。关闭 capture 后稳定区间控制／pump 索引增量均为 0；pump-off 仍保留控制记录而 pump 为 0；开启后两类 buffer 方法均出现；全部分离后再附加有效；只分离一目标后另一目标仍接收。每目标每阶段均有回调和采集帧，失败与计时储存溢出为 0，启动阶段各出现一次不连续，后续阶段为 0。

该既有探针确认子进程终止句柄有信号，但未读取其退出码，因此不把它当作独立的目标退出码证明；音量探针的真实退出码和 RemoteInjection 进程测试另行提供该项证据。此处不是逐方法新配置、保留历史或正式性能／音质验收。

## 额外全量尝试的失败及复验

额外全量曾将两配置各 `ctest --output-on-failure -j 6` 同时运行；两配置均 692/696，退出码 8。该方式使共享固定 WAV 路径、硬件状态及计时负载互相影响，不称为全量通过。

- Debug：`WavSink.ExactPathKeepsCallerName`、`MonitorEngine.SlowOpenCancelledBySourceEndCannotFollowReengage`、`CaptureTrackList.OptionalOpenWriteAndCloseFailuresKeepBothTracksAndTapsRunning`、`AudioEffects.SetHardwareSkipsIfUnavailable`。
- Release：`WavSink.ExactPathRoundTrip16BitStereo`、`WaveEnvelope.EightChannelFullHistoryTiming`、`WavLifecycle.CaptureTrackList.DestroyAllAndDestructorFinalizeEveryAcceptedByte`、同一 AudioEffects 用例。

保留 `full-{debug,release}.txt`。按配置顺序 `-j 1` 重跑原失败项：两个配置各自前三项全部通过（Debug：`full-debug-failed-serial.txt`；Release：显式全名选取的 `full-release-other-failed-serial.txt`）。`--rerun-failed` 列表在同一 build 的配置间共享，故 Release 的首次重跑文件只包含 AudioEffects，不拿它证明原其余三项。

AudioEffects 两配置单进程仍在 `test_audio_effects.cpp:193` 失败：查询到状态 Off，预期切换状态 On。既有 `docs/measurements/performance-acceptance/implementation-log.md:154,251,285` 已记录相同硬件失败和起点程序复现；当前该生产／测试源码相对 main 无差异。测试执行其原有 best-effort 恢复。这不是本票新引入或已修复的问题，不修改断言或硬件实现来补绿色数字。

## 证据边界与交接

提交前仅补充 `Module32NextW` 返回日志：保留 BOOL／原错误码，并通过 `wa::log::hrName(HRESULT_FROM_WIN32(error))` 给出符号名称，没有改变控制流。随后两配置探针增量构建均零警告，各执行一次 `AttachVolumeProbe.exe --method 3`：Debug 子 PID 30504、Release 子 PID 29992 均真实退出 0，`S_OK`、静音读回 TRUE、`behavior_match=1 matching_calls=1`，加载 DLL 身份仍与上表一致。此次仅重跑这两个 SetMute 目标，没有重复四 API 对照或完整回归；前述 16 目标属于日志补充前的行为验证。新增输出为 `build-final-log-{debug,release}.txt`、`volume-final-log-{debug,release}-mute.txt` 及相应退出文件；清单已更新最终探针源码和二进制哈希。

本票交付文件仅为上述五个代码／CMake 文件和本报告。`artifact-manifest.json` 保存源码、目标、源 DLL、实际已载暂存 DLL及旧红灯证据的路径／大小／SHA256；`preserved-original-files.json` 核对本票之外原工作未变。没有 commit、push 或合并；独立干净审查、PR、CI 和 squash 由主代理按已授权流程执行。

真实效果硬件 Hook、x86 真实注入、未知用户应用、GUI 人工验收及整个父规格的新功能／性能门槛未覆盖。旧 Hook 常驻目标必须重启；本票没有热卸载承诺。已有渲染／采集调用通过只限已发现且可安装的实现，不外推全部 COM 对象。
