param(
  [string]$BuildDir
)

$ErrorActionPreference = "Stop"

$toolsDir = $PSScriptRoot
$root = Split-Path -Parent $toolsDir

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = Join-Path $root "build"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDir = Join-Path $root $BuildDir
}

$wrapper = Join-Path $toolsDir "invoke_msbuild_safe.ps1"
$inspector = Join-Path $toolsDir "inspect_build_environment.ps1"
. (Join-Path $toolsDir "build_environment_helpers.ps1")

$summary = Get-BuildEnvironmentSummary
if ($summary.PathEntries.Count -lt 1) {
  throw "Expected at least one PATH-like entry from build environment summary."
}
if ([string]::IsNullOrEmpty($summary.CanonicalKey)) {
  throw "Expected a canonical PATH key from build environment summary."
}

$inspectorOutput = powershell -ExecutionPolicy Bypass -File $inspector | Out-String
if ($inspectorOutput -notmatch 'Build environment') {
  throw "Expected build environment header in inspector output."
}
if ($inspectorOutput -notmatch 'Source:') {
  throw "Expected summary source line in inspector output."
}
if ($inspectorOutput -notmatch 'PATH-like entries:') {
  throw "Expected PATH-like entries line in inspector output."
}
if ($inspectorOutput -notmatch 'Build wrapper recommended:') {
  throw "Expected wrapper recommendation in inspector output."
}

$wrapperOutput = powershell -ExecutionPolicy Bypass -File $wrapper -PrintEnvironmentSummary cmd /c echo wrapper-ok | Out-String
if ($wrapperOutput -notmatch 'SafeBuild: path entries=') {
  throw "Expected safe-build environment summary from wrapper."
}
if ($wrapperOutput -notmatch 'source=') {
  throw "Expected safe-build summary to report its source."
}
if ($wrapperOutput -notmatch 'wrapper-ok') {
  throw "Expected wrapped command to run successfully."
}

$generatedProject = Join-Path $BuildDir "winaudio_core.vcxproj"
if (-not (Test-Path -LiteralPath $generatedProject)) {
  throw "Expected generated MSVC project file at $generatedProject."
}
$generatedProjectText = Get-Content -LiteralPath $generatedProject -Raw
if ($generatedProjectText -notmatch '/FS') {
  throw "Expected generated MSVC project to include /FS for shared PDB protection."
}
if ($generatedProjectText -match 'MultiThreadedDebugDLL|MultiThreadedDLL') {
  throw "Expected generated MSVC project to avoid dynamic CRT runtime library settings."
}
if ($generatedProjectText -notmatch 'MultiThreadedDebug') {
  throw "Expected generated MSVC project to include static Debug CRT runtime library settings."
}
if ($generatedProjectText -notmatch 'MultiThreaded</RuntimeLibrary>') {
  throw "Expected generated MSVC project to include static Release CRT runtime library settings."
}

Write-Host "ALL_TESTS_PASSED"
