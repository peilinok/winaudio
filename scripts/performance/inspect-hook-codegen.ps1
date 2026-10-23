param(
    [ValidateSet('x64', 'Win32')][string]$Architecture = 'x64',
    [string]$Generator = 'Visual Studio 17 2022'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path.Replace('\', '/')
$source = "$repo/build/performance-hook-codegen/source-$Architecture"
$binary = "$repo/build/performance-hook-codegen/build-$Architecture"
New-Item -ItemType Directory -Force $source, $binary | Out-Null
# A separate /O2 object build exposes the acquire loads without changing the
# normal Release DLL's whole-program optimization or reusing its build files.
@"
cmake_minimum_required(VERSION 3.21)
project(HookCodegen LANGUAGES CXX)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_CXX_STANDARD 17)
add_library(HookCodegen OBJECT "$repo/src/hook/WinAudioHook.cpp")
target_include_directories(HookCodegen PRIVATE "$repo/src/core")
target_compile_definitions(HookCodegen PRIVATE UNICODE _UNICODE)
target_compile_options(HookCodegen PRIVATE /W4 /permissive- /utf-8 /sdl /FAs "/Fa$binary/")
"@ | Set-Content -Encoding UTF8 "$source/CMakeLists.txt"
cmake -S $source -B $binary -G $Generator -A $Architecture
if ($LASTEXITCODE -ne 0) { throw 'Codegen configure failed' }
cmake --build $binary --config Release -j 8
if ($LASTEXITCODE -ne 0) { throw 'Codegen build failed' }
Write-Output "$binary/WinAudioHook.asm"
