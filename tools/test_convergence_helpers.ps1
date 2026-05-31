param(
  [string]$BuildDir
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = Join-Path $root "build"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
  $BuildDir = Join-Path $root $BuildDir
}

$tempDir = Join-Path $BuildDir "convergence-helper-test-artifacts"
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null

. (Join-Path $PSScriptRoot "convergence_helpers.ps1")

$tmp = Join-Path $tempDir "convergence_helper_test.txt"
@"
- MatrixSummary: PASS=1
- SourceSummary: A
  detail line should not match
- BufferSummary: B
"@ | Out-File -LiteralPath $tmp -Encoding utf8

$lines = Select-ConvergenceSummaryLines $tmp @('^- MatrixSummary:', '^- SourceSummary:', '^- BufferSummary:')

if ($lines.Count -ne 3) {
  throw "Expected 3 summary lines, got $($lines.Count)"
}
if ($lines[0] -notmatch 'MatrixSummary' -or
    $lines[1] -notmatch 'SourceSummary' -or
    $lines[2] -notmatch 'BufferSummary') {
  throw "Summary line filtering returned unexpected output"
}

$deviceTmp = Join-Path $tempDir "convergence_helper_device_test.txt"
@"
CAPTURE_DEVICE: "cap-a" | Capture | Shared
CAPTURE_DEVICE: "cap-default" | Capture | Default | Shared
RENDER_DEVICE: "ren-a" | Render | Shared
"@ | Out-File -LiteralPath $deviceTmp -Encoding utf8

$captureId = Extract-PreferredIdFromFile $deviceTmp "CAPTURE_DEVICE:"
$renderId = Extract-PreferredIdFromFile $deviceTmp "RENDER_DEVICE:"

if ($captureId -ne "cap-default") {
  throw "Expected preferred capture id to select the Default device, got $captureId"
}
if ($renderId -ne "ren-a") {
  throw "Expected preferred render id to fall back to the first listed device, got $renderId"
}

$sourceModeFailTmp = Join-Path $tempDir "convergence_helper_source_mode_fail_test.txt"
@"
QuickSummary: failed | dump=none | cap-fmt=not-negotiated | ren-fmt=not-negotiated | mode=not-started | monitor=on | cap-wave=not-started | ren-wave=not-started | ren-updates=0
NegotiatedCapture: not-negotiated
NegotiatedRender: not-negotiated
CaptureFormatMatch: not-negotiated
RenderFormatMatch: not-negotiated
Waveform: not-started
CaptureWave: not-started
RenderWave: not-started
CaptureMode: not-started
RenderMode: not-started
Resampler: not-started
CaptureRuntime: not-started
RenderRuntime: not-started
FailureStage: source-mode
"@ | Out-File -LiteralPath $sourceModeFailTmp -Encoding utf8

Assert-QuickSourceModeFailureSemantics $sourceModeFailTmp "helper test"

$invalidRenderTmp = Join-Path $tempDir "convergence_helper_invalid_render_test.txt"
@"
FailureStage: render-device
RequestedRenderDeviceId: definitely-invalid-render-device
FailureReason: Selected render device is not available for this backend. Choose a device from devices for the same render backend, or omit --render-device-id.
"@ | Out-File -LiteralPath $invalidRenderTmp -Encoding utf8
Assert-QuickInvalidRenderDeviceSemantics $invalidRenderTmp "helper test"

$invalidLoopbackCaptureTmp = Join-Path $tempDir "convergence_helper_invalid_loopback_capture_test.txt"
@"
FailureStage: source-mode
RequestedCaptureDeviceId: not-a-loopback-render-endpoint
FailureReason: Selected loopback capture device is not available for this source. Use devices --source=loopback to choose a render-backed loopback endpoint.
"@ | Out-File -LiteralPath $invalidLoopbackCaptureTmp -Encoding utf8
Assert-QuickInvalidLoopbackCaptureDeviceSemantics $invalidLoopbackCaptureTmp "helper test"

$matrixDelayTmp = Join-Path $tempDir "convergence_helper_matrix_delay_test.txt"
@"
- DelaySummary: 0ms
"@ | Out-File -LiteralPath $matrixDelayTmp -Encoding utf8
Assert-MatrixDelayScopeSemantics $matrixDelayTmp "0ms" "helper test"

$matrixBufferTmp = Join-Path $tempDir "convergence_helper_matrix_buffer_test.txt"
@"
- BufferSummary: cap40-ren40
"@ | Out-File -LiteralPath $matrixBufferTmp -Encoding utf8
Assert-MatrixBufferScopeSemantics $matrixBufferTmp "cap40-ren40" "helper test"

$matrixAlignTmp = Join-Path $tempDir "convergence_helper_matrix_align_test.txt"
@"
- AlignSummary: on
"@ | Out-File -LiteralPath $matrixAlignTmp -Encoding utf8
Assert-MatrixAlignScopeSemantics $matrixAlignTmp "on" "helper test"

$matrixSharedTmp = Join-Path $tempDir "convergence_helper_matrix_shared_test.txt"
@"
- MatrixSummary: PASS=1 UNSUPPORTED_MODE=0
"@ | Out-File -LiteralPath $matrixSharedTmp -Encoding utf8
Assert-MatrixSharedOnlySemantics $matrixSharedTmp "helper test"

$matrixExplicitLoopbackTmp = Join-Path $tempDir "convergence_helper_matrix_explicit_loopback_test.txt"
@"
System Loopback | align=off | delay=0ms | buf=cap40-ren40 | profile=PCM16-48k-stereo | WASAPI -> WASAPI: PASS | cap-dev=loop-cap | ren-dev=loop-ren
- MatrixSummary: PASS=1 RENDER_DEVICE_FAIL=1
- MatrixHint: render-device failures are clustering.
"@ | Out-File -LiteralPath $matrixExplicitLoopbackTmp -Encoding utf8
Assert-ExplicitLoopbackMatrixSemantics $matrixExplicitLoopbackTmp "loop-cap" "loop-ren" "helper test"

$threw = $false
try {
  Invoke-ConvergenceCommand { cmd /c exit 7 } "intentional"
} catch {
  $threw = $true
}
if (-not $threw) {
  throw "Invoke-ConvergenceCommand did not fail on non-zero exit code"
}

Write-Host "ALL_TESTS_PASSED"
