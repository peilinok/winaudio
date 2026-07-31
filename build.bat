@echo off
setlocal EnableDelayedExpansion
set "CFG=Release"
set "ARCH=x64"
set "CMAKE_ARCH=x64"
set "BUILD_DIR=build"
set "DO_CLEAN="
for %%A in (%*) do (
    if /I "%%A"=="Debug"   set "CFG=Debug"
    if /I "%%A"=="Release" set "CFG=Release"
    if /I "%%A"=="x64" (
        set "ARCH=x64"
        set "CMAKE_ARCH=x64"
        set "BUILD_DIR=build"
    )
    if /I "%%A"=="x86" (
        set "ARCH=x86"
        set "CMAKE_ARCH=Win32"
        set "BUILD_DIR=build-x86"
    )
    if /I "%%A"=="--clean" set "DO_CLEAN=1"
)
if defined DO_CLEAN if exist !BUILD_DIR! rmdir /s /q !BUILD_DIR!
if not exist !BUILD_DIR!\CMakeCache.txt (
    cmake -S . -B !BUILD_DIR! -G "Visual Studio 17 2022" -A !CMAKE_ARCH! || exit /b 1
)
cmake --build !BUILD_DIR! --config !CFG! -j || exit /b 1
echo Built !CFG! !ARCH! -^> !BUILD_DIR!\bin\!CFG!\
exit /b 0
