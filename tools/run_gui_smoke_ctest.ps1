param(
  [string]$Config = "Debug",
  [string]$BuildDir = "build-gui"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDir = Join-Path $root $BuildDir
}

$buildWrapper = Join-Path $PSScriptRoot "invoke_msbuild_safe.ps1"

Write-Host "`n=== Configure ==="
powershell -ExecutionPolicy Bypass -File $buildWrapper -PrintEnvironmentSummary `
  cmake -S $root -B $BuildDir -DWINAUDIO_ENABLE_GUI_SMOKE_TESTS=ON

Write-Host "`n=== Build ==="
powershell -ExecutionPolicy Bypass -File $buildWrapper -PrintEnvironmentSummary `
  cmake --build $BuildDir --config $Config --target winaudio

Write-Host "`n=== CTest ==="
ctest --test-dir $BuildDir -C $Config -R gui_smoke_test --output-on-failure
