# WinAudio 构建系统迁移到 CMake — 设计文档

**日期**：2026-07-01
**目标**：将 WinAudio 从手写的 Visual Studio 工程（`WinAudio.sln` + 4 个 `.vcxproj` + MSBuild）迁移到 CMake。保留 MSVC 工具链，工程文件改由 CMake 生成，提供命令行批处理构建脚本与规范的产物目录。**零代码改动**。

## 1. 范围与约束

**做什么**
- 用 CMake（顶层 + 每模块子 `CMakeLists.txt`）完整、保真地复刻现有 4 个 target 的构建配置。
- 提供 `.bat` 构建脚本 + `CMakePresets.json`。
- 规范产物目录：所有 CMake 产物集中在 out-of-source 的 `build/`，最终 exe/lib 收敛到 `build/bin/<Config>/`、`build/lib/<Config>/`。
- 删除手写的 `WinAudio.sln` 与 4 个 `.vcxproj`（改由 CMake 生成到 `build/`）。
- 更新 `.gitignore`、`CLAUDE.md` 构建章节、`winaudio-build-env` 记忆。

**不做什么（明确排除）**
- 不改任何 C++ 源码（`src/**`、`third_party/**` 一字不动）。
- 不换编译器：仍是 MSVC v143（cl.exe，来自 VS 2022 / Build Tools）。
- 不换 CRT、不改优化级别、不动警告策略——逐项复刻现状。
- 不处理 Phase 3 遗留的功能性 Minor（M1–M4 等，与构建系统无关）。

**关键约束（迁移必须保持不变）**
- x64 单平台；MSVC v143；C++17 + `/permissive-`；`/W4`；`/utf-8`；`/sdl`；`WindowsTargetPlatformVersion=10.0`。
- CharacterSet=Unicode → 全局定义 `UNICODE` / `_UNICODE`（CLI 用 `wmain`，GUI 用 `CreateWindowW` 等宽字符 API，必须保留）。
- Debug：动态调试 CRT（`/MDd`），`_DEBUG`，无优化。
- Release：动态 CRT（`/MD`），`NDEBUG`，WPO/LTO（`/GL`+`/LTCG`）、`/Gy`、`/Oi`、链接 `/OPT:REF`+`/OPT:ICF`。
- 迁移后 `/W4` 仍须零告警零错误（我方代码；三方 TU 关警告）。

## 2. 生成器与工作流

- **生成器**：`Visual Studio 17 2022`，`-A x64`，多配置（一次 configure 同时得 Debug+Release）。
- **驱动方式**：纯命令行 `cmake` + `.bat`，**不打开 VS IDE**。VS 生成器自动定位 MSVC，脚本**无需手动 `vcvars`**。
- **生成的工程文件**：`build/WinAudio.sln` 及各 `.vcxproj` 是 CMake 产物，位于 `build/`（git 忽略）；需要时仍可用 VS 打开，但非必需。
- 选 VS 生成器而非 Ninja 的理由：零新增工具、脚本免 vcvars、仍产出可选的 `.sln`（契合"工程文件由 CMake 生成"的诉求）。

## 3. CMake 目标结构

```
CMakeLists.txt                     # 顶层
cmake/CompilerWarnings.cmake       # helper: wa_set_project_warnings() -> /W4 /permissive- /utf-8 /sdl
third_party/CMakeLists.txt         # imgui / implot 静态库 + googletest add_subdirectory
src/core/CMakeLists.txt            # WinAudioCore
src/cli/CMakeLists.txt             # WinAudioCli
src/gui/CMakeLists.txt             # WinAudioGui
src/tests/CMakeLists.txt           # WinAudioTests
```

### 3.1 顶层 `CMakeLists.txt`
- `cmake_minimum_required(VERSION 3.21)`（VS 多配置 + `CMAKE_*_OUTPUT_DIRECTORY` 的 `<CONFIG>` 生成表达式稳定支持）。
- `project(WinAudio LANGUAGES CXX)`。
- 守卫：非 MSVC / 非 x64 时报错（本项目仅支持 MSVC x64）。
- 全局：`CMAKE_CXX_STANDARD 17`、`CMAKE_CXX_STANDARD_REQUIRED ON`、`CMAKE_CXX_EXTENSIONS OFF`。
- 全局定义：`add_compile_definitions(UNICODE _UNICODE)`。
- 全局 MSVC 运行库：默认即 `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`（动态 CRT，与现状一致，无需显式设置）。
- Release LTO：`set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)`（复刻 WPO/LTCG）。
- **产物输出目录**（对多配置生成器，`<CONFIG>` 子目录自动追加）：
  - `set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)` → exe 落 `build/bin/<Config>/`
  - `set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)` → .lib 落 `build/lib/<Config>/`
  - `CMAKE_LIBRARY_OUTPUT_DIRECTORY` 同 lib（无 dll，占位）
- `enable_testing()`。
- `add_subdirectory(third_party)` 后再 `add_subdirectory(src/core|cli|gui|tests)`。

### 3.2 `cmake/CompilerWarnings.cmake`
- 提供函数 `wa_set_project_warnings(<target>)`：对**我方** target 施加 `/W4 /permissive- /utf-8 /sdl`（`/std:c++17` 由全局标准给出）。
- 三方 target（imgui/implot/gtest）**不**调用它，改用 `/W0`（关警告），复刻现有 `TurnOffAllWarnings`。

### 3.3 `third_party/CMakeLists.txt`
- **imgui**（`add_library(imgui STATIC ...)`）：`imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp backends/imgui_impl_win32.cpp backends/imgui_impl_dx11.cpp`；`target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)`；`target_compile_options(imgui PRIVATE /W0)`。
- **implot**（`add_library(implot STATIC ...)`）：`implot.cpp implot_items.cpp`；`target_link_libraries(implot PUBLIC imgui)`；`target_include_directories(implot PUBLIC third_party/implot)`；`/W0`。
- **googletest**：`set(BUILD_GMOCK OFF)`、`set(INSTALL_GTEST OFF)`、`set(gtest_force_shared_crt ON)`（匹配动态 CRT）→ `add_subdirectory(googletest)`。产出 `gtest` / `gtest_main` target（自带正确的 Windows/MSVC 配置，`GTEST_HAS_PTHREAD` 由其自动处理）。

### 3.4 各模块 target（逐项复刻，见 §4 保真映射表）
- **WinAudioCore**：`add_library(WinAudioCore STATIC <11 个 .cpp>)`；`target_include_directories(PUBLIC src/core)`；`target_link_libraries(PUBLIC ole32 oleaut32)`（WASAPI/COM，消费方继承）；`wa_set_project_warnings`。
- **WinAudioCli**：`add_executable(WinAudioCli src/cli/main.cpp)`；`target_link_libraries(PRIVATE WinAudioCore)`（含 ole32/oleaut32 与 include）；控制台子系统（默认）；`wa_set_project_warnings`。
- **WinAudioGui**：`add_executable(WinAudioGui <AppUi.cpp main.cpp Spectrogram.cpp>)`；`target_link_libraries(PRIVATE WinAudioCore implot d3d11 dxgi d3dcompiler)`；`target_include_directories(PRIVATE src/gui)`；`target_compile_definitions(PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)`；`set_target_properties(WIN32_EXECUTABLE TRUE)` + `target_link_options(PRIVATE /ENTRY:mainCRTStartup)`（Windows 子系统 + main 入口，复刻现状）；`wa_set_project_warnings`。
- **WinAudioTests**：`add_executable(WinAudioTests <12 个 test_*.cpp> src/gui/Spectrogram.cpp)`；`target_include_directories(PRIVATE src/gui)`；`target_link_libraries(PRIVATE WinAudioCore gtest_main)`；`wa_set_project_warnings`；`include(GoogleTest)` + `gtest_discover_tests(WinAudioTests)`（供 ctest）。
  - 注：`Spectrogram.cpp` 同时被 gui 与 tests 各自编译（复刻现状，避免重构；YAGNI，不抽公共库）。

## 4. 保真映射表（vcxproj → CMake）

| target | 类型 | 源 | include(附加) | 宏 | 链接库 | 子系统/入口 | 警告 |
|---|---|---|---|---|---|---|---|
| WinAudioCore | 静态库 | src/core 11×.cpp | (PUBLIC) src/core | _LIB(自动) | (PUBLIC) ole32 oleaut32 | — | /W4 |
| WinAudioCli | Console exe | cli/main.cpp | 经 Core | — | Core(+ole32/oleaut32) | Console(默认) | /W4 |
| WinAudioGui | WIN32 exe | gui/{AppUi,main,Spectrogram}.cpp | src/gui、经 imgui/implot | NOMINMAX、_CRT_SECURE_NO_WARNINGS | Core、implot、d3d11、dxgi、d3dcompiler | WINDOWS + /ENTRY:mainCRTStartup | /W4（三方 /W0） |
| WinAudioTests | Console exe | tests 12×test_*.cpp + gui/Spectrogram.cpp | src/gui、经 gtest | (gtest 自带) | Core、gtest_main | Console | /W4（gtest /W0） |
| imgui | 静态库 | imgui 4 核心 + 2 后端 | (PUBLIC) imgui、imgui/backends | — | — | — | /W0 |
| implot | 静态库 | implot.cpp、implot_items.cpp | (PUBLIC) implot | — | (PUBLIC) imgui | — | /W0 |

全局（所有我方 target）：x64、v143、C++17、/permissive-、/utf-8、/sdl、UNICODE/_UNICODE、Debug=/MDd+_DEBUG、Release=/MD+NDEBUG+LTO。

## 5. 产物目录

- 所有 CMake 产物集中于 `build/`（out-of-source，git 忽略）：缓存、中间 obj、生成的 `.sln`/`.vcxproj`、pdb。
- 最终产物收敛（可预测）：
  - `build/bin/Debug/`、`build/bin/Release/` → `WinAudioCli.exe`、`WinAudioGui.exe`、`WinAudioTests.exe`
  - `build/lib/Debug/`、`build/lib/Release/` → `WinAudioCore.lib`（及 imgui/implot/gtest 的 .lib）
- `.gitignore`：新增 `build/`；删除现有散落的 `x64/`（连同 `x64\Debug`、`x64\Release` 产物）。

## 6. 构建脚本（批处理）

仓库根目录：

- **`build.bat [Debug|Release] [--clean]`**（默认 Release）
  - 首次或无缓存时：`cmake -S . -B build -G "Visual Studio 17 2022" -A x64`
  - 构建：`cmake --build build --config <cfg> -j`
  - `--clean`：先删 `build/` 再全量。
- **`test.bat [Debug|Release]`**（默认 Release）
  - `ctest --test-dir build -C <cfg> --output-on-failure`
- **`clean.bat`**：删 `build/`。
- **`CMakePresets.json`**：`configure`（VS 生成器、x64、binaryDir=build）、`build`（Debug/Release）、`test` preset，使 `cmake --preset` / IDE / CI 亦可直接用。

（脚本用 `.bat`/cmd 批处理，契合"批处理指令"诉求。）

## 7. 迁移与验证

**迁移步骤（高层，细节留给实现计划）**
1. 建 `cmake/CompilerWarnings.cmake` + 顶层 `CMakeLists.txt`。
2. 建 `third_party/CMakeLists.txt`（imgui/implot/gtest）。
3. 建 4 个模块 `CMakeLists.txt`。
4. 建 `build.bat`/`test.bat`/`clean.bat`/`CMakePresets.json`。
5. 更新 `.gitignore`。
6. 首次 configure + Debug/Release 构建 + 跑测试通过后，删除手写 `WinAudio.sln` + 4 个 `.vcxproj`，删除散落的 `x64/` 产物。
7. 更新 `CLAUDE.md` 构建/运行章节 + `winaudio-build-env` 记忆。

**成功标准（可验证）**
- `build.bat Debug` 与 `build.bat Release` 均成功，**/W4 零告警零错误**（我方代码）。
- 产物落在规范目录：`build/bin/<Config>/` 有 3 个 exe，`build/lib/<Config>/` 有 `WinAudioCore.lib`。
- `test.bat Debug` 与 `test.bat Release` 均 **56/56 通过**（ctest 经 `gtest_discover_tests` 注册，或直接跑 exe 亦 56/56）。
- `WinAudioGui.exe` 存活性：启动 3 秒不崩溃。
- 手写 `.sln`/`.vcxproj` 已删除，仓库根不再有 `x64/`；`git status` 干净（build/ 被忽略）。

## 8. 风险与对策

- **gtest 与我方 CRT 不匹配** → 链接错误。对策：`gtest_force_shared_crt ON`（匹配 `/MD`+`/MDd`）。
- **GUI 子系统/入口**：`WIN32_EXECUTABLE TRUE` 默认期待 `WinMain`，而代码是 `main()`。对策：附加 `/ENTRY:mainCRTStartup`（与现状一致）。
- **UNICODE 宏遗漏** → 宽字符 API / `wmain` 行为变化。对策：全局 `UNICODE`/`_UNICODE`。
- **Release LTO 差异**：CMake 默认 Release 无 `/GL`。对策：`CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON`（若引入问题，可回退——不影响正确性，仅优化）。
- **三方警告泄漏进 /W4** → 构建噪音/潜在 error。对策：imgui/implot/gtest target 用 `/W0`，仅我方 target 用 `/W4`。
- **产物路径**：多配置生成器会在 `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 下自动加 `<Config>`，故 `build/bin/` 会得到 `build/bin/Debug|Release`，符合预期。

## 9. 交付物清单

新增：顶层 `CMakeLists.txt`、`cmake/CompilerWarnings.cmake`、`third_party/CMakeLists.txt`、`src/{core,cli,gui,tests}/CMakeLists.txt`、`build.bat`、`test.bat`、`clean.bat`、`CMakePresets.json`。
修改：`.gitignore`、`CLAUDE.md`。
删除：`WinAudio.sln`、`src/core/WinAudioCore.vcxproj`、`src/cli/WinAudioCli.vcxproj`、`src/gui/WinAudioGui.vcxproj`、`src/tests/WinAudioTests.vcxproj`、散落的 `x64/` 产物。
