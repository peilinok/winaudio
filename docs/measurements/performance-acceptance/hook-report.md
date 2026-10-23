# Hook 关闭快速路径验收（#229）

四个生产 buffer Hook 在真实 API 返回后检查记录开关；关闭时跳过 CallPod 初始化、格式化、记录取时和提交。共享 emit 在取时前再次检查。控制调用不依赖 pump 开关，Activate／GetService 的必要安装继续执行。

## 配置与证据

机器见 [machine.json](machine.json)，Release、MSVC 19.51.36257.0、静态 CRT、同一测量夹具。旧实现来自 `2483b0cbe7c07bfaba1e9f6d576a203aa03db3ee` 的原始 Git blob。初始 x64 基线在生产改动前采集；补充 x86 对照为事后重建旧源码。计时入口编译掉全部行为观测标记。

- [原始优化前 x64 基线](hook-baseline-x64.csv)／[初次优化后数据](hook-optimized-x64.csv)。
- [x64 配对前](hook-paired-before-x64.csv)／[后](hook-paired-after-x64.csv)，[x86 配对前](hook-paired-before-x86.csv)／[后](hook-paired-after-x86.csv)。每种位数均为四入口、六状态、三次重复；每组预热 1 万、批量 30 万、单次 2 万样本，原方法调用计数 33 万。
- 真实音频三组 [前 1](hook-audio-before-1.txt)／[后 1](hook-audio-after-1.txt)、[前 2](hook-audio-before-2.txt)／[后 2](hook-audio-after-2.txt)、[前 3](hook-audio-before-3.txt)／[后 3](hook-audio-after-3.txt)。每次两个新目标，同位数 x64。
- [完整汇总与原文件 SHA256](hook-summary.json)。单次分位数和每组数值以此文件为准，不混合不同入口或阶段。

实测加载模块由 Toolhelp 模块列表核对。优化前实际加载的 `WinAudioHook-5.dll` 和预期 DLL 的 SHA256 同为 `C069D242D43DA0EE3D7C0FEACC88D41ADD7F66D026AB009AC5811368F86C4C53`；优化后同为 `5C4EE043D229E860411EE0BA233833F8A9531B9564F50AB1040CE1EDF0A06380`。这些是当时测量二进制的身份；重新构建后的文件哈希可能不同，复跑必须再次核对。

## 独立调用开销

下表只用于概览，数值为四入口、三次重复中批量均值的最小～最大值，单位 ns／调用；它不是合并分位数。两次计时之间仍恰好执行原函数一次。

| 位数 | 状态 | 优化前范围 | 优化后范围 |
| --- | --- | ---: | ---: |
| x64 | 无 ring | 104.578～186.216 | 4.435～6.041 |
| x64 | quiet | 107.224～174.439 | 4.660～6.014 |
| x64 | capture 关 | 107.119～196.432 | 4.491～5.998 |
| x64 | pump 关 | 110.475～184.566 | 4.288～6.516 |
| x64 | 全开 | 141.452～231.933 | 135.776～211.265 |
| x86 | 无 ring | 269.190～605.184 | 5.145～9.974 |
| x86 | quiet | 279.226～820.908 | 4.931～9.964 |
| x86 | capture 关 | 282.751～1007.603 | 5.161～11.906 |
| x86 | pump 关 | 286.676～774.188 | 5.334～10.109 |
| x86 | 全开 | 310.546～638.132 | 290.169～563.251 |

关闭状态的每个配对都体现消重。全开 x64 render GetBuffer 的批量均值在第 1／3 组略升、第 2 组下降，三组单次 p95／p99 均未上升；其余入口也没有三组一致回退。不能将这些小差异解释成全开路径确定提速。

本机 QPC 为 10 MHz，单次分布的量化步长为 100 ns；关闭后的单次 p50 常为 0，p95／p99 为 100 ns。这不代表零成本。批量均值与确定性的零准备计数共同支持关闭消重，QPC 自身采样开销在原始文件单列，不强减。

## 真实音频

两个自有目标各执行静音 render＋真实 endpoint loopback，48 kHz、2 声道、32 位。每阶段约 8 秒；覆盖未 Attach、capture 关、pump 关、pump 开、Detach、再次 Attach、多 PID 只 Detach 一个。目标以 Sleep(5) 请求节拍，实际回调受 Windows 调度影响。

六次运行均 `result=PASS`，七阶段均持续取得捕获帧、调用失败数为零、样本溢出为零、静音非零字节数为零。每个目标只在未 Attach 的启动阶段报告一次 discontinuity，后续阶段为零；没有新增的阶段性中断。pump 开和再次 Attach 时两种 buffer 接口均出现在近期窗口，关闭稳定后对应写索引无增长；控制接收开／pump 关时控制调用仍增长。

按两个目标共六个配对比较：

- capture 关：回调 p95 六组均下降；p99 从 83.2～121.1 μs 变为 66.6～74.9 μs。
- pump 关：回调 p95 六组均下降；p99 从 81.6～115.0 μs 变为 68.9～84.0 μs。
- pump 开：回调 p95 六组均下降；p99 有一组 78.5→79.9 μs，其余下降。
- 再次 Attach 的第 2 组目标 0：p99 113.4→202.1 μs；Detach 的同组目标 0：89.0→210.2 μs。第 1／3 组没有重复该尖峰，不能删掉这些样本，也不能据此认定是新增门控导致。这里只得出在已采三组中未见可重复回退。

表中“回调耗时”实际是目标音频处理循环主体：从 GetCurrentPadding 前到 capture 缓冲排空后，包括真实 buffer API 与静音数据检查；不包含随后低频 GetBufferSize／GetService 和 Sleep。它并非事件回调的完整耗时，也不是单个 Hook 耗时。音频结果仅覆盖登记设备的静音信号，未证明所有设备、负载或非静音质量，也不证明严格排空或整个 IPC 发布协议。

## 确定性和生成代码检查

`WinAudioHookTests` 直接包含生产翻译单元，排除 DllMain 自动安装；八项测试覆盖实际四入口及共享 emit 的门控、原方法参数／输出／HRESULT、成功／失败／S_FALSE、无原方法、空输出、xrun 一次计数、在途边界和必要服务安装。开关切换测试使用同步回调标记，不靠 Sleep 猜时序。

双本地映射测试通过一份映射的两个不同地址访问，控制视图用 InterlockedExchange，生产 Hook 从另一视图读取门控并提交，验证关闭与重新开启可见、原方法仍恰好一次。它没有扩大为 IPC 发布协议正确性证明。

布局在 x64／x86 相同：CallPod 1040 字节，Ring 2197032 字节，capture／pump 字段各 4 字节，偏移分别 36／16，保持布局 5。生产头文件未修改。

[x64 指令摘录](hook-codegen-x64.txt) 与 [x86 指令摘录](hook-codegen-x86.txt) 来自独立 `/O2 /FAs` 编译，原始完整列表哈希附在文件首行。两个位数均以 DWORD `mov` 读取对齐的 quiet／capture／pump，初检跳转位于 CallPod 清零及格式化前，终检跳转位于 GetSystemTimeAsFileTime 前。读取未引入锁指令或等待；原提交计数仍用既有 interlocked 指令。函数栈空间与安全 cookie 仍会建立，零准备不意味着零函数开销。该检查使用非 `/GL` 的可读对象文件；性能数据来自普通 Release 的整程序优化构建，二者不混称同一二进制。

## 复跑与停止

构建、原始采样和汇总命令见 [README](README.md)。代码生成复跑：

```powershell
& scripts/performance/inspect-hook-codegen.ps1 -Architecture x64 -Generator 'Visual Studio 18 2026'
& scripts/performance/inspect-hook-codegen.ps1 -Architecture Win32 -Generator 'Visual Studio 18 2026'
py -3 -B scripts/performance/summarize_hooks.py docs/measurements/performance-acceptance --output docs/measurements/performance-acceptance/hook-summary.json
```

行为一致、稳定关闭零记录准备、配对体现消重，已采开启／真实音频无可重复回退后停止本票扩展。没有套用 GUI 0.5 ms 门槛或增加纳秒硬上限。低频控制调用提前格式化、延迟格式化、IPC 重写、严格排空和全局注入仍延期。完整 Debug／Release 测试与两轴评审结果记录在实施记录；尚未运行远端 CI。
