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
$buildWrapper = Join-Path $PSScriptRoot "invoke_msbuild_safe.ps1"
$envInspector = Join-Path $PSScriptRoot "inspect_build_environment.ps1"
$artifactDir = Join-Path $buildDir "$Config\\hardware_validation_artifacts"
$hardwareValidationOut = Join-Path $artifactDir "hardware_validation_output.txt"
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
  } "Hardware validation failed: build environment inspection failed."
  Invoke-ConvergenceCommand {
    powershell -ExecutionPolicy Bypass -File $buildWrapper -PrintEnvironmentSummary cmake --build $BuildDir --config $Config --target winaudio_probe
  } "Hardware validation failed: probe build failed."
} else {
  Write-Host "Skipping build; using existing $probeExe"
}

New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
$transcriptStarted = $false
try {
  Start-Transcript -LiteralPath $hardwareValidationOut -Force | Out-Null
  $transcriptStarted = $true
} catch {
  Write-Warning "Hardware validation artifact capture could not start: $($_.Exception.Message)"
}

trap {
  if ($transcriptStarted) {
    try {
      Stop-Transcript | Out-Null
    } catch {}
  }
  throw
}

if (-not (Test-Path -LiteralPath $probeExe)) {
  throw "Hardware validation failed: probe binary not found at $probeExe."
}

Write-Step "Devices"
$devicesOut = Join-Path $BuildDir "$Config\\hardware_devices.txt"
Invoke-ConvergenceCommand {
  & $probeExe devices | Out-File -LiteralPath $devicesOut -Encoding utf8
} "Hardware validation failed: devices command failed."
Get-Content $devicesOut | Select-Object -First 20
Require-Line $devicesOut '^CAPTURE_DEVICE:' "Hardware validation failed: no capture devices listed."
Require-Line $devicesOut '^RENDER_DEVICE:' "Hardware validation failed: no render devices listed."
$devicesNativeOut = Join-Path $BuildDir "$Config\\hardware_devices_native.txt"
Invoke-ConvergenceCommand {
  & $probeExe devices "--device-name-format=native" | Out-File -LiteralPath $devicesNativeOut -Encoding utf8
} "Hardware validation failed: native-name devices command failed."
Require-Line $devicesNativeOut '^CAPTURE_DEVICE:' "Hardware validation failed: native-name devices output listed no capture devices."
Require-Line $devicesNativeOut '^RENDER_DEVICE:' "Hardware validation failed: native-name devices output listed no render devices."
$escapedDevicesText = Get-Content -LiteralPath $devicesOut -Raw
$nativeDevicesText = Get-Content -LiteralPath $devicesNativeOut -Raw
if ($escapedDevicesText -match '\\u[0-9A-F]{4}' -and
    ($nativeDevicesText -eq $escapedDevicesText -or
     $nativeDevicesText -match '\\u[0-9A-F]{4}')) {
  throw "Hardware validation failed: native-name devices output did not produce a distinct human-readable device listing."
}

$captureId = Extract-PreferredIdFromFile $devicesOut "CAPTURE_DEVICE:"
$renderId = Extract-PreferredIdFromFile $devicesOut "RENDER_DEVICE:"

Write-Step "Loopback Devices"
$loopbackDevicesOut = Join-Path $BuildDir "$Config\\hardware_loopback_devices.txt"
Invoke-ConvergenceCommand {
  & $probeExe devices --source=loopback | Out-File -LiteralPath $loopbackDevicesOut -Encoding utf8
} "Hardware validation failed: loopback devices command failed."
Get-Content $loopbackDevicesOut | Select-Object -First 24
Require-Line $loopbackDevicesOut '^Loopback Capture Devices \(' "Hardware validation failed: loopback device output did not label loopback capture devices clearly."
Require-Line $loopbackDevicesOut '^Loopback capture uses render endpoints as capture sources\.$' "Hardware validation failed: loopback device output did not explain render-backed capture sources."
Require-Line $loopbackDevicesOut '^LOOPBACK_CAPTURE_DEVICE:' "Hardware validation failed: loopback device output did not use a dedicated loopback capture prefix."
$loopbackDevicesNativeOut = Join-Path $BuildDir "$Config\\hardware_loopback_devices_native.txt"
Invoke-ConvergenceCommand {
  & $probeExe devices "--source=loopback" "--device-name-format=native" | Out-File -LiteralPath $loopbackDevicesNativeOut -Encoding utf8
} "Hardware validation failed: native-name loopback devices command failed."
Require-Line $loopbackDevicesNativeOut '^LOOPBACK_CAPTURE_DEVICE:' "Hardware validation failed: native-name loopback devices output listed no loopback capture devices."
$escapedLoopbackDevicesText = Get-Content -LiteralPath $loopbackDevicesOut -Raw
$nativeLoopbackDevicesText = Get-Content -LiteralPath $loopbackDevicesNativeOut -Raw
if ($escapedLoopbackDevicesText -match '\\u[0-9A-F]{4}' -and
    ($nativeLoopbackDevicesText -eq $escapedLoopbackDevicesText -or
     $nativeLoopbackDevicesText -match '\\u[0-9A-F]{4}')) {
  throw "Hardware validation failed: native-name loopback devices output did not produce a distinct human-readable loopback listing."
}
$loopbackGuidanceCount = (Select-String -LiteralPath $loopbackDevicesOut -Pattern '^Loopback capture uses render endpoints as capture sources\.$').Count
if ($loopbackGuidanceCount -ne 1) {
  throw "Hardware validation failed: loopback device output repeated the render-backed capture guidance line."
}
$loopbackCaptureId = Extract-PreferredIdFromFile $loopbackDevicesOut "LOOPBACK_CAPTURE_DEVICE:"

Write-Step "Quick Default"
$quickDefaultOut = Join-Path $BuildDir "$Config\\hardware_quick_default.txt"
Invoke-ConvergenceCommand {
  & $probeExe quick | Out-File -LiteralPath $quickDefaultOut -Encoding utf8
} "Hardware validation failed: default quick probe command failed."
Get-Content $quickDefaultOut | Select-Object -First 20
Require-Line $quickDefaultOut '^QuickSummary: success' "Hardware validation failed: default quick probe was not successful."
Require-Line $quickDefaultOut '^Result: success' "Hardware validation failed: default quick result was not success."

if ($loopbackCaptureId) {
  Write-Step "Quick Explicit Loopback Capture Device"
  $quickLoopbackOut = Join-Path $BuildDir "$Config\\hardware_quick_loopback_capture.txt"
  Invoke-ConvergenceCommand {
    & $probeExe quick "--source=loopback" "--capture-device-id=$loopbackCaptureId" |
      Out-File -LiteralPath $quickLoopbackOut -Encoding utf8
  } "Hardware validation failed: loopback capture-device quick probe command failed."
  Get-Content $quickLoopbackOut | Select-Object -First 28
  Require-Line $quickLoopbackOut '^QuickSummary: success' "Hardware validation failed: loopback capture-device quick probe was not successful."
  Require-Line $quickLoopbackOut '^Result: success' "Hardware validation failed: loopback capture-device quick result was not success."
  Require-Line $quickLoopbackOut ("^RequestedCaptureDeviceId: " + [regex]::Escape($loopbackCaptureId) + "$") "Hardware validation failed: loopback capture-device quick probe did not report the requested loopback capture device id."
  Require-Line $quickLoopbackOut '^RequestedCaptureMode: WASAPI Shared / Event$' "Hardware validation failed: loopback capture-device quick probe did not report the expected loopback capture mode."

  Write-Step "Quick Invalid Loopback Capture Device"
  $quickLoopbackInvalidOut = Join-Path $BuildDir "$Config\\hardware_quick_loopback_invalid_capture.txt"
  & $probeExe quick "--source=loopback" "--capture-device-id=not-a-loopback-render-endpoint" |
    Out-File -LiteralPath $quickLoopbackInvalidOut -Encoding utf8
  if ($LASTEXITCODE -ne 2) {
    throw "Hardware validation failed: invalid loopback capture-device quick probe did not fail with the expected exit code."
  }
  Get-Content $quickLoopbackInvalidOut | Select-Object -First 24
  Assert-QuickInvalidLoopbackCaptureDeviceSemantics $quickLoopbackInvalidOut "Hardware validation failed: invalid loopback capture-device quick probe"
}

if ($captureId) {
  Write-Step "Quick Explicit Capture Device"
  $quickCaptureOut = Join-Path $BuildDir "$Config\\hardware_quick_capture.txt"
  Invoke-ConvergenceCommand {
    & $probeExe quick "--capture-device-id=$captureId" | Out-File -LiteralPath $quickCaptureOut -Encoding utf8
  } "Hardware validation failed: capture-device quick probe command failed."
  Get-Content $quickCaptureOut | Select-Object -First 28
  Require-Line $quickCaptureOut '^QuickSummary: success' "Hardware validation failed: capture-device quick probe was not successful."
  Require-Line $quickCaptureOut ("^RequestedCaptureDeviceId: " + [regex]::Escape($captureId) + "$") "Hardware validation failed: capture-device quick probe did not report the requested capture device id."
}

if ($renderId) {
  Write-Step "Quick Explicit Render Device"
  $quickRenderOut = Join-Path $BuildDir "$Config\\hardware_quick_render.txt"
  Invoke-ConvergenceCommand {
    & $probeExe quick "--render-device-id=$renderId" | Out-File -LiteralPath $quickRenderOut -Encoding utf8
  } "Hardware validation failed: render-device quick probe command failed."
  Get-Content $quickRenderOut | Select-Object -First 28
  Require-Line $quickRenderOut '^QuickSummary: success' "Hardware validation failed: render-device quick probe was not successful."
  Require-Line $quickRenderOut ("^RequestedRenderDeviceId: " + [regex]::Escape($renderId) + "$") "Hardware validation failed: render-device quick probe did not report the requested render device id."

  Write-Step "Quick Invalid Render Device"
  $quickRenderInvalidOut = Join-Path $BuildDir "$Config\\hardware_quick_render_invalid.txt"
  & $probeExe quick "--render-device-id=definitely-invalid-render-device" |
    Out-File -LiteralPath $quickRenderInvalidOut -Encoding utf8
  if ($LASTEXITCODE -ne 2) {
    throw "Hardware validation failed: invalid render-device quick probe did not fail with the expected exit code."
  }
  Get-Content $quickRenderInvalidOut | Select-Object -First 24
  Assert-QuickInvalidRenderDeviceSemantics $quickRenderInvalidOut "Hardware validation failed: invalid render-device quick probe"
}

Write-Step "Quick Monitor Off Invalid Render Device"
$quickMonitorOffOut = Join-Path $BuildDir "$Config\\hardware_quick_monitor_off.txt"
Invoke-ConvergenceCommand {
  & $probeExe quick "--monitor=off" "--render-device-id=definitely-invalid-render-device" |
    Out-File -LiteralPath $quickMonitorOffOut -Encoding utf8
} "Hardware validation failed: monitor-off quick probe command failed."
Get-Content $quickMonitorOffOut | Select-Object -First 26
Require-Line $quickMonitorOffOut '^QuickSummary: success' "Hardware validation failed: monitor-off quick probe did not succeed."
Require-Line $quickMonitorOffOut 'monitor=off' "Hardware validation failed: monitor-off quick probe summary did not report monitor=off."
Require-Line $quickMonitorOffOut 'ren-fmt=disabled' "Hardware validation failed: monitor-off quick probe summary did not mark render format as disabled."
Require-Line $quickMonitorOffOut '^RequestedRenderDeviceId: definitely-invalid-render-device$' "Hardware validation failed: monitor-off quick probe did not preserve the requested invalid render device id."
Require-Line $quickMonitorOffOut '^NegotiatedRender: disabled$' "Hardware validation failed: monitor-off quick probe did not mark negotiated render as disabled."
Require-Line $quickMonitorOffOut '^RenderFormatMatch: disabled$' "Hardware validation failed: monitor-off quick probe did not mark render format match as disabled."
Require-Line $quickMonitorOffOut '^RenderRuntime: disabled$' "Hardware validation failed: monitor-off quick probe did not mark render runtime as disabled."
Require-Line $quickMonitorOffOut '^RenderWaveNote: render monitoring is disabled because monitor playback is off$' "Hardware validation failed: monitor-off quick probe did not explain disabled render monitoring clearly."
Require-Line $quickMonitorOffOut '^FailureStage: none$' "Hardware validation failed: monitor-off quick probe still reported a failure stage."

Write-Step "Quick Source-Mode Failure"
$quickSourceModeFailOut = Join-Path $BuildDir "$Config\\hardware_quick_source_mode_fail.txt"
& $probeExe quick "--capture-backend=wave" "--source=loopback" |
  Out-File -LiteralPath $quickSourceModeFailOut -Encoding utf8
if ($LASTEXITCODE -ne 2) {
  throw "Hardware validation failed: source-mode quick probe did not fail with the expected exit code."
}
Get-Content $quickSourceModeFailOut | Select-Object -First 26
Assert-QuickSourceModeFailureSemantics $quickSourceModeFailOut "Hardware validation failed: source-mode quick probe"

Write-Step "Matrix Loopback"
$matrixOut = Join-Path $BuildDir "$Config\\hardware_matrix_loopback.txt"
if ($loopbackCaptureId -and $renderId) {
  Invoke-ConvergenceCommand {
    & $probeExe matrix "--capture-device-id=$loopbackCaptureId" "--render-device-id=$renderId" --matrix-source=loopback |
      Out-File -LiteralPath $matrixOut -Encoding utf8
  } "Hardware validation failed: loopback matrix command with explicit device ids failed."
} else {
  Invoke-ConvergenceCommand {
    & $probeExe matrix --matrix-source=loopback | Out-File -LiteralPath $matrixOut -Encoding utf8
  } "Hardware validation failed: loopback matrix command failed."
}
Get-Content $matrixOut | Select-Object -First 30
if (Select-String -LiteralPath $matrixOut -Pattern 'Microphone \|' -Quiet) {
  throw "Hardware validation failed: loopback-scoped matrix still contains microphone rows."
}
Require-Line $matrixOut 'System Loopback \|' "Hardware validation failed: loopback-scoped matrix produced no loopback rows."
if ($loopbackCaptureId) {
  Require-Line $matrixOut ("cap-dev=" + [regex]::Escape($loopbackCaptureId)) "Hardware validation failed: matrix output did not report the requested loopback capture device id."
}
if ($renderId) {
  Require-Line $matrixOut ("ren-dev=" + [regex]::Escape($renderId)) "Hardware validation failed: matrix output did not report the requested render device id."
  Require-Line $matrixOut 'RENDER_DEVICE_FAIL=' "Hardware validation failed: explicit loopback matrix did not surface render-device failure accounting."
  Require-Line $matrixOut '^- MatrixHint: render-device failures are clustering\.' "Hardware validation failed: explicit loopback matrix did not surface the render-device clustering hint."
}

Write-Step "Matrix Loopback Wasapi Capture Wave Render"
$matrixWasapiWaveOut = Join-Path $BuildDir "$Config\\hardware_matrix_loopback_wasapi_wave.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-capture-backend=wasapi --matrix-render-backend=wave |
    Out-File -LiteralPath $matrixWasapiWaveOut -Encoding utf8
} "Hardware validation failed: focused WASAPI-capture / WAVE-render loopback matrix command failed."
Get-Content $matrixWasapiWaveOut | Select-Object -First 30
Require-Line $matrixWasapiWaveOut '^-\sMatrixSummary:' "Hardware validation failed: focused WASAPI-capture / WAVE-render loopback matrix produced no summary."
Require-Line $matrixWasapiWaveOut '^-\sBackendSummary: WASAPI -> WAVE API' "Hardware validation failed: focused WASAPI-capture / WAVE-render loopback matrix did not narrow to the expected backend pair."
Require-Line $matrixWasapiWaveOut 'UNSUPPORTED_MODE=16' "Hardware validation failed: focused WASAPI-capture / WAVE-render loopback matrix did not surface the expected unsupported-mode concentration."
Require-Line $matrixWasapiWaveOut '\[unsupported-mode\] \{WASAPI loopback requires shared mode\.\}' "Hardware validation failed: focused WASAPI-capture / WAVE-render loopback matrix did not preserve its unsupported-mode rows."

Write-Step "Matrix Loopback PCM16 Stereo"
$matrixProfileOut = Join-Path $BuildDir "$Config\\hardware_matrix_loopback_pcm16_stereo.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-profile=pcm16-48k-stereo |
    Out-File -LiteralPath $matrixProfileOut -Encoding utf8
} "Hardware validation failed: focused PCM16 stereo loopback matrix command failed."
Get-Content $matrixProfileOut | Select-Object -First 30
Require-Line $matrixProfileOut '^-\sProfileSummary: PCM16-48k-stereo' "Hardware validation failed: focused PCM16 stereo loopback matrix did not narrow to the expected profile."
if (Select-String -LiteralPath $matrixProfileOut -Pattern 'PCM24-44k-mono' -Quiet) {
  throw "Hardware validation failed: focused PCM16 stereo loopback matrix still contains the PCM24-44k-mono profile."
}

Write-Step "Matrix Loopback Align On"
$matrixAlignOnOut = Join-Path $BuildDir "$Config\\hardware_matrix_loopback_align_on.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-align=on |
    Out-File -LiteralPath $matrixAlignOnOut -Encoding utf8
} "Hardware validation failed: focused align-on loopback matrix command failed."
Get-Content $matrixAlignOnOut | Select-Object -First 30
Require-Line $matrixAlignOnOut '^-\sAlignSummary: on' "Hardware validation failed: focused align-on loopback matrix did not narrow to align=on."
if (Select-String -LiteralPath $matrixAlignOnOut -Pattern 'AlignSummary: off' -Quiet) {
  throw "Hardware validation failed: focused align-on loopback matrix still contains align=off."
}

Write-Step "Matrix Loopback Delay 0ms"
$matrixDelayOut = Join-Path $BuildDir "$Config\\hardware_matrix_loopback_delay_0ms.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-delay=0ms |
    Out-File -LiteralPath $matrixDelayOut -Encoding utf8
} "Hardware validation failed: focused delay-0ms loopback matrix command failed."
Get-Content $matrixDelayOut | Select-Object -First 30
Require-Line $matrixDelayOut '^-\sDelaySummary: 0ms' "Hardware validation failed: focused delay-0ms loopback matrix did not narrow to delay=0ms."
if (Select-String -LiteralPath $matrixDelayOut -Pattern 'DelaySummary: 120ms' -Quiet) {
  throw "Hardware validation failed: focused delay-0ms loopback matrix still contains delay=120ms."
}

Write-Step "Matrix Loopback Buffer cap40-ren40"
$matrixBufferOut = Join-Path $BuildDir "$Config\\hardware_matrix_loopback_buffer_cap40_ren40.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-buffer=cap40-ren40 |
    Out-File -LiteralPath $matrixBufferOut -Encoding utf8
} "Hardware validation failed: focused cap40-ren40 loopback matrix command failed."
Get-Content $matrixBufferOut | Select-Object -First 30
Require-Line $matrixBufferOut '^-\sBufferSummary: cap40-ren40' "Hardware validation failed: focused cap40-ren40 loopback matrix did not narrow to the expected buffer profile."
if (Select-String -LiteralPath $matrixBufferOut -Pattern 'BufferSummary: cap80-ren120' -Quiet) {
  throw "Hardware validation failed: focused cap40-ren40 loopback matrix still contains cap80-ren120."
}

Write-Step "Matrix Loopback Wave Render"
$matrixWaveOut = Join-Path $BuildDir "$Config\\hardware_matrix_loopback_wave_render.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-render-backend=wave |
    Out-File -LiteralPath $matrixWaveOut -Encoding utf8
} "Hardware validation failed: focused wave-render loopback matrix command failed."
Get-Content $matrixWaveOut | Select-Object -First 30
Require-Line $matrixWaveOut '^-\sBackendSummary: WASAPI -> WAVE API' "Hardware validation failed: focused wave-render loopback matrix did not include WASAPI -> WAVE API."
Require-Line $matrixWaveOut '^-\sBackendSummary: WAVE API -> WAVE API' "Hardware validation failed: focused wave-render loopback matrix did not include WAVE API -> WAVE API."
if (Select-String -LiteralPath $matrixWaveOut -Pattern 'BackendSummary: WAVE API -> WASAPI|BackendSummary: WASAPI -> WASAPI' -Quiet) {
  throw "Hardware validation failed: focused wave-render loopback matrix still contains non-wave render backend summaries."
}

Write-Step "Matrix Loopback Shared Only"
$matrixSharedOut = Join-Path $BuildDir "$Config\\hardware_matrix_loopback_shared_only.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-wasapi-share=shared |
    Out-File -LiteralPath $matrixSharedOut -Encoding utf8
} "Hardware validation failed: focused shared-only loopback matrix command failed."
Get-Content $matrixSharedOut | Select-Object -First 30
Require-Line $matrixSharedOut '^-\sMatrixSummary:' "Hardware validation failed: focused shared-only loopback matrix produced no summary."
Require-Line $matrixSharedOut 'UNSUPPORTED_MODE=0' "Hardware validation failed: focused shared-only loopback matrix did not eliminate unsupported-mode noise."
if (Select-String -LiteralPath $matrixSharedOut -Pattern '\[unsupported-mode\]' -Quiet) {
  throw "Hardware validation failed: focused shared-only loopback matrix still contains unsupported-mode rows."
}

if ($transcriptStarted) {
  Stop-Transcript | Out-Null
}
