param(
    [ValidateSet('x64', 'Win32')][string]$Architecture = 'x64',
    [string]$Generator = 'Visual Studio 17 2022'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$source = Join-Path $repo "build/performance-hook-original/source-$Architecture"
$binary = Join-Path $repo "build/performance-hook-original/build-$Architecture"
New-Item -ItemType Directory -Force "$source/src/hook", "$source/src/core", "$source/src/probe" | Out-Null
# Preserve the Git blobs byte for byte, independent of PowerShell's encoding.
@'
from pathlib import Path
import subprocess
import sys
root = Path(sys.argv[1])
destination = Path(sys.argv[2])
revision = '2483b0cbe7c07bfaba1e9f6d576a203aa03db3ee'
for name in ('src/hook/WinAudioHook.cpp', 'src/core/HookedCallPod.h'):
    data = subprocess.check_output(['git', '-C', str(root), 'show', revision + ':' + name])
    (destination / name).write_bytes(data)
'@ | py -3 -B - $repo $source
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract the pinned pre-optimization source' }
Copy-Item -LiteralPath "$repo/src/probe/HookBufferProbe.cpp" -Destination "$source/src/probe/HookBufferProbe.cpp"
Copy-Item -LiteralPath "$repo/src/probe/HookBufferHarness.h" -Destination "$source/src/probe/HookBufferHarness.h"
@'
cmake_minimum_required(VERSION 3.21)
project(HookBaseline LANGUAGES CXX)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
add_compile_definitions(UNICODE _UNICODE)
add_executable(HookBufferProbe src/probe/HookBufferProbe.cpp)
target_compile_options(HookBufferProbe PRIVATE /W4 /permissive- /utf-8 /sdl)
target_include_directories(HookBufferProbe PRIVATE src/core)
target_link_libraries(HookBufferProbe PRIVATE ole32 oleaut32 mmdevapi uuid advapi32)
add_library(WinAudioHook SHARED src/hook/WinAudioHook.cpp)
target_compile_options(WinAudioHook PRIVATE /W4 /permissive- /utf-8 /sdl)
target_include_directories(WinAudioHook PRIVATE src/core)
target_link_libraries(WinAudioHook PRIVATE ole32 oleaut32 mmdevapi uuid advapi32)
'@ | Set-Content -Encoding UTF8 "$source/CMakeLists.txt"
cmake -S $source -B $binary -G $Generator -A $Architecture
if ($LASTEXITCODE -ne 0) { throw 'Baseline configure failed' }
cmake --build $binary --config Release --target HookBufferProbe WinAudioHook -j 8
if ($LASTEXITCODE -ne 0) { throw 'Baseline build failed' }
Write-Output (Join-Path $binary 'Release/HookBufferProbe.exe')
