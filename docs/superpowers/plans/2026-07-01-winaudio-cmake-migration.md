# WinAudio CMake Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the hand-written Visual Studio solution/projects with a CMake build (MSVC retained, project files generated), command-line `.bat` scripts, and a clean `build/`-centric artifact layout — with zero C++ source changes.

**Architecture:** Top-level `CMakeLists.txt` + a warnings helper + `third_party/CMakeLists.txt` (imgui/implot static libs + googletest via `add_subdirectory`) + one `CMakeLists.txt` per module (core/cli/gui/tests). The Visual Studio 17 2022 multi-config generator produces `.sln`/`.vcxproj` into `build/`; `.bat` scripts drive `cmake --build`. Built incrementally bottom-up so each task is independently configurable + buildable.

**Tech Stack:** CMake ≥ 3.21, Visual Studio 17 2022 generator, MSVC v143, C++17, Dear ImGui + ImPlot (vendored), GoogleTest (vendored full tree), DX11/WASAPI/COM.

## Global Constraints

- MSVC v143, x64 only; C++17 + `/permissive-`; `/W4` on our targets; `/utf-8`; `/sdl`. Third-party TUs (imgui/implot/gtest) compiled at `/W0` (warnings off), never `/W4`.
- CharacterSet=Unicode → global `UNICODE` / `_UNICODE` (CLI uses `wmain`; GUI uses `...W` Win32 APIs).
- Debug = dynamic debug CRT (`/MDd`) + `_DEBUG`; Release = dynamic CRT (`/MD`) + `NDEBUG` + IPO/LTO. GoogleTest MUST use `gtest_force_shared_crt ON` to match.
- `cmake_minimum_required(VERSION 3.21)` — this makes policy **CMP0092 default NEW** (no default `/W3` in the compiler flags), so adding `/W4` does not trigger `D9025 overriding /W3 with /W4`. Do not lower the minimum.
- GUI is a Windows-subsystem exe with a `main()` entry: `WIN32_EXECUTABLE TRUE` + `/ENTRY:mainCRTStartup` (replicates SubSystem=Windows + EntryPointSymbol=mainCRTStartup).
- Core links `ole32 oleaut32` as **PUBLIC** so cli/gui/tests inherit WASAPI/COM; GUI additionally links `d3d11 dxgi d3dcompiler`.
- Artifacts: exes → `build/bin/<Config>/`, static libs → `build/lib/<Config>/`. Everything under `build/` (git-ignored).
- **Zero source changes** — `src/**` and `third_party/** (imgui/implot/googletest)` C++ files are not edited. Success = both configs build `/W4` clean, 56/56 tests pass, GUI liveness OK.
- Build tool availability: `cmake`/`ctest` must be callable. If not on PATH, the VS-bundled copy is at `D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\` (add to PATH for the session), or install standalone CMake. The VS 2022 generator locates MSVC itself — no `vcvars` needed.

---

### Task 1: Foundation — top-level CMake, warnings helper, third_party, WinAudioCore

**Files:**
- Create: `cmake/CompilerWarnings.cmake`
- Create: `CMakeLists.txt` (repo root)
- Create: `third_party/CMakeLists.txt`
- Create: `src/core/CMakeLists.txt`

**Produces (later tasks rely on):** the `WinAudioCore` target (PUBLIC include `src/core`, PUBLIC `ole32 oleaut32`); the `imgui` + `implot` targets (PUBLIC include dirs); the googletest `gtest`/`gtest_main` targets; the `wa_set_project_warnings(<target>)` function; the top-level output-dir + standard settings. Later tasks each append one `add_subdirectory(src/<module>)` line to the root `CMakeLists.txt`.

- [ ] **Step 1: Confirm the build tool is available**

Run: `cmake --version`
Expected: prints `cmake version 3.2x`. If "not recognized", add the VS-bundled CMake bin dir (see Global Constraints) to `$env:PATH` for the session, then re-run. Do not proceed until `cmake --version` works.

- [ ] **Step 2: Create `cmake/CompilerWarnings.cmake`**

```cmake
# Warning + conformance flags for OUR targets (not third-party).
# /std:c++17 comes from the global CMAKE_CXX_STANDARD; /W3 is absent because
# CMP0092 is NEW (cmake_minimum_required 3.21), so /W4 adds cleanly (no D9025).
function(wa_set_project_warnings target)
    target_compile_options(${target} PRIVATE
        /W4            # warning level 4
        /permissive-   # conformance mode
        /utf-8         # UTF-8 source + execution charset
        /sdl           # additional security checks
    )
endfunction()
```

- [ ] **Step 3: Create the root `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.21)   # CMP0092 NEW => no default /W3 (see plan constraints)
project(WinAudio LANGUAGES CXX)

# --- Toolchain guards: MSVC x64 only ---
if(NOT MSVC)
    message(FATAL_ERROR "WinAudio requires the MSVC toolchain (cl.exe).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WinAudio requires a 64-bit (x64) build.")
endif()

# --- Language standard (=> /std:c++17, and /permissive- via the warnings helper) ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Unicode (matches VS CharacterSet=Unicode) ---
add_compile_definitions(UNICODE _UNICODE)

# --- Release whole-program optimization / LTO (matches WholeProgramOptimization) ---
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# --- Artifact layout: exe -> build/bin/<Config>, static lib -> build/lib/<Config> ---
# (The multi-config VS generator appends /<Config> automatically.)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(${CMAKE_SOURCE_DIR}/cmake/CompilerWarnings.cmake)

enable_testing()

add_subdirectory(third_party)
add_subdirectory(src/core)
# Tasks 2-4 each append one add_subdirectory line below (src/cli, src/gui, src/tests).
```

- [ ] **Step 4: Create `third_party/CMakeLists.txt`**

```cmake
# --- Dear ImGui: core + Win32/DX11 backends (static lib, warnings off) ---
add_library(imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_win32.cpp
    imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
)
target_compile_options(imgui PRIVATE /W0)

# --- ImPlot (depends on ImGui) ---
add_library(implot STATIC
    implot/implot.cpp
    implot/implot_items.cpp
)
target_link_libraries(implot PUBLIC imgui)
target_include_directories(implot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/implot)
target_compile_options(implot PRIVATE /W0)

# --- GoogleTest (vendored full tree); match our dynamic CRT, skip gmock/install ---
set(BUILD_GMOCK OFF)
set(INSTALL_GTEST OFF)
set(gtest_force_shared_crt ON)
add_subdirectory(googletest)
```

- [ ] **Step 5: Create `src/core/CMakeLists.txt`**

```cmake
add_library(WinAudioCore STATIC
    DelayFifo.cpp
    AudioFormat.cpp
    Fft.cpp
    RingBuffer.cpp
    SampleConvert.cpp
    WavFile.cpp
    DeviceEnumerator.cpp
    WasapiStream.cpp
    Engine.cpp
    FormatSpec.cpp
    MonitorEngine.cpp
)
target_include_directories(WinAudioCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(WinAudioCore PUBLIC ole32 oleaut32)  # WASAPI/COM, inherited by consumers
wa_set_project_warnings(WinAudioCore)
```

- [ ] **Step 6: Configure + build Core (Debug), verify /W4 clean + artifact location**

Run:
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target WinAudioCore
```
Expected: configure succeeds (googletest configures too); build succeeds with **0 warnings, 0 errors**; `build/lib/Debug/WinAudioCore.lib` exists. If any `warning` lines appear for `src/core/*` or a `D9025` appears, stop and fix flags (do NOT edit source). Also build Release to catch LTO issues early: `cmake --build build --config Release --target WinAudioCore` → `build/lib/Release/WinAudioCore.lib`.

- [ ] **Step 7: Commit**

```
git add cmake/CompilerWarnings.cmake CMakeLists.txt third_party/CMakeLists.txt src/core/CMakeLists.txt
git -c commit.gpgsign=false commit -m "build(cmake): foundation + third_party + WinAudioCore"
```

---

### Task 2: WinAudioCli target

**Files:**
- Create: `src/cli/CMakeLists.txt`
- Modify: `CMakeLists.txt` (uncomment/append `add_subdirectory(src/cli)`)

**Consumes:** `WinAudioCore` (PUBLIC include `src/core` + PUBLIC `ole32 oleaut32`), `wa_set_project_warnings`.

- [ ] **Step 1: Create `src/cli/CMakeLists.txt`**

```cmake
add_executable(WinAudioCli main.cpp)   # console subsystem is the default (no WIN32)
target_link_libraries(WinAudioCli PRIVATE WinAudioCore)  # inherits src/core include + ole32/oleaut32
wa_set_project_warnings(WinAudioCli)
```

- [ ] **Step 2: Append to the root `CMakeLists.txt`**

Add this line after `add_subdirectory(src/core)`:
```cmake
add_subdirectory(src/cli)
```

- [ ] **Step 3: Re-configure, build, smoke**

Run:
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target WinAudioCli
```
Expected: 0 warnings/errors; `build/bin/Debug/WinAudioCli.exe` exists. Smoke: `./build/bin/Debug/WinAudioCli.exe list` prints the device list (exit 0). (`wmain` links `wmainCRTStartup` automatically under MSVC.)

- [ ] **Step 4: Commit**

```
git add src/cli/CMakeLists.txt CMakeLists.txt
git -c commit.gpgsign=false commit -m "build(cmake): WinAudioCli target"
```

---

### Task 3: WinAudioGui target

**Files:**
- Create: `src/gui/CMakeLists.txt`
- Modify: `CMakeLists.txt` (append `add_subdirectory(src/gui)`)

**Consumes:** `WinAudioCore`, `implot` (PUBLIC-brings `imgui` + its include dirs), `wa_set_project_warnings`.

- [ ] **Step 1: Create `src/gui/CMakeLists.txt`**

```cmake
add_executable(WinAudioGui
    AppUi.cpp
    main.cpp
    Spectrogram.cpp
)
target_include_directories(WinAudioGui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(WinAudioGui PRIVATE WinAudioCore implot d3d11 dxgi d3dcompiler)
target_compile_definitions(WinAudioGui PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
# Windows-subsystem exe, but the code has main() (not WinMain): keep the CRT main entry.
set_target_properties(WinAudioGui PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(WinAudioGui PRIVATE /ENTRY:mainCRTStartup)
wa_set_project_warnings(WinAudioGui)
```

- [ ] **Step 2: Append to the root `CMakeLists.txt`**

Add after `add_subdirectory(src/cli)`:
```cmake
add_subdirectory(src/gui)
```

- [ ] **Step 3: Re-configure, build, liveness**

Run:
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target WinAudioGui
```
Expected: 0 warnings/errors on our TUs (imgui/implot compile silently at /W0); `build/bin/Debug/WinAudioGui.exe` exists. Liveness (opens a real window):
```powershell
$p = Start-Process ".\build\bin\Debug\WinAudioGui.exe" -PassThru; Start-Sleep 3
if ($p.HasExited) { "GUI EXITED EARLY (code $($p.ExitCode)) - FAIL" } else { Stop-Process $p; "GUI started OK" }
```
Expected: "GUI started OK".

- [ ] **Step 4: Commit**

```
git add src/gui/CMakeLists.txt CMakeLists.txt
git -c commit.gpgsign=false commit -m "build(cmake): WinAudioGui target (WIN32 + mainCRTStartup)"
```

---

### Task 4: WinAudioTests target + ctest discovery

**Files:**
- Create: `src/tests/CMakeLists.txt`
- Modify: `CMakeLists.txt` (append `add_subdirectory(src/tests)`)

**Consumes:** `WinAudioCore`, `gtest_main` (from `third_party/googletest`), `src/gui/Spectrogram.cpp`, `wa_set_project_warnings`.

- [ ] **Step 1: Create `src/tests/CMakeLists.txt`**

```cmake
add_executable(WinAudioTests
    test_delayfifo.cpp
    test_smoke.cpp
    test_audioformat.cpp
    test_fft.cpp
    test_ringbuffer.cpp
    test_sampleconvert.cpp
    test_wavfile.cpp
    test_formatspec.cpp
    test_scopebuffer.cpp
    test_analysis.cpp
    test_monitorengine.cpp
    test_spectrogram.cpp
    ${CMAKE_SOURCE_DIR}/src/gui/Spectrogram.cpp   # compiled into tests, as in the current setup
)
target_include_directories(WinAudioTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/gui                   # for Spectrogram.h
)
target_link_libraries(WinAudioTests PRIVATE WinAudioCore gtest_main)
wa_set_project_warnings(WinAudioTests)

include(GoogleTest)
gtest_discover_tests(WinAudioTests)
```

- [ ] **Step 2: Append to the root `CMakeLists.txt`**

Add after `add_subdirectory(src/gui)`:
```cmake
add_subdirectory(src/tests)
```

- [ ] **Step 3: Re-configure, build, run tests (Debug)**

Run:
```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target WinAudioTests
./build/bin/Debug/WinAudioTests.exe
```
Expected: 0 warnings/errors; `[  PASSED  ] 56 tests.` Then verify ctest sees them:
```
ctest --test-dir build -C Debug --output-on-failure
```
Expected: ctest runs the discovered cases, `100% tests passed` (56 tests).

- [ ] **Step 4: Commit**

```
git add src/tests/CMakeLists.txt CMakeLists.txt
git -c commit.gpgsign=false commit -m "build(cmake): WinAudioTests target + gtest_discover_tests"
```

---

### Task 5: Build scripts + CMakePresets

**Files:**
- Create: `build.bat`, `test.bat`, `clean.bat`, `CMakePresets.json`

**Consumes:** the full CMake project from Tasks 1–4.

- [ ] **Step 1: Create `build.bat`**

```bat
@echo off
setlocal EnableDelayedExpansion
set "CFG=Release"
set "DO_CLEAN="
for %%A in (%*) do (
    if /I "%%A"=="Debug"   set "CFG=Debug"
    if /I "%%A"=="Release" set "CFG=Release"
    if /I "%%A"=="--clean" set "DO_CLEAN=1"
)
if defined DO_CLEAN if exist build rmdir /s /q build
if not exist build\CMakeCache.txt (
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64 || exit /b 1
)
cmake --build build --config !CFG! -j || exit /b 1
echo Built !CFG! -^> build\bin\!CFG!\
exit /b 0
```

- [ ] **Step 2: Create `test.bat`**

```bat
@echo off
setlocal EnableDelayedExpansion
set "CFG=Release"
for %%A in (%*) do (
    if /I "%%A"=="Debug"   set "CFG=Debug"
    if /I "%%A"=="Release" set "CFG=Release"
)
ctest --test-dir build -C !CFG! --output-on-failure
exit /b %errorlevel%
```

- [ ] **Step 3: Create `clean.bat`**

```bat
@echo off
if exist build rmdir /s /q build
echo Removed build\
```

- [ ] **Step 4: Create `CMakePresets.json`**

```json
{
  "version": 3,
  "cmakeMinimumRequired": { "major": 3, "minor": 21, "patch": 0 },
  "configurePresets": [
    {
      "name": "vs2022",
      "displayName": "VS 2022 x64 (multi-config)",
      "generator": "Visual Studio 17 2022",
      "architecture": "x64",
      "binaryDir": "${sourceDir}/build"
    }
  ],
  "buildPresets": [
    { "name": "debug",   "configurePreset": "vs2022", "configuration": "Debug" },
    { "name": "release", "configurePreset": "vs2022", "configuration": "Release" }
  ],
  "testPresets": [
    { "name": "debug",   "configurePreset": "vs2022", "configuration": "Debug",   "output": { "outputOnFailure": true } },
    { "name": "release", "configurePreset": "vs2022", "configuration": "Release", "output": { "outputOnFailure": true } }
  ]
}
```

- [ ] **Step 5: End-to-end verify both configs via the scripts**

Run (from a clean tree):
```
.\clean.bat
.\build.bat Release
.\test.bat Release
.\build.bat Debug
.\test.bat Debug
```
Expected: both builds 0 warnings/0 errors; both `test.bat` runs report all 56 tests passing; exes present under `build/bin/Release/` and `build/bin/Debug/`. Also sanity-check the preset path: `cmake --preset vs2022` reconfigures without error.

- [ ] **Step 6: Commit**

```
git add build.bat test.bat clean.bat CMakePresets.json
git -c commit.gpgsign=false commit -m "build(cmake): batch build/test/clean scripts + CMakePresets"
```

---

### Task 6: Remove the old MSBuild project files + full from-scratch verification

**Files:**
- Modify: `.gitignore`
- Delete: `WinAudio.sln`, `src/core/WinAudioCore.vcxproj`, `src/cli/WinAudioCli.vcxproj`, `src/gui/WinAudioGui.vcxproj`, `src/tests/WinAudioTests.vcxproj`
- Delete (local, untracked): `x64/` (scattered MSBuild artifacts)

- [ ] **Step 1: Add `build/` to `.gitignore`**

Edit `.gitignore` — under the `# Build output` block, add a line `build/` (keep every existing line; `x64/`, `[Bb]in/`, `[Oo]bj/`, `.vs/` etc. stay). Resulting top of file:
```
# Build output
build/
[Bb]in/
[Oo]bj/
x64/
```

- [ ] **Step 2: Remove the hand-written MSBuild project files (tracked → `git rm`)**

```
git rm WinAudio.sln src/core/WinAudioCore.vcxproj src/cli/WinAudioCli.vcxproj src/gui/WinAudioGui.vcxproj src/tests/WinAudioTests.vcxproj
```

- [ ] **Step 3: Remove scattered local build output (untracked, already git-ignored)**

```powershell
if (Test-Path x64) { Remove-Item -Recurse -Force x64 }
```

- [ ] **Step 4: Full from-scratch dual-config verification**

Run:
```
.\clean.bat
.\build.bat Debug
.\test.bat Debug
.\build.bat Release
.\test.bat Release
```
Expected: both configs build **0 warnings / 0 errors**; both test runs **56/56 pass**; `build/bin/{Debug,Release}/` each contain `WinAudioCli.exe`, `WinAudioGui.exe`, `WinAudioTests.exe`; `build/lib/{Debug,Release}/WinAudioCore.lib` present. GUI liveness (Release):
```powershell
$p = Start-Process ".\build\bin\Release\WinAudioGui.exe" -PassThru; Start-Sleep 3
if ($p.HasExited) { "FAIL (exit $($p.ExitCode))" } else { Stop-Process $p; "GUI started OK" }
```
Then confirm a clean tree: `git status --short` shows only the staged `.sln`/`.vcxproj` deletions plus the modified `.gitignore` (the CMake files were already committed in Tasks 1–5) — **no `x64/`, no `build/`** (both ignored). If `build/` or `x64/` appears in `git status`, fix `.gitignore`.

- [ ] **Step 5: Commit**

```
git add .gitignore
git -c commit.gpgsign=false commit -m "build(cmake): drop hand-written .sln/.vcxproj; ignore build/"
```

---

### Task 7: Update CLAUDE.md build docs + memory

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Rewrite the "构建与运行" build commands in `CLAUDE.md`**

Replace the MSBuild-based build block (the `$MSBuild = ...` / `& $MSBuild WinAudio.sln ...` / `.\x64\Debug\...` lines) with the CMake workflow. Keep the CLI/GUI usage sections, but update every `.\x64\Debug\WinAudioCli.exe` / `.\x64\Debug\WinAudioTests.exe` path to `.\build\bin\Debug\...`. New build block:

````markdown
## 构建与运行

本项目使用 **CMake**（Visual Studio 17 2022 生成器，保留 MSVC），命令行/批处理驱动，产物集中在 `build/`。

```powershell
# 构建（默认 Release；可传 Debug；--clean 先清）
.\build.bat Debug
.\build.bat Release
.\build.bat Release --clean

# 运行测试（ctest，56 个）
.\test.bat Debug        # 或 .\test.bat Release
# 直接跑测试 exe 亦可：
.\build\bin\Debug\WinAudioTests.exe
.\build\bin\Debug\WinAudioTests.exe --gtest_filter=MonitorEngine.*

# 清理
.\clean.bat

# 也可直接用 CMake / preset：
cmake --preset vs2022
cmake --build build --config Release -j
```

产物：`build\bin\<Config>\{WinAudioCli,WinAudioGui,WinAudioTests}.exe`、`build\lib\<Config>\WinAudioCore.lib`。
若 `cmake` 不在 PATH，用 VS 自带的：`D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`。
````

Also update the CLI/GUI usage examples further down: change `.\x64\Debug\WinAudioCli.exe` → `.\build\bin\Debug\WinAudioCli.exe` and `.\x64\Debug\WinAudioGui.exe` → `.\build\bin\Debug\WinAudioGui.exe` (all occurrences).

- [ ] **Step 2: Build once more to confirm docs match reality**

Run: `.\build.bat Release` then `.\test.bat Release`
Expected: builds clean, 56/56 pass (the doc's commands are exactly these).

- [ ] **Step 3: Commit**

```
git add CLAUDE.md
git -c commit.gpgsign=false commit -m "docs: CMake build workflow in CLAUDE.md"
```

- [ ] **Step 4: Update the `winaudio-build-env` memory (controller does this, not a subagent)**

After the branch merges, update `C:\Users\admin\.claude\projects\D--work-vibe-coding-WinAudio\memory\winaudio-build-env.md`: the build is now CMake (VS 2022 generator, `build.bat`/`test.bat`, artifacts in `build/bin|lib/<Config>/`), replacing the raw-MSBuild instructions. Keep the note that tools may not be on PATH.

---

## Verification Summary (success criteria for the whole plan)

- `.\build.bat Debug` and `.\build.bat Release` both succeed with **0 warnings / 0 errors** on our targets (imgui/implot/gtest silent at /W0).
- `.\test.bat Debug` and `.\test.bat Release` both report **56/56 passing**.
- Artifacts land exactly in `build/bin/<Config>/` (3 exes) and `build/lib/<Config>/WinAudioCore.lib`.
- `WinAudioGui.exe` liveness OK (starts, survives 3 s).
- Hand-written `.sln` + 4 `.vcxproj` deleted; repo root has no tracked `x64/`; `git status` clean (`build/` ignored).
- `CLAUDE.md` build/run commands updated to the CMake workflow.
- **No C++ source changed** (verify: the diff touches only CMake files, `.bat`, `.json`, `.gitignore`, `CLAUDE.md`, and deletes the `.sln`/`.vcxproj`).
