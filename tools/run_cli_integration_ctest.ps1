param(
  [string]$Config = "Debug",
  [string]$BuildDir = "build-cli",
  [switch]$SkipConfigure,
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDir = Join-Path $root $BuildDir
}

$buildWrapper = Join-Path $PSScriptRoot "invoke_msbuild_safe.ps1"

function Write-Step($label) {
  Write-Host "`n=== $label ==="
}

if (-not $SkipConfigure) {
  Write-Step "Configure"
  powershell -ExecutionPolicy Bypass -File $buildWrapper -PrintEnvironmentSummary `
    cmake -S $root -B $BuildDir
}

if (-not $SkipBuild) {
  Write-Step "Build"
  powershell -ExecutionPolicy Bypass -File $buildWrapper -PrintEnvironmentSummary `
    cmake --build $BuildDir --config $Config --target winaudio_probe
}

Write-Step "CTest"
ctest --test-dir $BuildDir -C $Config -R cli_integration_test --output-on-failure
