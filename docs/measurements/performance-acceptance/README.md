# 性能实施与复跑

本目录对应 [规格 #219](https://github.com/peilinok/winaudio/issues/219)。逐票状态、失败和处理决定见 [实施记录](implementation-log.md)。目前没有图表或 Inspector 达标结论。

## GUI 计量入口

本机使用 VS2026、MSVC 19.51.36257.0、Windows SDK 10.0.26100.0；仓库 CI 的 VS2022 要求保持原样。独立构建目录不会覆盖原有 `build/bin`。

```powershell
cmake -S . -B build/performance-vs18 -G 'Visual Studio 18 2026' -A x64
cmake --build build/performance-vs18 --config Release --target WinAudioGui WinAudioTests WinAudioHookTests -j 8

$env:WINAUDIO_FRAME_PROBE = '1'
$env:WINAUDIO_FRAME_PROBE_PATH = "$PWD/build/performance-vs18/gui-raw.txt"
$env:WINAUDIO_FRAME_PROBE_STAGE = 'meter-smoke'
$env:WINAUDIO_FRAME_PROBE_FLUSH = '0'
& build/performance-vs18/bin/Release/WinAudioGui.exe
# 在 GUI 中切换页面，检查真实 scene、操作目标和完成帧，然后正常关闭。
py -3 -B scripts/performance/summarize.py build/performance-vs18/gui-raw.txt --warmup-seconds 0 --duration-seconds 120 --require-complete --output build/performance-vs18/gui-summary.json
```

没有新增 GUI 命令行参数。日志每次追加一个新 run，汇总仅选择最后一个 run，报告 run 数、原文件 SHA256 和完成状态，不混合阶段或多次运行。图表和 Inspector 后续计时采用预热 30 秒、测量 120 秒、重复三次，当前烟测命令不能替代它们的负载验收。

`main-thread` 从 NewFrame 前到 RenderDrawData 返回之后，覆盖 GUI 刷新、界面生成和 DX11 提交；`present` 和 `probe-io` 分开。`interval_ticks` 覆盖前后帧之间的全部工作与等待。`probe-io` 包括主体序列化、写入和可选 flush；尾部 I/O 计量行及帧元数据不能计入自己的写入，开销仍包含在下一帧间隔里。

操作响应以输入消息的时间为起点，以实际目标状态的完成帧提交为终点，包含等待，不代表屏幕出光时间。Win32 消息时间以毫秒为单位，起点有这一量级的精度限制；页签点击只在真正进入对应页面时完成。之后的 Inspector 票仍须在筛选、选择、滚动等真实状态入口补接。

如果同一批消息积压了多个左键按下，或页签点击缺少可关联的输入起点，探针明确计入 `lost_operations`，汇总拒绝该轮。保留 ImGui 默认输入队列；单个未认领的起点在帧末清理。此阶段不猜测多个输入的对应关系，出现丢样时需重新采集。

同一帧的 `N` 为切片命中次数，不能当成样本帧数。帧元数据包含命中 mask，允许区分没有工作与缺行；切片、操作、工作量或帧溢出都会使汇总失败。无工作切片在汇总中为零，缺失元数据、频率不一致、帧号跳跃、截断、操作终点与提交帧不一致均拒绝。分位数采用排序后 `ceil(p*n)` 位置。`--require-budget` 额外检查完整结束和每组 8／16 ms 帧预算；操作、样本时长和具体场景覆盖仍需按票据核对，不能只凭退出码签收。

探针默认关闭，关闭时不取时。干扰对照分别使用：不设置 `WINAUDIO_FRAME_PROBE`、缓冲写入、`WINAUDIO_FRAME_PROBE_FLUSH=1`。必须保持同一 Release、负载和窗口条件；关闭模式需要独立外部观测，不能从不存在的探针记录推算零开销。解锁后已完成本地真实 GUI 与三模式干扰对照，见 [计量报告](meter-report.md)；空闲对照不能替代完整负载采集干扰核对。

## Hook 独立入口

```powershell
cmake --build build/performance-vs18 --config Release --target WinAudioHook HookBufferProbe HookAudioProbe WinAudioHookTests -j 8
& build/performance-vs18/bin/Release/WinAudioHookTests.exe
& build/performance-vs18/bin/Release/HookBufferProbe.exe
& build/performance-vs18/bin/Release/HookAudioProbe.exe

# 按同一测量夹具重建实施起点的真实 Hook，避免复写当前生产源码。
& scripts/performance/build-hook-baseline.ps1 -Architecture x64 -Generator 'Visual Studio 18 2026'
# x86 编译／行为检查使用 -A Win32 的独立构建目录。
```

HookBufferProbe 直接包含真实生产翻译单元，重命名 DllMain，使测量／测试可执行文件不自动安装 Hook。计时版本将观测标记完全编译掉；行为测试才启用这些标记。四入口逐一测直调、无 ring、quiet、capture 关、pump 关、全开，三次重复，每组先预热 10,000 次、批量 300,000 次，再取 20,000 个单次样本。原函数的 volatile 调用计数与输出写入阻止循环被删除。批量均值与单次 p50／p95／p99 分列，QPC 调用开销单列，不从单次测量中强减开销。

本机 QPC 频率 10 MHz，单次分布量化步长 100 ns，关闭路径出现 0 ns 样本不代表没有成本；判断消重需要同时看批量均值和确定性的零准备计数。初始 `hook-baseline-x64.csv` 在修改生产门控前采集；`hook-paired-*` 使用固定起点 Git blob 重建，x86 是补充重建对照，不冒称修改前已做过实机测量。

HookAudioProbe 只创建自己的两个同位数目标：静音 render 和真实 endpoint loopback。每阶段约 8 秒，覆盖未 Attach、capture 关、pump 关、pump 开、Detach、再次 Attach、只 Detach 其中一项。目标以 5 ms Sleep 请求间隔持续执行；实际间隔受系统调度影响，没有设置全局计时精度。记录目标自身调用失败、discontinuity、捕获帧数、静音数据非零字节和音频处理循环主体耗时；计时不含低频 GetBufferSize／GetService 和 Sleep，不能用关闭后的 pump 计数静止证明音频无干扰。

目标定期重新获取两种服务，覆盖记录关闭期间后续安装 Hook 的必要工作。稳定关闭阶段先让被允许的在途调用结束，再观察两个索引的增量，不把这个检查称为严格排空。每次运行必须确认实际模块路径，并将加载的 `WinAudioHook-5.dll` 与预期 DLL 做 SHA256 比较；重启 GUI 或重新 Attach 本身不证明用了新 DLL。

测试仅覆盖所登记的设备和静音信号；不能据此推断所有真实信号、硬件、调度环境、严格 IPC 发布或全局注入安全性。Hook 不套 GUI 的 0.5 ms 收益门槛，也不设置未经确认的纳秒上限。

本次配对结果、DLL 身份、生成代码与限制见 [Hook 验收报告](hook-report.md)。入库采样文本统一为 UTF-8／LF；局部 .gitattributes 保持这些文件字节，`hook-summary.json` 的 SHA256 对应入库版本。后续用 PowerShell 采样请显式 `| Out-File -Encoding utf8`，避免默认 UTF-16。

## 完整 Hook 配对复跑

以下命令在仓库根目录运行，写入独立复跑目录，不覆盖本次入库记录。同一套 HookAudioProbe 拷贝到优化前目录，保证它从自己的目录选择旧 DLL；每次均创建自己的新目标。三组音频按前／后顺序运行，期间不并行编译或运行其他测量。

```powershell
$ErrorActionPreference = 'Stop'
$perfRoot = $PWD.Path
$perfAfter = Join-Path $perfRoot 'build/performance-vs18/bin/Release'
$perfBefore = Join-Path $perfRoot 'build/performance-hook-original/build-x64/Release'
$perfRerun = Join-Path $perfRoot 'build/performance-vs18/hook-rerun'
New-Item -ItemType Directory -Force $perfRerun | Out-Null
cmake --build build/performance-vs18 --config Release --target WinAudioHook HookBufferProbe HookAudioProbe -j 8
if ($LASTEXITCODE -ne 0) { throw 'Current build failed' }
& scripts/performance/build-hook-baseline.ps1 -Architecture x64 -Generator 'Visual Studio 18 2026'
Copy-Item -LiteralPath "$perfAfter/HookAudioProbe.exe" -Destination "$perfBefore/HookAudioProbe.exe"

function Assert-LoadedHook($logPath, $folder) {
    $alias = [IO.Path]::GetFullPath("$folder/WinAudioHook-5.dll")
    $loaded = @(Get-Content -Encoding utf8 -LiteralPath $logPath | Where-Object { $_.StartsWith('loaded pid=') })
    if ($loaded.Count -ne 4) { throw "Expected two targets and two attaches: $logPath" }
    foreach ($line in $loaded) {
        if (!$line.EndsWith("path=$alias", [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unexpected loaded module: $line"
        }
    }
    $expected = (Get-FileHash -Algorithm SHA256 -LiteralPath "$folder/WinAudioHook.dll").Hash
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $alias).Hash
    if ($actual -ne $expected) { throw 'Loaded alias differs from the expected DLL' }
    "$alias SHA256=$actual" | Out-File -Encoding utf8 -Append "$perfRerun/loaded-hashes.txt"
}

& "$perfBefore/HookBufferProbe.exe" | Out-File -Encoding utf8 "$perfRerun/hook-paired-before-x64.csv"
if ($LASTEXITCODE -ne 0) { throw 'Before micro probe failed' }
& "$perfAfter/HookBufferProbe.exe" | Out-File -Encoding utf8 "$perfRerun/hook-paired-after-x64.csv"
if ($LASTEXITCODE -ne 0) { throw 'After micro probe failed' }
foreach ($repeat in 1..3) {
    $oldLog = "$perfRerun/hook-audio-before-$repeat.txt"
    & "$perfBefore/HookAudioProbe.exe" | Out-File -Encoding utf8 $oldLog
    if ($LASTEXITCODE -ne 0) { throw "Before audio probe failed: $repeat" }
    Assert-LoadedHook $oldLog $perfBefore
    $newLog = "$perfRerun/hook-audio-after-$repeat.txt"
    & "$perfAfter/HookAudioProbe.exe" | Out-File -Encoding utf8 $newLog
    if ($LASTEXITCODE -ne 0) { throw "After audio probe failed: $repeat" }
    Assert-LoadedHook $newLog $perfAfter
}

cmake -S . -B build/performance-vs18-x86 -G 'Visual Studio 18 2026' -A Win32
if ($LASTEXITCODE -ne 0) { throw 'x86 configure failed' }
cmake --build build/performance-vs18-x86 --config Release --target HookBufferProbe WinAudioHookTests -j 8
if ($LASTEXITCODE -ne 0) { throw 'x86 build failed' }
& build/performance-vs18-x86/bin/Release/WinAudioHookTests.exe
if ($LASTEXITCODE -ne 0) { throw 'x86 behavior tests failed' }
& scripts/performance/build-hook-baseline.ps1 -Architecture Win32 -Generator 'Visual Studio 18 2026'
& build/performance-hook-original/build-Win32/Release/HookBufferProbe.exe | Out-File -Encoding utf8 "$perfRerun/hook-paired-before-x86.csv"
if ($LASTEXITCODE -ne 0) { throw 'x86 before probe failed' }
& build/performance-vs18-x86/bin/Release/HookBufferProbe.exe | Out-File -Encoding utf8 "$perfRerun/hook-paired-after-x86.csv"
if ($LASTEXITCODE -ne 0) { throw 'x86 after probe failed' }
py -3 -B scripts/performance/summarize_hooks.py $perfRerun --output "$perfRerun/hook-summary.json"
if ($LASTEXITCODE -ne 0) { throw 'Incomplete probe data' }
```

哈希匹配仅确认加载身份；验收仍需逐组比较开启路径、音频尾延迟、discontinuity 等字段，不能只看探针 PASS。若目录中存在之前加载的别名且复制失败，命令会因模块路径／哈希不匹配终止，应先让本次自有目标退出再复跑，不能将旧 DLL 当新 DLL。
