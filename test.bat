@echo off
setlocal EnableDelayedExpansion
set "CFG=Release"
for %%A in (%*) do (
    if /I "%%A"=="Debug"   set "CFG=Debug"
    if /I "%%A"=="Release" set "CFG=Release"
)
ctest --test-dir build -C !CFG! --output-on-failure
exit /b %errorlevel%
