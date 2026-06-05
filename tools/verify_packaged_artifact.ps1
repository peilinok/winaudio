param(
  [string]$ArtifactDir = "artifacts\stage\binary"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($ArtifactDir)) {
  $ArtifactDir = Join-Path $root $ArtifactDir
}

$probeExe = Join-Path $ArtifactDir "winaudio_probe.exe"
$guiExe = Join-Path $ArtifactDir "winaudio.exe"
$readme = Join-Path $ArtifactDir "README.md"

foreach ($path in @($probeExe, $guiExe, $readme)) {
  if (-not (Test-Path -LiteralPath $path)) {
    throw "Packaged artifact is missing required file: $path"
  }
}

Write-Host "Verifying packaged probe artifact: $probeExe"
& $probeExe --help | Out-Null
if ($LASTEXITCODE -ne 0) {
  throw "Packaged winaudio_probe.exe --help failed with exit code $LASTEXITCODE"
}

Write-Host "Verifying packaged probe device listing"
& $probeExe devices | Out-Null
if ($LASTEXITCODE -ne 0) {
  throw "Packaged winaudio_probe.exe devices failed with exit code $LASTEXITCODE"
}

Write-Host "PACKAGED_ARTIFACT_VERIFIED=$ArtifactDir"
