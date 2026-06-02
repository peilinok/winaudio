param(
  [Parameter(Mandatory = $true)]
  [string]$ArtifactLabel,

  [string]$Config = "Release",
  [string]$BuildDir = "build-release",
  [string]$OutputDir = "artifacts"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDir = Join-Path $root $BuildDir
}
if (-not [System.IO.Path]::IsPathRooted($OutputDir)) {
  $OutputDir = Join-Path $root $OutputDir
}

$stageDir = Join-Path $OutputDir "stage"
$binaryStageDir = Join-Path $stageDir "binary"
$symbolsStageDir = Join-Path $stageDir "symbols"

$binaryZip = Join-Path $OutputDir ("WinAudio-" + $ArtifactLabel + "-windows-x64.zip")
$symbolsZip = Join-Path $OutputDir ("WinAudio-" + $ArtifactLabel + "-windows-x64-symbols.zip")

$requiredBinaryFiles = @(
  (Join-Path $BuildDir "$Config\winaudio.exe"),
  (Join-Path $BuildDir "$Config\winaudio_probe.exe"),
  (Join-Path $root "README.md")
)
$requiredSymbolFiles = @(
  (Join-Path $BuildDir "$Config\winaudio.pdb"),
  (Join-Path $BuildDir "$Config\winaudio_probe.pdb")
)

foreach ($path in ($requiredBinaryFiles + $requiredSymbolFiles)) {
  if (-not (Test-Path -LiteralPath $path)) {
    throw "Required release asset input not found: $path"
  }
}

Remove-Item -LiteralPath $binaryStageDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $symbolsStageDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $binaryStageDir | Out-Null
New-Item -ItemType Directory -Force -Path $symbolsStageDir | Out-Null

Copy-Item -LiteralPath (Join-Path $BuildDir "$Config\winaudio.exe") -Destination $binaryStageDir
Copy-Item -LiteralPath (Join-Path $BuildDir "$Config\winaudio_probe.exe") -Destination $binaryStageDir
Copy-Item -LiteralPath (Join-Path $root "README.md") -Destination $binaryStageDir

Copy-Item -LiteralPath (Join-Path $BuildDir "$Config\winaudio.pdb") -Destination $symbolsStageDir
Copy-Item -LiteralPath (Join-Path $BuildDir "$Config\winaudio_probe.pdb") -Destination $symbolsStageDir

Remove-Item -LiteralPath $binaryZip -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $symbolsZip -Force -ErrorAction SilentlyContinue

Compress-Archive -Path (Join-Path $binaryStageDir '*') -DestinationPath $binaryZip
Compress-Archive -Path (Join-Path $symbolsStageDir '*') -DestinationPath $symbolsZip

Write-Host "BINARY_ZIP=$binaryZip"
Write-Host "SYMBOLS_ZIP=$symbolsZip"
