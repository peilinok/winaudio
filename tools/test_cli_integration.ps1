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
$probeExe = Join-Path $buildDir "$Config\\winaudio_probe.exe"
$artifactDir = Join-Path $BuildDir "$Config\\cli_integration_artifacts"
$cliIntegrationOut = Join-Path $artifactDir "cli_integration_output.txt"
$buildWrapper = Join-Path $PSScriptRoot "invoke_msbuild_safe.ps1"
$envInspector = Join-Path $PSScriptRoot "inspect_build_environment.ps1"
. (Join-Path $PSScriptRoot "convergence_helpers.ps1")

function Write-Step($label) {
  Write-Host "`n=== $label ==="
}

function Require-Line($path, $pattern, $message) {
  if (-not (Select-String -LiteralPath $path -Pattern $pattern -Quiet)) {
    throw $message
  }
}

Write-Step "Build"
if (-not $SkipBuild) {
  Invoke-ConvergenceCommand {
    powershell -ExecutionPolicy Bypass -File $envInspector
  } "CLI integration failed: build environment inspection failed."
  Invoke-ConvergenceCommand {
    powershell -ExecutionPolicy Bypass -File $buildWrapper -PrintEnvironmentSummary cmake --build $BuildDir --config $Config --target winaudio_probe
  } "CLI integration failed: probe build failed."
} else {
  Write-Host "Skipping build; using existing $probeExe"
}

if (-not (Test-Path -LiteralPath $probeExe)) {
  throw "CLI integration failed: probe binary not found at $probeExe."
}

New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
$transcriptStarted = $false
try {
  Start-Transcript -LiteralPath $cliIntegrationOut -Force | Out-Null
  $transcriptStarted = $true
} catch {
  Write-Warning "CLI integration artifact capture could not start: $($_.Exception.Message)"
}

trap {
  if ($transcriptStarted) {
    try {
      Stop-Transcript | Out-Null
    } catch {}
  }
  throw
}

Write-Step "Devices"
$devicesOut = Join-Path $artifactDir "cli_integration_devices.txt"
Invoke-ConvergenceCommand {
  & $probeExe devices | Out-File -LiteralPath $devicesOut -Encoding utf8
} "CLI integration failed: devices command failed."
Get-Content $devicesOut | Select-Object -First 18
Require-Line $devicesOut '^CAPTURE_DEVICE:' "CLI integration failed: devices output did not include a capture device."
Require-Line $devicesOut '^RENDER_DEVICE:' "CLI integration failed: devices output did not include a render device."
$captureId = Extract-PreferredIdFromFile $devicesOut "CAPTURE_DEVICE:"
if ([string]::IsNullOrEmpty($captureId)) {
  throw "CLI integration failed: unable to extract a capture device id from devices output."
}
$renderId = Extract-PreferredIdFromFile $devicesOut "RENDER_DEVICE:"
if ([string]::IsNullOrEmpty($renderId)) {
  throw "CLI integration failed: unable to extract a render device id from devices output."
}

Write-Step "Devices Native Names"
$devicesNativeOut = Join-Path $artifactDir "cli_integration_devices_native.txt"
$nativeDevicesText = (& $probeExe devices "--device-name-format=native") | Out-String
if ($LASTEXITCODE -ne 0) {
  throw "CLI integration failed: native-name devices command failed."
}
$nativeDevicesText | Out-File -LiteralPath $devicesNativeOut -Encoding utf8
Get-Content $devicesNativeOut | Select-Object -First 18
if ($nativeDevicesText -notmatch '(?m)^CAPTURE_DEVICE:') {
  throw "CLI integration failed: native-name devices output did not include a capture device."
}
if ($nativeDevicesText -notmatch '(?m)^RENDER_DEVICE:') {
  throw "CLI integration failed: native-name devices output did not include a render device."
}
$escapedDevicesText = Get-Content -LiteralPath $devicesOut -Raw
if ($escapedDevicesText -match '\\u' -and $escapedDevicesText -eq $nativeDevicesText) {
  throw "CLI integration failed: native-name devices output did not differ from escaped-name output even though escaped Unicode names were present."
}

Write-Step "Quick Explicit Capture Device"
$quickCaptureOut = Join-Path $artifactDir "cli_integration_quick_capture.txt"
Invoke-ConvergenceCommand {
  & $probeExe quick "--capture-device-id=$captureId" | Out-File -LiteralPath $quickCaptureOut -Encoding utf8
} "CLI integration failed: explicit capture-device quick probe command failed."
Get-Content $quickCaptureOut | Select-Object -First 24
Require-Line $quickCaptureOut '^QuickSummary: success' "CLI integration failed: explicit capture-device quick probe was not successful."
Require-Line $quickCaptureOut '^Result: success$' "CLI integration failed: explicit capture-device quick result was not success."
Require-Line $quickCaptureOut ("^RequestedCaptureDeviceId: " + [regex]::Escape($captureId) + "$") "CLI integration failed: explicit capture-device quick probe did not preserve the requested capture device id."

Write-Step "Quick Explicit Render Device"
$quickRenderOut = Join-Path $artifactDir "cli_integration_quick_render.txt"
Invoke-ConvergenceCommand {
  & $probeExe quick "--render-device-id=$renderId" | Out-File -LiteralPath $quickRenderOut -Encoding utf8
} "CLI integration failed: explicit render-device quick probe command failed."
Get-Content $quickRenderOut | Select-Object -First 24
Require-Line $quickRenderOut '^QuickSummary: success' "CLI integration failed: explicit render-device quick probe was not successful."
Require-Line $quickRenderOut '^Result: success$' "CLI integration failed: explicit render-device quick result was not success."
Require-Line $quickRenderOut ("^RequestedRenderDeviceId: " + [regex]::Escape($renderId) + "$") "CLI integration failed: explicit render-device quick probe did not preserve the requested render device id."

Write-Step "Quick Invalid Render Device"
$quickRenderInvalidOut = Join-Path $artifactDir "cli_integration_quick_render_invalid.txt"
& $probeExe quick "--render-device-id=definitely-invalid-render-device" |
  Out-File -LiteralPath $quickRenderInvalidOut -Encoding utf8
if ($LASTEXITCODE -ne 2) {
  throw "CLI integration failed: invalid render-device quick probe did not fail with the expected exit code."
}
Get-Content $quickRenderInvalidOut | Select-Object -First 24
Assert-QuickInvalidRenderDeviceSemantics $quickRenderInvalidOut "CLI integration failed: invalid render-device quick probe"

Write-Step "Loopback Devices"
$loopbackDevicesOut = Join-Path $artifactDir "cli_integration_loopback_devices.txt"
Invoke-ConvergenceCommand {
  & $probeExe devices --source=loopback | Out-File -LiteralPath $loopbackDevicesOut -Encoding utf8
} "CLI integration failed: loopback devices command failed."
Get-Content $loopbackDevicesOut | Select-Object -First 20
Assert-LoopbackDevicesOutputSemantics $loopbackDevicesOut "CLI integration failed: loopback devices output"
Require-Line $loopbackDevicesOut '^LOOPBACK_CAPTURE_DEVICE:' "CLI integration failed: loopback devices output did not include a loopback capture device."
$loopbackCaptureId = Extract-PreferredIdFromFile $loopbackDevicesOut "LOOPBACK_CAPTURE_DEVICE:"
if ([string]::IsNullOrEmpty($loopbackCaptureId)) {
  throw "CLI integration failed: unable to extract a loopback capture device id from loopback devices output."
}

Write-Step "Loopback Devices Native Names"
$loopbackDevicesNativeOut = Join-Path $artifactDir "cli_integration_loopback_devices_native.txt"
$nativeLoopbackDevicesText = (& $probeExe devices "--source=loopback" "--device-name-format=native") | Out-String
if ($LASTEXITCODE -ne 0) {
  throw "CLI integration failed: native-name loopback devices command failed."
}
$nativeLoopbackDevicesText | Out-File -LiteralPath $loopbackDevicesNativeOut -Encoding utf8
Get-Content $loopbackDevicesNativeOut | Select-Object -First 18
if ($nativeLoopbackDevicesText -notmatch '(?m)^LOOPBACK_CAPTURE_DEVICE:') {
  throw "CLI integration failed: native-name loopback devices output did not include a loopback capture device."
}
$escapedLoopbackDevicesText = Get-Content -LiteralPath $loopbackDevicesOut -Raw
if ($escapedLoopbackDevicesText -match '\\u' -and $escapedLoopbackDevicesText -eq $nativeLoopbackDevicesText) {
  throw "CLI integration failed: native-name loopback devices output did not differ from escaped-name loopback output even though escaped Unicode names were present."
}

Write-Step "Quick Explicit Loopback Capture Device"
$quickLoopbackOut = Join-Path $artifactDir "cli_integration_quick_loopback_capture.txt"
Invoke-ConvergenceCommand {
  & $probeExe quick "--source=loopback" "--capture-device-id=$loopbackCaptureId" |
    Out-File -LiteralPath $quickLoopbackOut -Encoding utf8
} "CLI integration failed: explicit loopback capture-device quick probe command failed."
Get-Content $quickLoopbackOut | Select-Object -First 24
Require-Line $quickLoopbackOut '^QuickSummary: success' "CLI integration failed: explicit loopback capture-device quick probe was not successful."
Require-Line $quickLoopbackOut '^Result: success$' "CLI integration failed: explicit loopback capture-device quick result was not success."
Require-Line $quickLoopbackOut ("^RequestedCaptureDeviceId: " + [regex]::Escape($loopbackCaptureId) + "$") "CLI integration failed: explicit loopback capture-device quick probe did not preserve the requested loopback capture device id."
Require-Line $quickLoopbackOut '^RequestedCaptureMode: WASAPI Shared / Event$' "CLI integration failed: explicit loopback capture-device quick probe did not report the expected loopback capture mode."

Write-Step "Quick Invalid Loopback Capture Device"
$quickLoopbackInvalidOut = Join-Path $artifactDir "cli_integration_quick_loopback_invalid_capture.txt"
& $probeExe quick "--source=loopback" "--capture-device-id=not-a-loopback-render-endpoint" |
  Out-File -LiteralPath $quickLoopbackInvalidOut -Encoding utf8
if ($LASTEXITCODE -ne 2) {
  throw "CLI integration failed: invalid loopback capture-device quick probe did not fail with the expected exit code."
}
Get-Content $quickLoopbackInvalidOut | Select-Object -First 24
Assert-QuickInvalidLoopbackCaptureDeviceSemantics $quickLoopbackInvalidOut "CLI integration failed: invalid loopback capture-device quick probe"

Write-Step "Quick Source-Mode Failure"
$quickSourceModeFailOut = Join-Path $artifactDir "cli_integration_quick_source_mode_fail.txt"
& $probeExe quick "--capture-backend=wave" "--source=loopback" |
  Out-File -LiteralPath $quickSourceModeFailOut -Encoding utf8
if ($LASTEXITCODE -ne 2) {
  throw "CLI integration failed: source-mode quick probe did not fail with the expected exit code."
}
Get-Content $quickSourceModeFailOut | Select-Object -First 24
Require-Line $quickSourceModeFailOut '^QuickSummary: failed \| dump=none \| cap-fmt=not-negotiated \| ren-fmt=not-negotiated \| mode=not-started \| monitor=on \| cap-wave=not-started \| ren-wave=not-started \| ren-updates=0$' "CLI integration failed: source-mode quick probe did not surface the expected not-negotiated/not-started summary semantics."
Require-Line $quickSourceModeFailOut '^FailureStage: source-mode$' "CLI integration failed: source-mode quick probe did not report the expected failure stage."
Require-Line $quickSourceModeFailOut '^FailureReason: Selected backend does not support the chosen capture source\. Use --capture-backend=wasapi for loopback, or switch --source=mic\.$' "CLI integration failed: source-mode quick probe did not surface the expected recovery hint."

Write-Step "Quick Monitor Off Invalid Render Device"
$quickMonitorOffOut = Join-Path $artifactDir "cli_integration_quick_monitor_off_invalid_render.txt"
Invoke-ConvergenceCommand {
  & $probeExe quick "--monitor=off" "--render-device-id=definitely-invalid-render-device" |
    Out-File -LiteralPath $quickMonitorOffOut -Encoding utf8
} "CLI integration failed: monitor-off invalid-render-device quick probe command failed."
Get-Content $quickMonitorOffOut | Select-Object -First 24
Assert-QuickMonitorOffInvalidRenderDeviceSemantics $quickMonitorOffOut "CLI integration failed: monitor-off invalid-render-device quick probe"

Write-Step "Matrix Loopback Delay 0ms"
$matrixDelayOut = Join-Path $artifactDir "cli_integration_matrix_loopback_delay_0ms.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix "--matrix-source=loopback" "--matrix-delay=0ms" |
    Out-File -LiteralPath $matrixDelayOut -Encoding utf8
} "CLI integration failed: loopback delay-0ms matrix command failed."
Get-Content $matrixDelayOut | Select-Object -First 24
Assert-MatrixDelayScopeSemantics $matrixDelayOut "0ms" "CLI integration failed: loopback delay-0ms matrix"

Write-Step "Matrix Loopback Buffer cap40-ren40"
$matrixBufferOut = Join-Path $artifactDir "cli_integration_matrix_loopback_buffer_cap40_ren40.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix "--matrix-source=loopback" "--matrix-buffer=cap40-ren40" |
    Out-File -LiteralPath $matrixBufferOut -Encoding utf8
} "CLI integration failed: loopback cap40-ren40 matrix command failed."
Get-Content $matrixBufferOut | Select-Object -First 24
Assert-MatrixBufferScopeSemantics $matrixBufferOut "cap40-ren40" "CLI integration failed: loopback cap40-ren40 matrix"

Write-Step "Matrix Loopback Align On"
$matrixAlignOut = Join-Path $artifactDir "cli_integration_matrix_loopback_align_on.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix "--matrix-source=loopback" "--matrix-align=on" |
    Out-File -LiteralPath $matrixAlignOut -Encoding utf8
} "CLI integration failed: loopback align-on matrix command failed."
Get-Content $matrixAlignOut | Select-Object -First 24
Assert-MatrixAlignScopeSemantics $matrixAlignOut "on" "CLI integration failed: loopback align-on matrix"

Write-Step "Matrix Loopback Shared Only"
$matrixSharedOut = Join-Path $artifactDir "cli_integration_matrix_loopback_shared_only.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix "--matrix-source=loopback" "--matrix-wasapi-share=shared" |
    Out-File -LiteralPath $matrixSharedOut -Encoding utf8
} "CLI integration failed: loopback shared-only matrix command failed."
Get-Content $matrixSharedOut | Select-Object -First 24
Assert-MatrixSharedOnlySemantics $matrixSharedOut "CLI integration failed: loopback shared-only matrix"

Write-Step "Matrix Loopback Wave Render"
$matrixWaveRenderOut = Join-Path $artifactDir "cli_integration_matrix_loopback_wave_render.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix "--matrix-source=loopback" "--matrix-render-backend=wave" |
    Out-File -LiteralPath $matrixWaveRenderOut -Encoding utf8
} "CLI integration failed: loopback wave-render matrix command failed."
Get-Content $matrixWaveRenderOut | Select-Object -First 24
Assert-WaveRenderLoopbackMatrixSemantics $matrixWaveRenderOut "CLI integration failed: loopback wave-render matrix"

Write-Step "Matrix Loopback Wasapi Capture Wave Render"
$matrixWasapiWaveOut = Join-Path $artifactDir "cli_integration_matrix_loopback_wasapi_wave.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix "--matrix-source=loopback" "--matrix-capture-backend=wasapi" "--matrix-render-backend=wave" |
    Out-File -LiteralPath $matrixWasapiWaveOut -Encoding utf8
} "CLI integration failed: focused WASAPI-capture / WAVE-render loopback matrix command failed."
Get-Content $matrixWasapiWaveOut | Select-Object -First 24
Assert-WasapiWaveLoopbackMatrixSemantics $matrixWasapiWaveOut "CLI integration failed: focused WASAPI-capture / WAVE-render loopback matrix"

Write-Step "Matrix Loopback PCM16 Stereo"
$matrixProfileOut = Join-Path $artifactDir "cli_integration_matrix_loopback_pcm16_stereo.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix "--matrix-source=loopback" "--matrix-profile=pcm16-48k-stereo" |
    Out-File -LiteralPath $matrixProfileOut -Encoding utf8
} "CLI integration failed: focused PCM16 stereo loopback matrix command failed."
Get-Content $matrixProfileOut | Select-Object -First 24
Assert-Pcm16LoopbackMatrixSemantics $matrixProfileOut "CLI integration failed: focused PCM16 stereo loopback matrix"

if (-not [string]::IsNullOrEmpty($loopbackCaptureId) -and
    -not [string]::IsNullOrEmpty($renderId)) {
  Write-Step "Matrix Loopback Explicit Devices"
  $matrixExplicitOut = Join-Path $artifactDir "cli_integration_matrix_loopback_explicit_devices.txt"
  Invoke-ConvergenceCommand {
    & $probeExe matrix "--matrix-source=loopback" "--capture-device-id=$loopbackCaptureId" "--render-device-id=$renderId" |
      Out-File -LiteralPath $matrixExplicitOut -Encoding utf8
  } "CLI integration failed: explicit loopback matrix command failed."
  Get-Content $matrixExplicitOut | Select-Object -First 24
  Assert-ExplicitLoopbackMatrixSemantics $matrixExplicitOut $loopbackCaptureId $renderId "CLI integration failed: explicit loopback matrix"
}

Write-Host "ALL_TESTS_PASSED"

if ($transcriptStarted) {
  try {
    Stop-Transcript | Out-Null
  } catch {}
}
