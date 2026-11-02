# 附加远程资源寿命验收（#253）

本票修正远程线程仍可能读取参数时提前释放内存的问题，保留现有六槽补丁。此次没有交付逐方法选择、批量 GUI 或启动观测。

## 行为与所有权

- 远程调用独占进程引用、线程句柄和参数内存。超时、取消或等待失败均不结束线程；只有确认线程结束或目标退出后才能释放参数。等待失败立即保存真实系统错误，不统称超时。
- 按 PID＋创建时间预留进程实例，查询与注入身份不一致即失败。最多保留 32 个活动／待收尾资源条目，满额在创建新远程工作之前拒绝；同实例待收尾时不能再次注入。
- 远程 LoadLibrary 线程与后续 Hook 安装是两个寿命。待收尾表继续持有映射；线程已结束但 DLL 不存在时回收，DLL 已存在则等待安装结束。模块枚举失败保持未知，不能当作已经证明 DLL 不存在。
- HookThread 只报告安装结果，由控制器独占开启记录的权利。超时／取消后的迟到安装不会重新开启 capture。
- GUI 主循环的 pollBackground（包含最小化路径）非阻塞调用 reapPending，无需新一次附加才有收尾机会。正常退出仍不能确认结束的远程参数保留在目标中，并经日志明确报告未回收；目标退出最终回收该有限分配，不杀既有应用或远程线程。
- 加载成功根据目标模块列表核验预期 DLL 路径；远程线程的 DWORD 退出码只作结果记录，不当作完整的 x64 HMODULE。

## 判红证据

原始日志保存在本地 `.scratch/implement-pipeline-2026-11-02/253/`，不是远端下载资产。

| 用例 | 修改前结果 | 修改后结果 |
| --- | --- | --- |
| 参数读取前等待事件，控制方超时／取消 | 将旧等待释放逻辑保持行为地提取并接入生产 injectDll 后，两个用例失败：MEM_FREE，放行后读参数失败 | 参数保持 MEM_COMMIT，放行后值正确；线程完成后 MEM_FREE |
| 预期 DLL 缺失后再次附加 | 第二次被永久占为待收尾；探针退出 5 | 两次均明确报告模块不存在，探针退出 0 |
| 取消后迟到安装 | 仅恢复旧 HookThread 自行开 capture 的语句，真实生产回路捕获 captureEnabled=1 | captureEnabled 保持 0，维护回收后可重新附加 |
| 取消后 LoadLibrary 才结束且失败 | 独立的无 DLL 控制探针等待收尾超时，退出 10 | 无需新 attach，维护即释放条目；探针退出 0 |

对应日志：red-tests、red-missing-test、red-late-test、red-late-failure-test；生产提取证据为 red-production-extraction.patch。首次恢复红测源码保留了旧文件时间，导致 Debug DLL 未重建，该中间失败单独保存为 intermediate-stale-debug-tests；最终已强制重建修复 DLL，不将此中间日志当成通过。

## 本次验证

在原工作区、现有 Visual Studio 18 2026 的 x64 构建缓存中验证。下列 Debug／Release 命令均退出 0，构建未产生新增警告。

```powershell
cmake --build build --config Debug --target WinAudioTests WinAudioGui WinAudioHookTests AttachSmoke -j 6
cmake --build build --config Release --target WinAudioTests WinAudioGui WinAudioHookTests AttachSmoke -j 6
build/bin/Debug/WinAudioTests.exe --gtest_filter=RemoteThreadWait.*:RemoteInjectionTest.*:AttachLifecycleTest.*:OnDemandAttach.*:PipelineAttachSet.*:PipelineAttachUiModel.*:VtablePatch.*
build/bin/Release/WinAudioTests.exe --gtest_filter=RemoteThreadWait.*:RemoteInjectionTest.*:AttachLifecycleTest.*:OnDemandAttach.*:PipelineAttachSet.*:PipelineAttachUiModel.*:VtablePatch.*
build/bin/Debug/WinAudioHookTests.exe
build/bin/Release/WinAudioHookTests.exe
build/bin/Debug/AttachSmoke.exe
build/bin/Release/AttachSmoke.exe
```

- 直接相关测试各 **35/35**；包含真实自有远程入口、真实生产 LoadLibrary 门控、系统 WAIT_FAILED 错误、超时、取消、目标退出、控制方析构、迟到失败与重复 stop。自有子进程退出码均检查为 0；夹具失败时会清理自己创建的进程并保持测试失败。
- Hook 行为回归各 **14/14**。其中 6 项 HookServices 和六槽补丁是工作区既有内容，留给 #243 单独交付；不能把这一工作区测试数量当作 #253 独立 PR 的新增测试数量。
- 实际 AttachSmoke 两配置均 `control_calls=2 pump_ring=64 xruns=0`，父探针退出 0。该既有探针不检查真实目标退出码，目标退出证明来自上述新受控生命周期测试。
- 加载门控只在测试 DLL 中故意等待事件；没有在 DllMain／TLS 调用 COM。无设备环境可完成资源回路，重附加必须成功或严格匹配明确的 kInstallFailed，其他错误不能吞成通过。本机真实回路完成成功重附加。
- 最终原始构建、测试、冒烟日志及 GUI／Hook／测试目标的 SHA256 存于上述本地目录的 artifact-manifest.json。

## 未覆盖与后续

本次没有运行全量本地测试、未知用户应用、x86 真实注入或效果硬件验收；由后续票据和 CI 按各自范围执行。没有以旧 76/76 等历史结果证明新协议。实际已载旧 Hook 的目标必须重启，当前票不会动态替换其常驻 DLL；更完整的兼容能力检查由 #254 承接。性能、启动门控及逐 PID 配置不在本票验收内。
