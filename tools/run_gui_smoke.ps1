param(
  [string]$Config = "Debug",
  [string]$BuildDir,
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = Join-Path $root "build"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDir = Join-Path $root $BuildDir
}

$buildDir = $BuildDir
$guiExe = Join-Path $buildDir "$Config\\winaudio.exe"
$convergenceScript = Join-Path $PSScriptRoot "run_convergence_check.ps1"
$buildWrapper = Join-Path $PSScriptRoot "invoke_msbuild_safe.ps1"
$envInspector = Join-Path $PSScriptRoot "inspect_build_environment.ps1"
$artifactDir = Join-Path $BuildDir "$Config\\gui_smoke_artifacts"
$runStamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$guiSmokeOut = Join-Path $artifactDir ("gui_smoke_output_" + $runStamp + ".txt")
. (Join-Path $PSScriptRoot "convergence_helpers.ps1")

function Write-Step($label) {
  Write-Host "`n=== $label ==="
}

if (-not $SkipBuild) {
  Write-Step "Build Environment"
  Invoke-ConvergenceCommand {
    powershell -ExecutionPolicy Bypass -File $envInspector
  } "GUI smoke failed: build environment inspection failed."

  Write-Step "Build"
  Invoke-ConvergenceCommand {
    powershell -ExecutionPolicy Bypass -File $buildWrapper -PrintEnvironmentSummary cmake --build $BuildDir --config $Config --target winaudio
  } "GUI smoke failed: GUI build failed."
} else {
  Write-Step "Build"
  Write-Host "Skipping build; using existing artifacts from $BuildDir"
}

if (-not (Test-Path -LiteralPath $guiExe)) {
  throw "GUI smoke failed: GUI binary not found at $guiExe."
}

New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null

$arguments = @(
  "-ExecutionPolicy", "Bypass",
  "-File", $convergenceScript,
  "-Config", $Config,
  "-BuildDir", $BuildDir,
  "-GuiSmokeOnly",
  "-SkipBuild",
  "-SuppressBuildStepOutput"
)

cmd /c ("powershell " + [string]::Join(" ", ($arguments | ForEach-Object {
      if ($_ -match '\s') { '"' + $_.Replace('"', '""') + '"' } else { $_ }
    })) + " > `"" + $guiSmokeOut + "`" 2>&1")
$exitCode = $LASTEXITCODE
if (Test-Path -LiteralPath $guiSmokeOut) {
  Get-Content -LiteralPath $guiSmokeOut
}
if ($exitCode -ne 0) {
  throw "GUI smoke failed (exit=$exitCode)."
}
