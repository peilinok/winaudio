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
