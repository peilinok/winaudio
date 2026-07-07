# WinAudio 详细接口调用日志设计

**日期**: 2026-07-06
**状态**: 待实现（brainstorming 定稿）

## 目标

为 WinAudio 增加**详细的接口调用日志**能力,让用户能通过日志看到一切接口调用(尤其 core 内部的 WASAPI/COM/Win32 API 调用)的**参数与返回值(HRESULT)**,并统一优化日志输出格式。核心理念 **"默认安静,一键详细"**:平时只记生命周期关键事件(info),需要排障时把级别切到 debug 即可看到所有控制路径接口调用的完整参数+返回值,再切到 trace 可逐帧审计音频热路径——而这一切**不破坏被测音频的实时性**。

日志实现基于 **spdlog**(以 git submodule 固定版本引入),不自建日志后端。

## 背景与现状痛点

现状扫描结论(见附录 A 的调用点地图):

- `src/core` 是一个**刻意"从不打印"的纯库**(`Result.h:7` 契约),约 **68 处** WASAPI/COM 调用点全部只把结果压进 `Result` 往上抛,其中 **~15 处连返回值都被直接忽略**(`GetBufferSize`/`GetDevicePeriod`/`Stop`/`ReleaseBuffer` 等)。
- **没有 `hrToString`**:HRESULT 现在只显示成 `0x88890004` 裸十六进制,无可读符号名/描述(全代码库无 `FormatMessage`)。
- **没有 `AudioFormat::toString()`**:格式参数在 4+ 处被手写重复格式化。
- **没有任何日志级别/开关**。GUI 日志面板 `logLines_` 只有 3 处写入(`invalid format` / `monitor started` / `monitor stopped`)、只增不删、无时间戳;CLI 全靠 `printf`。

即"想看接口调用细节根本无从看起"。本设计基于 spdlog 搭一套薄封装 + 全面插桩。

## 为什么用 spdlog(选型依据)

- **原生 async logger**:内置无锁 mpmc 队列 + 后台线程池。设 `overrun_oldest` 溢出策略后,日志调用**非阻塞**(队满丢最旧,不等待),音频线程可安全调用,不引发 xrun——直接替代自建的双路径前端队列。
- **原生 `trace` 级别**:`trace/debug/info/warn/err/critical`,与本设计级别一一对应(log4cpp 无 trace)。
- **现代 C++ / CMake 原生 / MSVC 友好**:`add_subdirectory` 即集成,可继承本项目 `CMAKE_MSVC_RUNTIME_LIBRARY`(`/MT`);作为 SYSTEM include 抑制第三方 `/W4` 告警。
- **sink 生态**:`rotating_file_sink`(文件大小轮转,免自写)、自定义 sink 只需继承 `spdlog::sinks::base_sink<Mutex>`(GUI 面板用)。
- **格式**:`set_pattern` 定制前缀 + fmt 格式化 payload。

## 架构总览

在 `src/core` 新增一层**基于 spdlog 的薄封装 `wa::log`**。它保留 `Result.h` "never prints" 的**精神**:core 不直接选择输出目的地,由上层通过 `wa::log` 注册 sink;未初始化/未加 sink 时不产生输出。`Result` 语义完全不变——日志是旁路的观测通道,不替代返回值。

- **单一 async logger**(spdlog),挂多个 sink;**溢出策略 `overrun_oldest`(非阻塞)**。
- **控制路径**(枚举/格式协商/`Initialize`/`Start`/`open`,低频):经 `wa::log` 同步入队 async logger。
- **热路径**(音频线程 `GetBuffer`/`ReleaseBuffer`/`GetCurrentPadding`,trace 级):同样调 async logger,但因 `overrun_oldest` + 调用前 `should_log(trace)` 短路,**关 trace 时零开销、开 trace 时非阻塞无堆分配**(spdlog 用 stack buffer 格式化,payload < 250B 不堆分配)。后台线程池做实际 sink 写。
- **不再自建 `RingBuffer` 日志队列**:spdlog async 队列即前端。

## 范围

### 目标(本次交付)

- `third_party/spdlog` 作为 git submodule(固定版本);CMake 集成、`/MT` 继承、SYSTEM include。
- core `wa::log` 封装:init async logger(`overrun_oldest`)、级别 get/set、sink 注册、`emit`/`emitTrace` 转发、`hrName`。
- 三个 sink:`rotating_file_sink`(文件)、stderr sink(CLI)、自定义 GUI 面板 sink(继承 `base_sink`)。
- `AudioFormat::toString()`。
- 在 core 全部控制路径 WASAPI/COM 调用点插桩(debug)、音频热路径插桩(trace)、生命周期事件(info)、失败与被忽略返回值(err/warn)。
- GUI:升级日志面板 + 级别下拉 + 默认写 `winaudio.log`。
- CLI:`--log-level` / `--log-file` + 日志走 stderr。

### 非目标(YAGNI)

- 不自建日志后端/队列(交给 spdlog);不再考虑 log4cpp。
- 不做**每 sink 独立级别**——全局单一 logger 级别,运行时可切。
- 不做结构化 JSON 日志(已选对齐分栏文本格式)。
- 不做日志的网络/远程传输、不做日志压缩。
- 不改变现有 CLI stdout 行为(状态行、`caps` 表、设备列表照旧);日志是新增的 stderr 通道。

## 日志级别与覆盖映射

直接用 spdlog `level::{trace,debug,info,warn,err,critical}`,全局 logger 级别 **默认 `info`**。级别 X 生效时输出 ≥X 严重度。

| 级别 | 记什么 | 典型调用点 |
|---|---|---|
| **err** | 调用失败(`FAILED(hr)`)、`Result::Fail` 的产生点 | `Initialize`/`Activate`/`Start`/`GetService` 失败 |
| **warn** | 可容忍的异常与**现状被忽略的返回值** | `IsFormatSupported` 返回不支持、xrun 发生、`Stop`/`ReleaseBuffer`/`GetBufferSize`/`GetDevicePeriod` 非 S_OK、async 队列丢弃计数 |
| **info** | 生命周期关键事件(普通用户看得懂的主线) | open/close、start/stop、选中设备(名字+id)、最终协商格式、后端模式 |
| **debug** | **所有控制路径 WASAPI/COM 调用**的参数 + 返回值 | 附录 A 中 ~50 处非热路径调用点(枚举、`GetMixFormat`、`IsFormatSupported`、`Initialize` 各分支、`GetService`、`SetClientProperties`、`SetEventHandle`、`GetBufferSize`…) |
| **trace** | 音频线程热路径逐帧 | `GetBuffer`/`ReleaseBuffer`/`GetCurrentPadding`/`GetNextPacketSize`/`WaitForSingleObject`(capture/render 循环) |

**默认 info** = 面板/文件开箱只有关键事件,安静;把级别切到 debug 立刻看到一切接口调用参数+返回值(核心诉求);trace 再逐帧。

## 日志行格式(对齐分栏)

目标呈现(缩进即代码块):

    12:34:56.789 INFO  [pump] WasapiStream::Initialize   args: mode=exclusive fmt=48000/16/2 buf=10.0ms flags=0x60000   ret: S_OK
    12:34:56.792 ERROR [main] DeviceEnum::GetMixFormat                                                                   ret: 0x88890004 AUDCLNT_E_DEVICE_INVALIDATED
    12:34:57.010 TRACE [capW] IAudioCaptureClient::GetBuffer                                                             ret: frames=480 flags=0x0

- 前缀(时间戳/级别/线程)交给 spdlog `set_pattern`,例如 `%H:%M:%S.%e %^%-5l%$ [%t] %v`(`%e`=毫秒、`%-5l`=定宽级别、`%t`=线程、`%v`=payload;`%^%$` 让 GUI/console sink 上色)。
- **线程标签**:spdlog `%t` 默认是数字线程 id;本设计给每线程注册**短名**(`main`/`pump`/`capW`/`renW`/`enum`),用自定义 flag 或在 payload 内呈现。各线程入口设置一次(漏设显示 `?`)。
- **payload(`%v`)= `Module::Call` + 左对齐填充 + `args: …` + `ret: …`**,由调用点用 `AudioFormat::toString()` 等拼好(对齐由 payload 内固定列宽处理)。
- **ret**:HRESULT 经 `hrName` → `S_OK` 或 `0x88890004 AUDCLNT_E_DEVICE_INVALIDATED`;输出型参数(如 `GetBuffer` 的 frames/flags)拼成 `frames=480 flags=0x0`。

## core 日志层组件(新增 `src/core/Log.h` / `Log.cpp`)

`wa::log` facade 草案(缩进即代码块):

    namespace wa::log {

    enum class Level { Trace, Debug, Info, Warn, Err };   // 映射 spdlog::level

    // 初始化:建 async logger(overrun_oldest),挂已注册 sink;进程启动调用一次
    void init();
    void shutdown();                       // drain + spdlog::shutdown()

    void  setLevel(Level lvl);             // 转 logger->set_level
    Level level();
    bool  shouldLog(Level lvl);            // 转 logger->should_log,热路径短路

    // sink 注册(内部转 logger->sinks().push_back)
    void addFileSink(const std::string& path, size_t maxBytes, size_t maxFiles);
    void addStderrSink();
    void addSink(spdlog::sink_ptr sink);   // GUI 自定义 sink

    // 控制路径:同步记录(经宏 WA_LOG 短路级别后再拼 args/ret)
    void emit(Level lvl, const char* module, const char* call,
              std::string args, std::string ret);

    // 热路径:音频线程调用;内部 should_log(trace) 短路 + 非阻塞入队
    void emitTrace(const char* module, const char* call,
                   unsigned frames, unsigned flags, long hr);

    const char* hrName(long hr);           // HRESULT to 符号名

    } // namespace wa::log

- **payload 组装**:`emit`/`emitTrace` 内把 `Module::Call args ret` 拼成一行 payload,交 spdlog logger 按级别输出;前缀由 pattern 加。
- **宏短路**:`WA_LOG(lvl, module, call, argsExpr, retExpr)` 先 `if (wa::log::shouldLog(lvl))` 再求值 `argsExpr`/`retExpr`,关级别不做无谓格式化。
- **async 溢出**:logger 用 `spdlog::async_overflow_policy::overrun_oldest`,队满丢最旧且不阻塞;spdlog 自身统计丢弃,必要时周期性补一条 warn。
- **时间戳**:spdlog `%e` 提供毫秒墙钟,无需自管。
- **shutdown**:进程退出前 `wa::log::shutdown()` → drain 队列 + `spdlog::shutdown()`,避免丢尾部日志。

### `hrName`(HRESULT to 可读)

手写一张 WinAudio 会遇到的**常见码表**(硬编码,零依赖):`S_OK`、`S_FALSE`、`E_INVALIDARG`、`E_POINTER`、`E_NOINTERFACE`、`E_OUTOFMEMORY`、`RPC_E_CHANGED_MODE`、`AUDCLNT_E_*`(DEVICE_INVALIDATED / UNSUPPORTED_FORMAT / DEVICE_IN_USE / BUFFER_SIZE_NOT_ALIGNED / EXCLUSIVE_MODE_ONLY / NOT_INITIALIZED 等)、`AUDCLNT_S_*`。命中即返回符号名;未命中 fallback `FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM)` 取系统描述;再不行返回空(只留裸 hex)。

### `AudioFormat::toString()`(新增)

返回 `"48000/16/2"`(float 时 `"48000/32f/2"`),消除现状 CLI(`main.cpp:160/169`)、GUI(`AppUi.cpp:139-145/676-683/715-718/121-124`)4+ 处重复手写格式化,并供日志 args 统一使用。

## 三个 sink 与前端集成

- **文件 sink**:直接用 spdlog `rotating_file_sink_mt`(**大小轮转**,默认如 10 MB × 3 份)。GUI 默认 `winaudio.log`(exe 同目录,`GetModuleFileNameW` 定位)。
- **GUI 面板 sink**:**自定义 `spdlog::sinks::base_sink<std::mutex>`**,`sink_it_` 里把 spdlog 格式化后的行推入 GUI 侧一个**加锁 pending 缓冲**,`AppUi::draw()` 每帧取出追加到 `logLines_`(**环形行数上限**、自动滚动、清空按钮、**级别下拉运行时切**);**绝不在音频线程碰 ImGui**(sink 运行在 spdlog 后台线程)。
- **CLI stderr sink**:spdlog `stderr_color_sink_mt`(或普通 stderr sink),与现有 `\r` 状态行/表格(stdout)分离,便于 `2> log.txt`。参数 `--log-level <trace|debug|info|warn|err>`(默认 info)、`--log-file <path>`(给了才加 file sink)。
- **GUI 默认**:启动 `wa::log::init()` 后 `addFileSink("winaudio.log", ...)` + GUI 面板 sink。

## 全局约束(Global Constraints)

- **依赖 spdlog(submodule 固定版本)**:`third_party/spdlog` 作 git submodule,pin 到指定 release tag 的 commit;CMake `add_subdirectory` 链 `spdlog::spdlog`;作为 SYSTEM include 抑制其 `/W4` 告警;继承本项目 `CMAKE_MSVC_RUNTIME_LIBRARY`(`/MT`)。除 spdlog 外 core 不引其它第三方。
- **core 默认零输出**:未 `init`/未加 sink 时不产生 I/O;`Result` 语义与既有返回值行为**字节级不变**,日志纯旁路。
- **插桩纯观测**:所有日志插桩只读取现有变量/参数/HRESULT 来记录,**不得改变任何现有控制流、返回值或时序**;"补齐被忽略的返回值"仅指捕获 hr 用于记录一条 warn,原有的继续/跳过/break 行为保持不变。
- **音频线程绝不阻塞**:热路径经 async logger `overrun_oldest`(队满丢弃)+ `should_log` 短路;不加自定义锁、不在实时线程做 I/O。
- **现有 CLI stdout 行为不变**:状态行、`caps` 表、设备列表照旧;日志只走 stderr。
- **构建/质量**:C++17、静态 CRT(`/MT`)、本项目代码 `/W4` 零告警、Debug+Release 双配置;`build.bat`/`test.bat` 驱动;gtest 无硬件;GUI 存活验证。
- **回答用中文**,代码/命令/API 标识符保持英文。

## 错误处理与线程安全

- 线程安全交给 spdlog(logger/sink 内部线程安全);`wa::log` 级别用 spdlog 原子级别。
- GUI 面板 sink 的 pending 缓冲用一把轻锁,后台线程写、主线程读。
- 文件 sink I/O 失败由 spdlog 处理(默认抛 spdlog 异常);`wa::log` 在 init/注册处捕获并降级为静默,**不得把日志错误反抛进 core 的 `Result`**。
- 关闭顺序:停止音频流 → `wa::log::shutdown()`(drain + spdlog shutdown)。

## 测试策略(gtest,无硬件)

- 级别过滤:`setLevel` 后挂一个 fake sink(继承 `base_sink`),断言 `shouldLog()` 与实际收到的行级别集合正确。
- `hrName`:常见码映射正确;未知码走 fallback 不崩。
- `AudioFormat::toString`:int/float、单/双声道、各采样率的字符串正确;与旧手写输出等价。
- payload 组装:给定 module/call/args/ret 产出对齐 payload(列宽正确)。
- 非阻塞语义:async logger 设小队列 + `overrun_oldest`,高频写不阻塞、丢弃计数可观测(用 fake sink 计数)。
- 多 sink 分发:挂 N 个 fake sink,一条 `emit` 每个都收到。
- 沿用 `build.bat`/`test.bat` 双配置 + `/W4` 零告警(本项目代码)+ GUI 存活验证。

## 风险与待定

- **spdlog `/W4` 告警**:必须作为 SYSTEM include,否则本项目 `/W4` 零告警门槛会被第三方头污染。
- **async 溢出丢日志**:`overrun_oldest` 下极高频 trace 可能丢最旧行(含偶发控制路径行);队列容量取足够大(如 8192),并让 spdlog 统计丢弃。可接受(trace 本就诊断用途)。
- **线程短名**:spdlog `%t` 默认数字 id;需自定义 flag/registration 给 `main/pump/capW/renW` 短名,漏设显示 `?`。
- **submodule 版本**:pin 到 spdlog 最新稳定 release tag(实现时确定确切版本号与 commit)。
- **插桩工作量大**:~50 debug + ~15 warn 补齐 + 热路径 trace,分布在 `DeviceEnumerator.cpp`/`Engine.cpp`/`WasapiStream.cpp`/`MonitorEngine.cpp`。writing-plans 阶段按文件/子系统拆多任务。

## 附录 A:现状调用点地图(供 writing-plans 拆任务)

> core 里仅 4 个 .cpp 触及 COM/WASAPI:`DeviceEnumerator.cpp`、`Engine.cpp`、`WasapiStream.cpp`(重头)、`MonitorEngine.cpp`(仅 Win32 事件,设备调用全走 `IAudioBackend`)。`WavFile.cpp` 用 C stdio(非 COM)。现有错误处理两模式:(A) `if (FAILED(hr)) return HrToResult(hr,"where")`;(B) 包在 `SUCCEEDED(...)` 里,失败静默跳过——(B) 及"返回值忽略"处是 warn 补齐重点。

- **`DeviceEnumerator.cpp`**(~29 处): `CoCreateInstance`、`GetDefaultAudioEndpoint`/`GetDevice`、`IMMDevice::GetId`/`OpenPropertyStore`/`Activate`、`IPropertyStore::GetValue`(FriendlyName/DeviceFormat/OEMFormat)、`GetMixFormat`、`EnumAudioEndpoints`、`IMMDeviceCollection::GetCount`(忽略)/`Item`、`IsFormatSupported`(能力矩阵探测)。
- **`Engine.cpp`**(~6 处,`probeFormat`): `CoCreateInstance`、`GetDefaultAudioEndpoint`/`GetDevice`、`Activate`、`IsFormatSupported`(分类 S_OK/S_FALSE/失败)。
- **`WasapiStream.cpp`**(~33 处,**重头**): `QueryInterface(IAudioClient2)`、`IsOffloadCapable`、`SetClientProperties`、`GetService(IAudioSessionControl/2)`、`SetDuckingPreference`、`Initialize`(shared-requested / shared-mix / exclusive / 重对齐)、`GetMixFormat`、`IsFormatSupported(EXCLUSIVE)`、`GetDevicePeriod`(忽略)、`GetBufferSize`(部分忽略)、`Activate`、`SetEventHandle`、`Start`、`Stop`(忽略)、`GetService(Capture/Render Client)`、`GetNextPacketSize`、`GetBuffer`/`ReleaseBuffer`(cap+ren,**热路径**)、`GetCurrentPadding`(**热路径**);Win32: `CreateEventW`/`CloseHandle`/`SetEvent`/`WaitForSingleObject`/`GetLastError`/`std::thread`。
- **`MonitorEngine.cpp`**: 无直接 WASAPI;`ComInitGuard`、`WaitForSingleObject`/`SetEvent`;经 `IAudioBackend` 的 `open`/`start`/`stop`/`stats`/`dataReadyEvent`(高层入口,info/debug 该覆盖,每个内部展开成上面一串)。
- **`ComUtil.h`**: `CoInitializeEx`/`CoUninitialize`(接受 `RPC_E_CHANGED_MODE`)。`HrToResult(hr,"where")` 现有 `where` 文案可复用为 debug 的 module/call 种子。

## 附录 B:基础设施现状(复用点)

- `Result{ok, code, message}`(`Result.h:9`);`HrToResult(hr, where)`(`ComUtil.h:24`,产出十六进制 Result,可作日志文案种子)。
- `AudioFormat`(`AudioFormat.h:4`,缺 `toString()`);`toWaveFormatExtensible`/`fromWaveFormat` 已有。
- GUI `logLines_`(`AppUi.h:36`,绘制 `AppUi.cpp:329-333`)——面板 sink 落点。
- CLI 全 `printf`/`wprintf`(`cli/main.cpp`),`Result::message` 已在 7+ 处回显。
- `third_party/`(已有 `implot` submodule 惯例)——`spdlog` submodule 落此。
