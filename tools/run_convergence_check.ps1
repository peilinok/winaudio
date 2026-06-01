param(
  [string]$Config = "Debug",
  [string]$BuildDir,
  [switch]$SkipBuild,
  [switch]$GuiSmokeOnly,
  [switch]$SuppressBuildStepOutput
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
$guiExe = Join-Path $buildDir "$Config\\winaudio.exe"
$buildWrapper = Join-Path $PSScriptRoot "invoke_msbuild_safe.ps1"
$envInspector = Join-Path $PSScriptRoot "inspect_build_environment.ps1"
. (Join-Path $PSScriptRoot "convergence_helpers.ps1")

function Write-Step($label) {
  Write-Host "`n=== $label ==="
}

function Assert-ConvergenceArtifactExists {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$FailureMessage
  )

  if (-not (Test-Path -LiteralPath $Path)) {
    throw $FailureMessage
  }
}

if (-not $SkipBuild -and -not $GuiSmokeOnly) {
  Write-Step "Build Environment"
  Invoke-ConvergenceCommand {
    powershell -ExecutionPolicy Bypass -File $envInspector
  } "Build environment inspection failed"
  Invoke-ConvergenceCommand {
    powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "test_build_environment_tools.ps1") -BuildDir $BuildDir
  } "Build environment self-test failed"
  Invoke-ConvergenceCommand {
    powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "test_convergence_helpers.ps1") -BuildDir $BuildDir
  } "Convergence helper self-test failed"
}

if (-not $SkipBuild) {
  Write-Step "Build"
  if ($GuiSmokeOnly) {
    Invoke-ConvergenceCommand {
      powershell -ExecutionPolicy Bypass -File $buildWrapper -PrintEnvironmentSummary cmake --build $BuildDir --config $Config --target winaudio
    } "Build step failed"
  } else {
    Invoke-ConvergenceCommand {
      powershell -ExecutionPolicy Bypass -File $buildWrapper -PrintEnvironmentSummary cmake --build $BuildDir --config $Config --target winaudio winaudio_probe app_model_text_test probe_cli_test probe_ui_text_test
    } "Build step failed"
  }
} else {
  if (-not $SuppressBuildStepOutput) {
    Write-Step "Build"
    Write-Host "Skipping build; using existing artifacts from $BuildDir"
  }
}

if ($GuiSmokeOnly) {
  Assert-ConvergenceArtifactExists $guiExe "GUI smoke failed: GUI binary not found at $guiExe."
} else {
  Assert-ConvergenceArtifactExists $probeExe "Convergence failed: CLI binary not found at $probeExe."
  Assert-ConvergenceArtifactExists $guiExe "Convergence failed: GUI binary not found at $guiExe."
}

if (-not $GuiSmokeOnly) {
Write-Step "CLI Help"
Invoke-ConvergenceCommand { & $probeExe --help } "CLI help failed"

Write-Step "Devices"
$devicesOut = Join-Path $buildDir "$Config\\devices_convergence.txt"
Invoke-ConvergenceCommand { & $probeExe devices | Out-File -LiteralPath $devicesOut -Encoding utf8 } "Devices command failed"
Get-Content $devicesOut | Select-Object -First 20
$renderId = Extract-PreferredIdFromFile $devicesOut "RENDER_DEVICE:"

Write-Step "Loopback Devices"
$loopbackDevicesOut = Join-Path $buildDir "$Config\\devices_loopback_convergence.txt"
Invoke-ConvergenceCommand { & $probeExe devices --source=loopback | Out-File -LiteralPath $loopbackDevicesOut -Encoding utf8 } "Loopback devices command failed"
Get-Content $loopbackDevicesOut | Select-Object -First 18
$loopbackCaptureId = Extract-PreferredIdFromFile $loopbackDevicesOut "LOOPBACK_CAPTURE_DEVICE:"

Write-Step "Quick"
$quickOut = Join-Path $buildDir "$Config\\quick_convergence.txt"
Invoke-ConvergenceCommand { & $probeExe quick | Out-File -LiteralPath $quickOut -Encoding utf8 } "Quick probe failed"
Select-ConvergenceSummaryLines $quickOut @('^QuickSummary:', '^RequestedCaptureDeviceId:', '^RequestedRenderDeviceId:', '^RequestedCapture:', '^RequestedRender:', '^NegotiatedCapture:', '^NegotiatedRender:', '^CaptureFormatMatch:', '^RenderFormatMatch:')

Write-Step "Quick Monitor Off"
$quickMonitorOffOut = Join-Path $buildDir "$Config\\quick_monitor_off_convergence.txt"
Invoke-ConvergenceCommand {
  & $probeExe quick "--monitor=off" "--render-device-id=definitely-invalid-render-device" |
    Out-File -LiteralPath $quickMonitorOffOut -Encoding utf8
} "Quick monitor-off probe failed"
Select-ConvergenceSummaryLines $quickMonitorOffOut @('^QuickSummary:', '^RequestedRenderDeviceId:', '^NegotiatedRender:', '^RenderFormatMatch:', '^FailureStage:')

Write-Step "Quick Source-Mode Failure"
$quickSourceModeFailOut = Join-Path $buildDir "$Config\\quick_source_mode_fail_convergence.txt"
& $probeExe quick "--capture-backend=wave" "--source=loopback" |
  Out-File -LiteralPath $quickSourceModeFailOut -Encoding utf8
if ($LASTEXITCODE -ne 2) {
  throw "Quick source-mode failure probe did not fail with the expected exit code."
}
Select-ConvergenceSummaryLines $quickSourceModeFailOut @('^QuickSummary:', '^NegotiatedCapture:', '^NegotiatedRender:', '^CaptureFormatMatch:', '^RenderFormatMatch:', '^CaptureMode:', '^RenderMode:', '^FailureStage:')
Assert-QuickSourceModeFailureSemantics $quickSourceModeFailOut "Quick source-mode failure probe"

Write-Step "CLI Integration"
Invoke-ConvergenceCommand {
  powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "test_cli_integration.ps1") -Config $Config -BuildDir $BuildDir -SkipBuild
} "CLI integration verification failed"

Write-Step "Matrix Loopback"
$matrixOut = Join-Path $buildDir "$Config\\matrix_loopback_convergence.txt"
Invoke-ConvergenceCommand { & $probeExe matrix --matrix-source=loopback | Out-File -LiteralPath $matrixOut -Encoding utf8 } "Loopback matrix failed"
Select-ConvergenceSummaryLines $matrixOut @('^- MatrixSummary:', '^- MatrixHint:', '^- BackendSummary:', '^- PairSummary:', '^- ProfileSummary:', '^- SourceSummary:', '^- AlignSummary:', '^- DelaySummary:', '^- BufferSummary:')

Write-Step "Matrix Loopback Wave Render"
$matrixWaveOut = Join-Path $buildDir "$Config\\matrix_loopback_wave_render_convergence.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-render-backend=wave |
    Out-File -LiteralPath $matrixWaveOut -Encoding utf8
} "Loopback wave-render matrix failed"
Select-ConvergenceSummaryLines $matrixWaveOut @('^- MatrixSummary:', '^- MatrixHint:', '^- BackendSummary:', '^- PairSummary:', '^- ProfileSummary:', '^- SourceSummary:', '^- AlignSummary:', '^- DelaySummary:', '^- BufferSummary:')

Write-Step "Matrix Loopback Wasapi Capture Wave Render"
$matrixWasapiWaveOut = Join-Path $buildDir "$Config\\matrix_loopback_wasapi_capture_wave_render_convergence.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-capture-backend=wasapi --matrix-render-backend=wave |
    Out-File -LiteralPath $matrixWasapiWaveOut -Encoding utf8
} "Loopback wasapi-capture wave-render matrix failed"
Select-ConvergenceSummaryLines $matrixWasapiWaveOut @('^- MatrixSummary:', '^- MatrixHint:', '^- BackendSummary:', '^- PairSummary:', '^- ProfileSummary:', '^- SourceSummary:', '^- AlignSummary:', '^- DelaySummary:', '^- BufferSummary:')

Write-Step "Matrix Loopback Align On"
$matrixAlignOnOut = Join-Path $buildDir "$Config\\matrix_loopback_align_on_convergence.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-align=on |
    Out-File -LiteralPath $matrixAlignOnOut -Encoding utf8
} "Loopback align-on matrix failed"
Select-ConvergenceSummaryLines $matrixAlignOnOut @('^- MatrixSummary:', '^- MatrixHint:', '^- BackendSummary:', '^- PairSummary:', '^- ProfileSummary:', '^- SourceSummary:', '^- AlignSummary:', '^- DelaySummary:', '^- BufferSummary:')

Write-Step "Matrix Loopback Delay 0ms"
$matrixDelayOut = Join-Path $buildDir "$Config\\matrix_loopback_delay_0ms_convergence.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-delay=0ms |
    Out-File -LiteralPath $matrixDelayOut -Encoding utf8
} "Loopback delay-0ms matrix failed"
Select-ConvergenceSummaryLines $matrixDelayOut @('^- MatrixSummary:', '^- MatrixHint:', '^- BackendSummary:', '^- PairSummary:', '^- ProfileSummary:', '^- SourceSummary:', '^- AlignSummary:', '^- DelaySummary:', '^- BufferSummary:')

Write-Step "Matrix Loopback Buffer cap40-ren40"
$matrixBufferOut = Join-Path $buildDir "$Config\\matrix_loopback_buffer_cap40_ren40_convergence.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-buffer=cap40-ren40 |
    Out-File -LiteralPath $matrixBufferOut -Encoding utf8
} "Loopback cap40-ren40 matrix failed"
Select-ConvergenceSummaryLines $matrixBufferOut @('^- MatrixSummary:', '^- MatrixHint:', '^- BackendSummary:', '^- PairSummary:', '^- ProfileSummary:', '^- SourceSummary:', '^- AlignSummary:', '^- DelaySummary:', '^- BufferSummary:')

Write-Step "Matrix Loopback PCM16 Stereo"
$matrixProfileOut = Join-Path $buildDir "$Config\\matrix_loopback_pcm16_stereo_convergence.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-profile=pcm16-48k-stereo |
    Out-File -LiteralPath $matrixProfileOut -Encoding utf8
} "Loopback PCM16 stereo matrix failed"
Select-ConvergenceSummaryLines $matrixProfileOut @('^- MatrixSummary:', '^- MatrixHint:', '^- BackendSummary:', '^- PairSummary:', '^- ProfileSummary:', '^- SourceSummary:', '^- AlignSummary:', '^- DelaySummary:', '^- BufferSummary:')

Write-Step "Matrix Loopback Shared Only"
$matrixSharedOut = Join-Path $buildDir "$Config\\matrix_loopback_shared_only_convergence.txt"
Invoke-ConvergenceCommand {
  & $probeExe matrix --matrix-source=loopback --matrix-wasapi-share=shared |
    Out-File -LiteralPath $matrixSharedOut -Encoding utf8
} "Loopback shared-only matrix failed"
Select-ConvergenceSummaryLines $matrixSharedOut @('^- MatrixSummary:', '^- MatrixHint:', '^- BackendSummary:', '^- PairSummary:', '^- ProfileSummary:', '^- SourceSummary:', '^- AlignSummary:', '^- DelaySummary:', '^- BufferSummary:')

if ($loopbackCaptureId -and $renderId) {
  Write-Step "Matrix Loopback Explicit Devices"
  $matrixExplicitOut = Join-Path $buildDir "$Config\\matrix_loopback_explicit_convergence.txt"
  Invoke-ConvergenceCommand {
    & $probeExe matrix "--capture-device-id=$loopbackCaptureId" "--render-device-id=$renderId" --matrix-source=loopback |
      Out-File -LiteralPath $matrixExplicitOut -Encoding utf8
  } "Explicit-device loopback matrix failed"
  Select-ConvergenceSummaryLines $matrixExplicitOut @('^- MatrixSummary:', '^- MatrixHint:', '^- BackendSummary:', '^- PairSummary:', '^- ProfileSummary:', '^- SourceSummary:', '^- AlignSummary:', '^- DelaySummary:', '^- BufferSummary:')
}

Invoke-ConvergenceCommand {
  powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run_gui_smoke.ps1") -Config $Config -BuildDir $BuildDir -SkipBuild
} "GUI smoke failed"

Write-Step "CTest"
Invoke-ConvergenceCommand { ctest --test-dir $BuildDir -C $Config --output-on-failure } "CTest failed"
return
}

Write-Step "GUI Smoke"
Get-Process winaudio -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
$sig=@'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Native {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);
  [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hDlg, int nIDDlgItem);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll")] public static extern bool SendNotifyMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr hWnd, uint Msg, IntPtr wParam, StringBuilder lParam);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
}
'@
Add-Type $sig
function Get-Text([IntPtr]$h){
  if ($h -eq [IntPtr]::Zero) {
    return ""
  }
  $len=[Native]::GetWindowTextLength($h)
  $sb=New-Object System.Text.StringBuilder ($len+8)
  [void][Native]::GetWindowText($h,$sb,$sb.Capacity)
  $sb.ToString()
}
function Get-ControlTextById([IntPtr]$hwnd, [int]$controlId) {
  $handle = [Native]::GetDlgItem($hwnd, $controlId)
  if ($handle -eq [IntPtr]::Zero) {
    return ""
  }
  Get-Text $handle
}
function Get-ControlHandle([IntPtr]$hwnd, [int]$controlId) {
  if ($hwnd -eq [IntPtr]::Zero) {
    return [IntPtr]::Zero
  }
  [Native]::GetDlgItem($hwnd, $controlId)
}
function Get-ControlEnabledSafe([IntPtr]$handle) {
  if ($handle -eq [IntPtr]::Zero) {
    return $false
  }
  [Native]::IsWindowEnabled($handle)
}
function Get-ControlTextSafe([IntPtr]$handle) {
  if ($handle -eq [IntPtr]::Zero) {
    return ""
  }
  Get-Text $handle
}
function Wait-ForWindowHandle([System.Diagnostics.Process]$process, [int]$maxPolls = 100) {
  $hwnd = [IntPtr]::Zero
  for ($i = 0; $i -lt $maxPolls -and $hwnd -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 100
    try {
      $proc = Get-Process -Id $process.Id -ErrorAction Stop
      if ($proc.MainWindowHandle -ne 0) {
        $hwnd = [IntPtr]$proc.MainWindowHandle
        break
      }
    } catch {}
    $candidate = [Native]::FindWindow('WinAudioDemoWindowClass', $null)
    if ($candidate -ne [IntPtr]::Zero) {
      [uint32]$ownerPid = 0
      [void][Native]::GetWindowThreadProcessId($candidate, [ref]$ownerPid)
      if ($ownerPid -eq $process.Id) {
        $hwnd = $candidate
      }
    }
  }
  $hwnd
}
function Wait-ForProcessExit([System.Diagnostics.Process]$process, [int]$maxPolls = 300) {
  if ($null -eq $process) {
    return $true
  }
  $exited = $process.WaitForExit($maxPolls * 100)
  if (-not $exited) {
    for ($i = 0; $i -lt $maxPolls; $i++) {
      Start-Sleep -Milliseconds 100
      try {
        Get-Process -Id $process.Id -ErrorAction Stop | Out-Null
      } catch {
        $exited = $true
        break
      }
    }
  }
  if (-not $exited) {
    Stop-ProcessByIdIfRunning $process.Id
    for ($i = 0; $i -lt [Math]::Min($maxPolls, 20); $i++) {
      Start-Sleep -Milliseconds 100
      try {
        Get-Process -Id $process.Id -ErrorAction Stop | Out-Null
      } catch {
        $exited = $true
        break
      }
    }
  }
  $exited
}
function Wait-ForRunningEvidence([IntPtr]$hwnd, [IntPtr]$startButton, [IntPtr]$stopButton, [IntPtr]$probeButton, [int]$maxPolls = 300) {
  $runningSeen = $false
  $runningObservedEver = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $title = Get-Text $hwnd
    $probeButtonText = Get-ControlTextSafe $probeButton
    $probeDisabled = -not (Get-ControlEnabledSafe $probeButton)
    if ($title -like "*| Running |*") {
      $runningObservedEver = $true
    }
    if ((($title -like "*| Running |*") -or
         ($probeButtonText -eq "Run Quick Probe") -or
         ($probeButtonText -eq "Run Probe Matrix") -or
         $probeDisabled) -and
        (-not (Get-ControlEnabledSafe $startButton)) -and
        (Get-ControlEnabledSafe $stopButton)) {
      $runningSeen = $true
      break
    }
  }
  [pscustomobject]@{
    RunningSeen = $runningSeen
    RunningObservedEver = $runningObservedEver
  }
}
function Wait-ForSessionRunningState([IntPtr]$hwnd, [IntPtr]$startButton, [IntPtr]$stopButton, [scriptblock]$additionalCheck = $null, [int]$maxPolls = 120, [int]$stableSamples = 2) {
  $runningSeen = $false
  $runningObservedEver = $false
  $stableRunningSamples = 0
  $lastTitle = ""
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $lastTitle = Get-Text $hwnd
    if ($lastTitle -like "*| Running |*") {
      $runningObservedEver = $true
    }
    $extraSatisfied = $true
    if ($null -ne $additionalCheck) {
      $extraSatisfied = [bool](& $additionalCheck)
    }
    if ((-not (Get-ControlEnabledSafe $startButton)) -and
        (Get-ControlEnabledSafe $stopButton) -and
        $extraSatisfied) {
      $stableRunningSamples += 1
      if ($stableRunningSamples -ge $stableSamples) {
        $runningSeen = $true
        break
      }
    } else {
      $stableRunningSamples = 0
    }
  }
  [pscustomobject]@{
    RunningSeen = $runningSeen
    RunningObservedEver = $runningObservedEver
    Title = $lastTitle
  }
}
function Start-SessionAndWait([IntPtr]$hwnd, [IntPtr]$startButton, [IntPtr]$stopButton, [int]$startButtonId, [scriptblock]$additionalCheck = $null, [int]$maxPolls = 120, [int]$stableSamples = 2) {
  $started = $false
  if (Get-ControlEnabledSafe $startButton) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }
  $runningState = Wait-ForSessionRunningState $hwnd $startButton $stopButton $additionalCheck $maxPolls $stableSamples
  [pscustomobject]@{
    Started = $started
    RunningSeen = $runningState.RunningSeen
    RunningObservedEver = $runningState.RunningObservedEver
    Title = $runningState.Title
  }
}
function Wait-ForIdleAfterStop([IntPtr]$hwnd, [IntPtr]$startButton, [IntPtr]$stopButton, [int]$maxPolls = 60) {
  $stableIdleSamples = 0
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $title = Get-Text $hwnd
    $notRunningTitle =
      $title -notlike "*| Running |*" -and
      $title -notlike "*Quick Probe Running*" -and
      $title -notlike "*Probe Matrix Running*"
    $startEnabled = Get-ControlEnabledSafe $startButton
    $stopEnabled = Get-ControlEnabledSafe $stopButton
    $idleSurfaceSeen =
      $startEnabled -and
      (-not $stopEnabled) -and
      ($notRunningTitle -or
       [string]::IsNullOrWhiteSpace($title) -or
       $title -like "*WinAudio Demo*")
    if ($idleSurfaceSeen) {
      $stableIdleSamples += 1
      if ($stableIdleSamples -ge 3) {
        return $true
      }
    } else {
      $stableIdleSamples = 0
    }
  }
  return $false
}
function Stop-SessionAndWait([IntPtr]$hwnd, [IntPtr]$startButton, [IntPtr]$stopButton, [int]$stopButtonId, [int]$maxPolls = 60) {
  $attempts = @(
    { Invoke-ButtonClick $stopButton },
    { [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$stopButtonId, [IntPtr]::Zero) },
    { Invoke-ButtonClick $stopButton }
  )

  foreach ($attempt in $attempts) {
    & $attempt
    if (Wait-ForIdleAfterStop $hwnd $startButton $stopButton $maxPolls) {
      return $true
    }
  }

  return $false
}
function Set-ControlText([IntPtr]$handle, [string]$text) {
  $wmSetText = 0x000C
  $buffer = New-Object System.Text.StringBuilder $text
  [void][Native]::SendMessage($handle, $wmSetText, [IntPtr]::Zero, $buffer)
}
function Invoke-EditTextChange([IntPtr]$hwnd, [int]$editId, [string]$text) {
  $enChange = 0x0300
  $edit = [Native]::GetDlgItem($hwnd, $editId)
  if ($edit -eq [IntPtr]::Zero) {
    return
  }
  Set-ControlText $edit $text
  $wParamValue = $editId -bor ($enChange -shl 16)
  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$wParamValue, $edit)
}
function Stop-WindowProcessIfRunning([System.Diagnostics.Process]$process) {
  if ($null -eq $process) {
    return
  }
  try {
    if (-not $process.HasExited) {
      Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
  } catch {}
}
function Stop-ProcessByIdIfRunning([int]$processId) {
  try {
    $proc = Get-Process -Id $processId -ErrorAction Stop
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
  } catch {}
}
function Format-StateMap([hashtable]$map) {
  ($map.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ", "
}
function Test-AllStatesTrue([hashtable]$map) {
  foreach ($value in $map.Values) {
    if (-not $value) {
      return $false
    }
  }
  return $true
}
function Get-ComboSelectionText([IntPtr]$handle) {
  $cbGetCurSel = 0x0147
  $cbGetLbText = 0x0148
  $cbErr = -1
  $index = [int][Native]::SendMessage($handle, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($index -eq $cbErr) {
    return ""
  }
  $buffer = New-Object System.Text.StringBuilder 512
  [void][Native]::SendMessage($handle, $cbGetLbText, [IntPtr]$index, $buffer)
  $buffer.ToString()
}
function Get-ComboItemText([IntPtr]$handle, [int]$index) {
  $cbGetLbText = 0x0148
  $buffer = New-Object System.Text.StringBuilder 512
  [void][Native]::SendMessage($handle, $cbGetLbText, [IntPtr]$index, $buffer)
  $buffer.ToString()
}
function Get-ComboCount([IntPtr]$handle) {
  $cbGetCount = 0x0146
  [int][Native]::SendMessage($handle, $cbGetCount, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Get-ComboItems([IntPtr]$handle) {
  $count = Get-ComboCount $handle
  $items = New-Object System.Collections.Generic.List[string]
  for ($i = 0; $i -lt $count; $i++) {
    $items.Add((Get-ComboItemText $handle $i))
  }
  ,$items.ToArray()
}
function Test-StringArrayEquality([string[]]$left, [string[]]$right) {
  if ($left.Count -ne $right.Count) {
    return $false
  }
  for ($i = 0; $i -lt $left.Count; $i++) {
    if ($left[$i] -ne $right[$i]) {
      return $false
    }
  }
  return $true
}
function Get-CheckState([IntPtr]$handle) {
  $bmGetCheck = 0x00F0
  [int][Native]::SendMessage($handle, $bmGetCheck, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Invoke-CheckboxClick([IntPtr]$handle) {
  $bmClick = 0x00F5
  [void][Native]::SendMessage($handle, $bmClick, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Invoke-ButtonClick([IntPtr]$handle) {
  if ($handle -eq [IntPtr]::Zero) {
    return
  }
  $bmClick = 0x00F5
  [void][Native]::SendMessage($handle, $bmClick, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Invoke-ComboSelectionChange([IntPtr]$hwnd, [int]$comboId, [int]$index) {
  $cbSetCurSel = 0x014E
  $cbnSelChange = 1
  $combo = [Native]::GetDlgItem($hwnd, $comboId)
  [void][Native]::SendMessage($combo, $cbSetCurSel, [IntPtr]$index, [IntPtr]::Zero)
  $wParamValue = $comboId -bor ($cbnSelChange -shl 16)
  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$wParamValue, $combo)
}
function Invoke-ComboSelectionChangeSync([IntPtr]$hwnd, [int]$comboId, [int]$index) {
  $cbSetCurSel = 0x014E
  $cbnSelChange = 1
  $combo = [Native]::GetDlgItem($hwnd, $comboId)
  [void][Native]::SendMessage($combo, $cbSetCurSel, [IntPtr]$index, [IntPtr]::Zero)
  $wParamValue = $comboId -bor ($cbnSelChange -shl 16)
  [void][Native]::SendMessage($hwnd, 0x0111, [IntPtr]$wParamValue, $combo)
}
function Wait-ForProbeTextContains([IntPtr]$hwnd, [string[]]$needles, [int]$maxPolls = 120) {
  $automationProbeTextId = 1905
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $probeText = Get-ControlTextById $hwnd $automationProbeTextId
    if ([string]::IsNullOrEmpty($probeText)) {
      continue
    }
    $allPresent = $true
    foreach ($needle in $needles) {
      if ($probeText.IndexOf($needle, [System.StringComparison]::Ordinal) -lt 0) {
        $allPresent = $false
        break
      }
    }
    if ($allPresent) {
      return [pscustomobject]@{
        Seen = $true
        ProbeText = $probeText
      }
    }
  }
  [pscustomobject]@{
    Seen = $false
    ProbeText = Get-ControlTextById $hwnd $automationProbeTextId
  }
}
function Wait-ForComboSelection([IntPtr]$hwnd, [int]$comboId, [string]$expectedText, [int]$maxPolls = 80) {
  $combo = [Native]::GetDlgItem($hwnd, $comboId)
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((Get-ComboSelectionText $combo) -eq $expectedText) {
      return $true
    }
  }
  return $false
}
function Find-ComboItemIndex([IntPtr]$handle, [string]$expectedText) {
  $items = Get-ComboItems $handle
  for ($i = 0; $i -lt $items.Count; $i++) {
    if ($items[$i] -eq $expectedText) {
      return $i
    }
  }
  return -1
}
function Wait-ForControlTextContains([IntPtr]$hwnd, [int]$controlId, [string[]]$needles, [int]$maxPolls = 120) {
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $text = Get-ControlTextById $hwnd $controlId
    if ([string]::IsNullOrEmpty($text)) {
      continue
    }
    $allPresent = $true
    foreach ($needle in $needles) {
      if ($text.IndexOf($needle, [System.StringComparison]::Ordinal) -lt 0) {
        $allPresent = $false
        break
      }
    }
    if ($allPresent) {
      return [pscustomobject]@{
        Seen = $true
        Text = $text
      }
    }
  }
  [pscustomobject]@{
    Seen = $false
    Text = Get-ControlTextById $hwnd $controlId
  }
}
function Wait-ForControlTextEquals([IntPtr]$hwnd, [int]$controlId, [string]$expectedText, [int]$maxPolls = 120) {
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $text = Get-ControlTextById $hwnd $controlId
    if ($text -eq $expectedText) {
      return [pscustomobject]@{
        Seen = $true
        Text = $text
      }
    }
  }
  [pscustomobject]@{
    Seen = $false
    Text = Get-ControlTextById $hwnd $controlId
  }
}
function Invoke-ApplicationLoopbackGuiCheck([IntPtr]$hwnd, [int]$sourceComboId, [int]$captureComboId, [int]$appLoopbackEditId, [int]$probeButtonId, [int]$maxPolls = 120) {
  $sourceCombo = [Native]::GetDlgItem($hwnd, $sourceComboId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $appLoopbackEdit = [Native]::GetDlgItem($hwnd, $appLoopbackEditId)
  $automationCaptureLabelId = 1901
  $automationDeviceCountLineId = 1902
  $automationSummaryTextId = 1903
  $automationProbeTextId = 1905

  $baselineSourceIndex = [int][Native]::SendMessage($sourceCombo, 0x0147, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineSourceText = Get-ComboItemText $sourceCombo $baselineSourceIndex
  $baselineCaptureEnabled = [Native]::IsWindowEnabled($captureCombo)
  $baselineAppTargetText = Get-Text $appLoopbackEdit

  Invoke-ComboSelectionChange $hwnd $sourceComboId 2
  Invoke-EditTextChange $hwnd $appLoopbackEditId "1234"

  $surfaceSeen = $false
  $sourceSeen = $false
  $labelSeen = $false
  $deviceCountSeen = $false
  $targetSeen = $false
  $requestedCaptureIdSeen = $false
  $quickFailureSeen = $false
  $summarySeen = $false
  $probeText = ""
  $summaryText = ""
  $lastSourceText = ""
  $lastCaptureLabel = ""
  $lastDeviceCountLine = ""
  $lastAppTargetText = ""
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $sourceText = Get-ComboSelectionText $sourceCombo
    $captureLabel = Get-ControlTextById $hwnd $automationCaptureLabelId
    $deviceCountLine = Get-ControlTextById $hwnd $automationDeviceCountLineId
    $summaryText = Get-ControlTextById $hwnd $automationSummaryTextId
    $appTargetText = Get-Text $appLoopbackEdit
    $lastSourceText = $sourceText
    $lastCaptureLabel = $captureLabel
    $lastDeviceCountLine = $deviceCountLine
    $lastAppTargetText = $appTargetText
    $sourceSeen = $sourceText -eq "Application Loopback"
    $labelSeen = $captureLabel -eq "App Loopback Source"
    $deviceCountSeen = $deviceCountLine -like "Application loopback sources:*"
    $targetSeen =
      $summaryText.IndexOf("App loopback target: 1234", [System.StringComparison]::Ordinal) -ge 0
    $surfaceSeen =
      $sourceSeen -and
      $labelSeen -and
      $deviceCountSeen -and
      $targetSeen
    $summarySeen =
      $summaryText.IndexOf("App loopback target: 1234", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Application loopback captures audio rendered by a target process tree instead of a device endpoint.", [System.StringComparison]::Ordinal) -ge 0
    if ($surfaceSeen -and $summarySeen) {
      break
    }
  }

  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$probeButtonId, [IntPtr]::Zero)
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $probeText = Get-ControlTextById $hwnd $automationProbeTextId
    $requestedCaptureIdSeen =
      $probeText.IndexOf("RequestedCaptureDeviceId: app-loopback", [System.StringComparison]::Ordinal) -ge 0
    if ($probeText.IndexOf("FailureStage: format-resolution", [System.StringComparison]::Ordinal) -ge 0 -and
        $probeText.IndexOf("Application loopback is not supported on this machine.", [System.StringComparison]::Ordinal) -ge 0 -and
        $requestedCaptureIdSeen) {
      $quickFailureSeen = $true
      break
    }
  }

  Invoke-EditTextChange $hwnd $appLoopbackEditId $baselineAppTargetText
  Invoke-ComboSelectionChange $hwnd $sourceComboId $baselineSourceIndex
  $restored = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $sourceText = Get-ComboSelectionText $sourceCombo
    $captureEnabled = [Native]::IsWindowEnabled($captureCombo)
    $appTargetText = Get-Text $appLoopbackEdit
    if ($sourceText -eq $baselineSourceText -and
        $captureEnabled -eq $baselineCaptureEnabled -and
        $appTargetText -eq $baselineAppTargetText) {
      $restored = $true
      break
    }
  }

  [pscustomobject]@{
    SurfaceSeen = $surfaceSeen
    SourceSeen = $sourceSeen
    LabelSeen = $labelSeen
    DeviceCountSeen = $deviceCountSeen
    TargetSeen = $targetSeen
    RequestedCaptureIdSeen = $requestedCaptureIdSeen
    SummarySeen = $summarySeen
    QuickFailureSeen = $quickFailureSeen
    Restored = $restored
    LastSourceText = $lastSourceText
    LastCaptureLabel = $lastCaptureLabel
    LastDeviceCountLine = $lastDeviceCountLine
    LastAppTargetText = $lastAppTargetText
    ProbeText = $probeText
    SummaryText = $summaryText
  }
}
function Get-ControlSnapshot([IntPtr]$handle, [string]$type) {
  $snapshot = [ordered]@{
    Enabled = [Native]::IsWindowEnabled($handle)
    Value = ""
  }
  switch ($type) {
    "check" {
      $snapshot.Value = [string](Get-CheckState $handle)
      break
    }
    "combo" {
      $snapshot.Value = Get-ComboSelectionText $handle
      break
    }
    default {
      $snapshot.Value = Get-Text $handle
      break
    }
  }
  [pscustomobject]$snapshot
}
function Format-SnapshotMap([hashtable]$map) {
  ($map.GetEnumerator() | ForEach-Object { "$($_.Key)=[$($_.Value)]" }) -join ", "
}
function Test-SnapshotEquality([hashtable]$baseline, [hashtable]$current) {
  foreach ($entry in $baseline.GetEnumerator()) {
    if (-not $current.Contains($entry.Key)) {
      return $false
    }
    if ($current[$entry.Key] -ne $entry.Value) {
      return $false
    }
  }
  return $true
}
function Test-GuiActiveConfigurationDiagnosticsText([string]$text) {
  if ([string]::IsNullOrEmpty($text)) {
    return $false
  }
  $needles = @(
    "Current configured capture:",
    "Current configured render:",
    "Effective configured render request:",
    "Active session requested capture:",
    "Active session requested render:",
    "Active session requested capture id:",
    "Active session requested render id:",
    "Active session negotiated capture:",
    "Active session negotiated render:",
    "Active capture mode:",
    "Active render mode:",
    "Active resampler:",
    "Active capture runtime:",
    "Active render runtime:",
    "Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream."
  )
  foreach ($needle in $needles) {
    if ($text.IndexOf($needle, [System.StringComparison]::Ordinal) -lt 0) {
      return $false
    }
  }
  return $true
}
function Invoke-AutoAlignToggleCheck([IntPtr]$hwnd, [int]$checkboxId, [int[]]$renderControlIds, [int]$maxPolls = 60) {
  $checkbox = [Native]::GetDlgItem($hwnd, $checkboxId)
  $renderControls = @{}
  foreach ($id in $renderControlIds) {
    $renderControls[[string]$id] = [Native]::GetDlgItem($hwnd, $id)
  }

  $baselineCheck = Get-CheckState $checkbox
  $baselineEnabled = [ordered]@{}
  foreach ($entry in $renderControls.GetEnumerator()) {
    $baselineEnabled[$entry.Key] = [Native]::IsWindowEnabled($entry.Value)
  }

  Invoke-CheckboxClick $checkbox
  $toggledSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $allToggled = $true
    foreach ($entry in $renderControls.GetEnumerator()) {
      if ([Native]::IsWindowEnabled($entry.Value) -eq $baselineEnabled[$entry.Key]) {
        $allToggled = $false
        break
      }
    }
    if ($allToggled) {
      $toggledSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $checkbox
  $restored = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $currentCheck = Get-CheckState $checkbox
    $matches = ($currentCheck -eq $baselineCheck)
    foreach ($entry in $renderControls.GetEnumerator()) {
      if ([Native]::IsWindowEnabled($entry.Value) -ne $baselineEnabled[$entry.Key]) {
        $matches = $false
        break
      }
    }
    if ($matches) {
      $restored = $true
      break
    }
  }

  [pscustomobject]@{
    ToggledSeen = $toggledSeen
    Restored = $restored
  }
}
function Invoke-FollowDefaultsToggleCheck([IntPtr]$hwnd, [int]$checkboxId, [int[]]$deviceControlIds, [int]$maxPolls = 60) {
  $checkbox = [Native]::GetDlgItem($hwnd, $checkboxId)
  $deviceControls = @{}
  foreach ($id in $deviceControlIds) {
    $deviceControls[[string]$id] = [Native]::GetDlgItem($hwnd, $id)
  }

  $baselineCheck = Get-CheckState $checkbox
  $baselineEnabled = [ordered]@{}
  foreach ($entry in $deviceControls.GetEnumerator()) {
    $baselineEnabled[$entry.Key] = [Native]::IsWindowEnabled($entry.Value)
  }

  Invoke-CheckboxClick $checkbox
  $toggledSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $allToggled = $true
    foreach ($entry in $deviceControls.GetEnumerator()) {
      if ([Native]::IsWindowEnabled($entry.Value) -eq $baselineEnabled[$entry.Key]) {
        $allToggled = $false
        break
      }
    }
    if ($allToggled) {
      $toggledSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $checkbox
  $restored = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $currentCheck = Get-CheckState $checkbox
    $matches = ($currentCheck -eq $baselineCheck)
    foreach ($entry in $deviceControls.GetEnumerator()) {
      if ([Native]::IsWindowEnabled($entry.Value) -ne $baselineEnabled[$entry.Key]) {
        $matches = $false
        break
      }
    }
    if ($matches) {
      $restored = $true
      break
    }
  }

  [pscustomobject]@{
    ToggledSeen = $toggledSeen
    Restored = $restored
  }
}
function Invoke-MonitorToggleCheck([IntPtr]$hwnd, [int]$checkboxId, [int[]]$renderControlIds, [int]$maxPolls = 60) {
  $checkbox = [Native]::GetDlgItem($hwnd, $checkboxId)
  $renderControls = @{}
  foreach ($id in $renderControlIds) {
    $renderControls[[string]$id] = [Native]::GetDlgItem($hwnd, $id)
  }

  $baselineCheck = Get-CheckState $checkbox
  $baselineEnabled = [ordered]@{}
  foreach ($entry in $renderControls.GetEnumerator()) {
    $baselineEnabled[$entry.Key] = [Native]::IsWindowEnabled($entry.Value)
  }

  Invoke-CheckboxClick $checkbox
  $disabledSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $allDisabled = $true
    $anyChanged = $false
    foreach ($entry in $renderControls.GetEnumerator()) {
      $enabled = [Native]::IsWindowEnabled($entry.Value)
      if ($enabled) {
        $allDisabled = $false
      }
      if ($enabled -ne $baselineEnabled[$entry.Key]) {
        $anyChanged = $true
      }
    }
    if ($allDisabled -and $anyChanged) {
      $disabledSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $checkbox
  $restored = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $currentCheck = Get-CheckState $checkbox
    $matches = ($currentCheck -eq $baselineCheck)
    foreach ($entry in $renderControls.GetEnumerator()) {
      if ([Native]::IsWindowEnabled($entry.Value) -ne $baselineEnabled[$entry.Key]) {
        $matches = $false
        break
      }
    }
    if ($matches) {
      $restored = $true
      break
    }
  }

  [pscustomobject]@{
    DisabledSeen = $disabledSeen
    Restored = $restored
  }
}
function Invoke-FollowDefaultsMonitorCompositionCheck([IntPtr]$hwnd, [int]$followDefaultsCheckboxId, [int]$monitorCheckboxId, [int]$captureDeviceComboId, [int]$renderDeviceComboId, [int]$maxPolls = 60) {
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $monitorCheckbox = [Native]::GetDlgItem($hwnd, $monitorCheckboxId)
  $captureDeviceCombo = [Native]::GetDlgItem($hwnd, $captureDeviceComboId)
  $renderDeviceCombo = [Native]::GetDlgItem($hwnd, $renderDeviceComboId)

  $baselineFollow = Get-CheckState $followDefaultsCheckbox
  $baselineMonitor = Get-CheckState $monitorCheckbox

  if ($baselineFollow -ne 0) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }
  if ($baselineMonitor -ne 1) {
    Invoke-CheckboxClick $monitorCheckbox
    Start-Sleep -Milliseconds 200
  }

  $normalizedOk =
      [Native]::IsWindowEnabled($captureDeviceCombo) -and
      [Native]::IsWindowEnabled($renderDeviceCombo)

  Invoke-CheckboxClick $monitorCheckbox
  $monitorOffSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((Get-CheckState $monitorCheckbox) -eq 0 -and
        [Native]::IsWindowEnabled($captureDeviceCombo) -and
        (-not [Native]::IsWindowEnabled($renderDeviceCombo))) {
      $monitorOffSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $followDefaultsCheckbox
  $combinedDisabledSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((Get-CheckState $followDefaultsCheckbox) -eq 1 -and
        (-not [Native]::IsWindowEnabled($captureDeviceCombo)) -and
        (-not [Native]::IsWindowEnabled($renderDeviceCombo))) {
      $combinedDisabledSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $followDefaultsCheckbox
  $followReleasedSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((Get-CheckState $followDefaultsCheckbox) -eq 0 -and
        [Native]::IsWindowEnabled($captureDeviceCombo) -and
        (-not [Native]::IsWindowEnabled($renderDeviceCombo))) {
      $followReleasedSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $monitorCheckbox
  $restoredNormalized = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((Get-CheckState $monitorCheckbox) -eq 1 -and
        [Native]::IsWindowEnabled($captureDeviceCombo) -and
        [Native]::IsWindowEnabled($renderDeviceCombo)) {
      $restoredNormalized = $true
      break
    }
  }

  if ($baselineFollow -ne (Get-CheckState $followDefaultsCheckbox)) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }
  if ($baselineMonitor -ne (Get-CheckState $monitorCheckbox)) {
    Invoke-CheckboxClick $monitorCheckbox
    Start-Sleep -Milliseconds 200
  }

  $restoredBaseline = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((Get-CheckState $followDefaultsCheckbox) -eq $baselineFollow -and
        (Get-CheckState $monitorCheckbox) -eq $baselineMonitor) {
      $restoredBaseline = $true
      break
    }
  }

  [pscustomobject]@{
    NormalizedOk = $normalizedOk
    MonitorOffSeen = $monitorOffSeen
    CombinedDisabledSeen = $combinedDisabledSeen
    FollowReleasedSeen = $followReleasedSeen
    RestoredNormalized = $restoredNormalized
    RestoredBaseline = $restoredBaseline
  }
}
function Invoke-FollowDefaultsWhileRunningCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$followDefaultsCheckboxId, [int]$captureDeviceComboId, [int]$renderDeviceComboId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $captureDeviceCombo = [Native]::GetDlgItem($hwnd, $captureDeviceComboId)
  $renderDeviceCombo = [Native]::GetDlgItem($hwnd, $renderDeviceComboId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $baselineFollowDefaultsCheck = Get-CheckState $followDefaultsCheckbox
  $runningStart = Start-SessionAndWait -hwnd $hwnd -startButton $startButton -stopButton $stopButton -startButtonId $startButtonId -additionalCheck {
    (Get-CheckState $followDefaultsCheckbox) -eq $baselineFollowDefaultsCheck -and
    (Get-ControlEnabledSafe $captureDeviceCombo) -and
    (Get-ControlEnabledSafe $renderDeviceCombo)
  } -maxPolls $maxPolls
  $started = $runningStart.Started
  $runningBaselineSeen = $runningStart.RunningSeen

  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }

  $disabledWhileRunningSeen = $false
  $summarySeen = $false
  $diagnosticsSeen = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $title = Get-Text $hwnd
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $summarySeen =
      $summaryText.IndexOf("Device selection follows current system defaults; manual device picks are inactive", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0
    if ($title -like "*| Running |*" -and
        (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
        (-not [Native]::IsWindowEnabled($captureDeviceCombo)) -and
        (-not [Native]::IsWindowEnabled($renderDeviceCombo)) -and
        (-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton) -and
        $summarySeen -and
        $diagnosticsSeen) {
      $disabledWhileRunningSeen = $true
      break
    }
  }

  if ($baselineFollowDefaultsCheck -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }

  $restoredRunningSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $title = Get-Text $hwnd
    if ($title -like "*| Running |*" -and
        (Get-CheckState $followDefaultsCheckbox) -eq $baselineFollowDefaultsCheck -and
        [Native]::IsWindowEnabled($captureDeviceCombo) -and
        [Native]::IsWindowEnabled($renderDeviceCombo) -and
        (-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $restoredRunningSeen = $true
      break
    }
  }

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
    if (-not $stoppedCleanly) {
      Start-Sleep -Milliseconds 200
      $stoppedCleanly = Wait-ForIdleAfterStop $hwnd $startButton $stopButton ($maxPolls + 30)
    }
    if (-not $stoppedCleanly -and $runningRestoredSeen) {
      $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId ($maxPolls + 30)
    }
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    RunningBaselineSeen = $runningBaselineSeen
    DisabledWhileRunningSeen = $disabledWhileRunningSeen
    SummarySeen = $summarySeen
    DiagnosticsSeen = $diagnosticsSeen
    RestoredRunningSeen = $restoredRunningSeen
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-AutoAlignMonitorCompositionCheck([IntPtr]$hwnd, [int]$autoAlignCheckboxId, [int]$monitorCheckboxId, [int[]]$renderFormatControlIds, [int]$maxPolls = 60) {
  $autoAlignCheckbox = [Native]::GetDlgItem($hwnd, $autoAlignCheckboxId)
  $monitorCheckbox = [Native]::GetDlgItem($hwnd, $monitorCheckboxId)
  $renderFormatControls = @{}
  foreach ($id in $renderFormatControlIds) {
    $renderFormatControls[[string]$id] = [Native]::GetDlgItem($hwnd, $id)
  }

  $baselineAutoAlign = Get-CheckState $autoAlignCheckbox
  $baselineMonitor = Get-CheckState $monitorCheckbox

  if ($baselineAutoAlign -ne 0) {
    Invoke-CheckboxClick $autoAlignCheckbox
    Start-Sleep -Milliseconds 200
  }
  if ($baselineMonitor -ne 1) {
    Invoke-CheckboxClick $monitorCheckbox
    Start-Sleep -Milliseconds 200
  }

  $normalizedOk = $true
  foreach ($entry in $renderFormatControls.GetEnumerator()) {
    if (-not [Native]::IsWindowEnabled($entry.Value)) {
      $normalizedOk = $false
      break
    }
  }

  Invoke-CheckboxClick $autoAlignCheckbox
  $autoAlignDisabledSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $allDisabled = $true
    foreach ($entry in $renderFormatControls.GetEnumerator()) {
      if ([Native]::IsWindowEnabled($entry.Value)) {
        $allDisabled = $false
        break
      }
    }
    if ((Get-CheckState $autoAlignCheckbox) -eq 1 -and $allDisabled) {
      $autoAlignDisabledSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $monitorCheckbox
  $monitorOffWhileAutoAlignSeen = $false
  $autoAlignCheckboxDisabledSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $allDisabled = $true
    foreach ($entry in $renderFormatControls.GetEnumerator()) {
      if ([Native]::IsWindowEnabled($entry.Value)) {
        $allDisabled = $false
        break
      }
    }
    if (-not [Native]::IsWindowEnabled($autoAlignCheckbox)) {
      $autoAlignCheckboxDisabledSeen = $true
    }
    if ((Get-CheckState $monitorCheckbox) -eq 0 -and
        (Get-CheckState $autoAlignCheckbox) -eq 1 -and
        $allDisabled -and
        $autoAlignCheckboxDisabledSeen) {
      $monitorOffWhileAutoAlignSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $monitorCheckbox
  $monitorRestoredWhileAutoAlignSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $allDisabled = $true
    foreach ($entry in $renderFormatControls.GetEnumerator()) {
      if ([Native]::IsWindowEnabled($entry.Value)) {
        $allDisabled = $false
        break
      }
    }
    if ((Get-CheckState $autoAlignCheckbox) -eq 1 -and
        (Get-CheckState $monitorCheckbox) -eq 1 -and
        $allDisabled) {
      $monitorRestoredWhileAutoAlignSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $autoAlignCheckbox
  $restoredNormalized = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $allEnabled = $true
    foreach ($entry in $renderFormatControls.GetEnumerator()) {
      if (-not [Native]::IsWindowEnabled($entry.Value)) {
        $allEnabled = $false
        break
      }
    }
    if ((Get-CheckState $monitorCheckbox) -eq 1 -and
        (Get-CheckState $autoAlignCheckbox) -eq 0 -and
        $allEnabled) {
      $restoredNormalized = $true
      break
    }
  }

  if ($baselineAutoAlign -ne (Get-CheckState $autoAlignCheckbox)) {
    Invoke-CheckboxClick $autoAlignCheckbox
    Start-Sleep -Milliseconds 200
  }
  if ($baselineMonitor -ne (Get-CheckState $monitorCheckbox)) {
    Invoke-CheckboxClick $monitorCheckbox
    Start-Sleep -Milliseconds 200
  }

  $restoredBaseline = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((Get-CheckState $autoAlignCheckbox) -eq $baselineAutoAlign -and
        (Get-CheckState $monitorCheckbox) -eq $baselineMonitor) {
      $restoredBaseline = $true
      break
    }
  }

  [pscustomobject]@{
    NormalizedOk = $normalizedOk
    AutoAlignDisabledSeen = $autoAlignDisabledSeen
    MonitorOffWhileAutoAlignSeen = $monitorOffWhileAutoAlignSeen
    AutoAlignCheckboxDisabledSeen = $autoAlignCheckboxDisabledSeen
    MonitorRestoredWhileAutoAlignSeen = $monitorRestoredWhileAutoAlignSeen
    RestoredNormalized = $restoredNormalized
    RestoredBaseline = $restoredBaseline
  }
}
function Invoke-SessionButtonCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 80) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $baselineTitle = Get-Text $hwnd

  $idleBaselineOk =
      [Native]::IsWindowEnabled($startButton) -and
      (-not [Native]::IsWindowEnabled($stopButton))
  $idleTitleOk =
      $baselineTitle -notlike "*Quick Probe Running*" -and
      $baselineTitle -notlike "*Probe Matrix Running*" -and
      $baselineTitle -notlike "*Capture  | Render *"

  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
  $runningStateSeen = $false
  $runningTitleSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $title = Get-Text $hwnd
    if ($title -like "*| Running |*") {
      $runningTitleSeen = $true
    }
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton) -and
        $runningTitleSeen) {
      $runningStateSeen = $true
      break
    }
  }

  $restored = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  $titleRestored = $false
  $finalTitle = Get-Text $hwnd
  if ($finalTitle -notlike "*| Running |*" -and
      $finalTitle -notlike "*Quick Probe Running*" -and
      $finalTitle -notlike "*Probe Matrix Running*") {
    $titleRestored = $true
  }

  [pscustomobject]@{
    IdleBaselineOk = $idleBaselineOk
    IdleTitleOk = $idleTitleOk
    BaselineTitle = $baselineTitle
    RunningStateSeen = $runningStateSeen
    RunningTitleSeen = $runningTitleSeen
    TitleRestored = $titleRestored
    Restored = $restored
  }
}
function Invoke-CloseDuringBusyProbeCheck([string]$guiExePath, [int]$probeButtonId, [string]$runningTitleNeedle, [string]$runningButtonText, [int]$maxPolls = 300) {
  Get-Process winaudio -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  $p = Start-Process -FilePath $guiExePath -PassThru
  $hwnd = Wait-ForWindowHandle $p 100
  if ($p.HasExited -or $hwnd -eq [IntPtr]::Zero) {
    Stop-ProcessByIdIfRunning $p.Id
    return [pscustomobject]@{
      WindowReady = $false
      BusySeen = $false
      ExitedCleanly = $false
    }
  }

  $probeButton = Get-ControlHandle $hwnd $probeButtonId
  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$probeButtonId, [IntPtr]::Zero)
  $busySeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $title = Get-Text $hwnd
    $buttonText = Get-ControlTextSafe $probeButton
    $probeDisabled = -not (Get-ControlEnabledSafe $probeButton)
    if ($title -like "*$runningTitleNeedle*" -or
        $buttonText -eq $runningButtonText -or
        $probeDisabled) {
      $busySeen = $true
      break
    }
  }

  [void][Native]::SendNotifyMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
  $exited = Wait-ForProcessExit $p $maxPolls

  [pscustomobject]@{
    WindowReady = $true
    BusySeen = $busySeen
    ExitedCleanly = $exited
  }
}
function Invoke-CloseWhileRunningSessionCheck([string]$guiExePath, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 300) {
  Get-Process winaudio -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  $p = Start-Process -FilePath $guiExePath -PassThru
  $hwnd = Wait-ForWindowHandle $p 100
  if ($p.HasExited -or $hwnd -eq [IntPtr]::Zero) {
    Stop-ProcessByIdIfRunning $p.Id
    return [pscustomobject]@{
      WindowReady = $false
      RunningSeen = $false
      ExitedCleanly = $false
    }
  }

  $startButton = Get-ControlHandle $hwnd $startButtonId
  $stopButton = Get-ControlHandle $hwnd $stopButtonId
  $runningStart = Start-SessionAndWait $hwnd $startButton $stopButton $startButtonId $null $maxPolls
  $runningSeen = $runningStart.RunningSeen -or $runningStart.RunningObservedEver

  [void][Native]::SendNotifyMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
  $exited = Wait-ForProcessExit $p $maxPolls

  [pscustomobject]@{
    WindowReady = $true
    RunningSeen = $runningSeen
    ExitedCleanly = $exited
  }
}
function Invoke-CloseDuringBusyProbeWhileRunningCheck([string]$guiExePath, [int]$probeButtonId, [int]$startButtonId, [int]$stopButtonId, [string]$runningTitleNeedle, [string]$runningButtonText, [int]$maxPolls = 300) {
  Get-Process winaudio -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  $p = Start-Process -FilePath $guiExePath -PassThru
  $hwnd = Wait-ForWindowHandle $p 100
  if ($p.HasExited -or $hwnd -eq [IntPtr]::Zero) {
    Stop-ProcessByIdIfRunning $p.Id
    return [pscustomobject]@{
      WindowReady = $false
      RunningSeen = $false
      BusySeen = $false
      ExitedCleanly = $false
    }
  }

  $startButton = Get-ControlHandle $hwnd $startButtonId
  $stopButton = Get-ControlHandle $hwnd $stopButtonId
  $probeButton = Get-ControlHandle $hwnd $probeButtonId
  if (Get-ControlEnabledSafe $startButton) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
  }

  $runningState = Wait-ForRunningEvidence $hwnd $startButton $stopButton $probeButton $maxPolls

  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$probeButtonId, [IntPtr]::Zero)
  $busySeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $title = Get-Text $hwnd
    $buttonText = Get-ControlTextSafe $probeButton
    $probeDisabled = -not (Get-ControlEnabledSafe $probeButton)
    if (($title -like "*$runningTitleNeedle*" -or
        $buttonText -eq $runningButtonText -or
        $probeDisabled) -and
        ($runningState.RunningSeen -or $runningState.RunningObservedEver -or (Get-ControlEnabledSafe $stopButton))) {
      $busySeen = $true
      break
    }
  }

  [void][Native]::SendNotifyMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
  $exited = Wait-ForProcessExit $p $maxPolls

  [pscustomobject]@{
    WindowReady = $true
    RunningSeen = ($runningState.RunningSeen -or $runningState.RunningObservedEver)
    BusySeen = $busySeen
    ExitedCleanly = $exited
  }
}
function Invoke-SourceModeToggleCheck([IntPtr]$hwnd, [int]$sourceComboId, [int]$captureComboId, [int]$renderComboId, [int]$maxPolls = 80) {
  $sourceCombo = [Native]::GetDlgItem($hwnd, $sourceComboId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $automationCaptureLabelId = 1901
  $automationDeviceCountLineId = 1902

  $baselineSource = Get-ComboSelectionText $sourceCombo
  $baselineCaptureCount = Get-ComboCount $captureCombo
  $baselineCaptureSelected = Get-ComboSelectionText $captureCombo
  $baselineRenderSelected = Get-ComboSelectionText $renderCombo
  $baselineRenderCount = Get-ComboCount $renderCombo
  $baselineCaptureItems = Get-ComboItems $captureCombo
  $baselineRenderItems = Get-ComboItems $renderCombo
  $baselineCaptureLabel = Get-ControlTextById $hwnd $automationCaptureLabelId
  $baselineDeviceCountLine = Get-ControlTextById $hwnd $automationDeviceCountLineId

  Invoke-ComboSelectionChangeSync $hwnd $sourceComboId 1
  $loopbackSeen = $false
  $loopbackLabelSeen = $false
  $loopbackDeviceCountSeen = $false
  $loopbackItemsMatchSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $sourceText = Get-ComboSelectionText $sourceCombo
    $captureSelected = Get-ComboSelectionText $captureCombo
    $captureCount = Get-ComboCount $captureCombo
    $renderSelected = Get-ComboSelectionText $renderCombo
    $renderCount = Get-ComboCount $renderCombo
    $captureItems = Get-ComboItems $captureCombo
    $renderItems = Get-ComboItems $renderCombo
    $captureLabel = Get-ControlTextById $hwnd $automationCaptureLabelId
    $deviceCountLine = Get-ControlTextById $hwnd $automationDeviceCountLineId
    if ($captureLabel -eq "Loopback Capture Device") {
      $loopbackLabelSeen = $true
    }
    if ($deviceCountLine -like "Loopback capture devices:*") {
      $loopbackDeviceCountSeen = $true
    }
    if (Test-StringArrayEquality $captureItems $renderItems) {
      $loopbackItemsMatchSeen = $true
    }
    if ($sourceText -eq "System Loopback" -and
        $captureSelected -eq $renderSelected -and
        $captureCount -eq $renderCount -and
        $loopbackLabelSeen -and
        $loopbackDeviceCountSeen -and
        $loopbackItemsMatchSeen) {
      $loopbackSeen = $true
      break
    }
  }

  Invoke-ComboSelectionChangeSync $hwnd $sourceComboId 0
  $restored = $false
  $labelRestored = $false
  $deviceCountRestored = $false
  $captureItemsRestored = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $sourceText = Get-ComboSelectionText $sourceCombo
    $captureSelected = Get-ComboSelectionText $captureCombo
    $captureCount = Get-ComboCount $captureCombo
    $captureItems = Get-ComboItems $captureCombo
    $captureLabel = Get-ControlTextById $hwnd $automationCaptureLabelId
    $deviceCountLine = Get-ControlTextById $hwnd $automationDeviceCountLineId
    if ($captureLabel -eq $baselineCaptureLabel) {
      $labelRestored = $true
    }
    if ($deviceCountLine -eq $baselineDeviceCountLine) {
      $deviceCountRestored = $true
    }
    if (Test-StringArrayEquality $captureItems $baselineCaptureItems) {
      $captureItemsRestored = $true
    }
    if ($sourceText -eq $baselineSource -and
        $captureSelected -eq $baselineCaptureSelected -and
        $captureCount -eq $baselineCaptureCount -and
        $labelRestored -and
        $deviceCountRestored -and
        $captureItemsRestored) {
      $restored = $true
      break
    }
  }

  [pscustomobject]@{
    BaselineSource = $baselineSource
    BaselineRenderSelected = $baselineRenderSelected
    BaselineRenderCount = $baselineRenderCount
    BaselineCaptureLabel = $baselineCaptureLabel
    BaselineDeviceCountLine = $baselineDeviceCountLine
    LoopbackSeen = $loopbackSeen
    LoopbackLabelSeen = $loopbackLabelSeen
    LoopbackDeviceCountSeen = $loopbackDeviceCountSeen
    LoopbackItemsMatchSeen = $loopbackItemsMatchSeen
    LabelRestored = $labelRestored
    DeviceCountRestored = $deviceCountRestored
    CaptureItemsRestored = $captureItemsRestored
    Restored = $restored
  }
}
function Invoke-SourceModeWhileRunningCheck([IntPtr]$hwnd, [int]$sourceComboId, [int]$captureComboId, [int]$renderComboId, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 120) {
  $sourceCombo = [Native]::GetDlgItem($hwnd, $sourceComboId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $automationCaptureLabelId = 1901
  $automationDeviceCountLineId = 1902
  $summaryControlId = 1903

  $baselineSource = Get-ComboSelectionText $sourceCombo
  $baselineCaptureSelected = Get-ComboSelectionText $captureCombo
  $baselineCaptureItems = Get-ComboItems $captureCombo
  $baselineCaptureLabel = Get-ControlTextById $hwnd $automationCaptureLabelId
  $baselineDeviceCountLine = Get-ControlTextById $hwnd $automationDeviceCountLineId

  $runningStart = Start-SessionAndWait -hwnd $hwnd -startButton $startButton -stopButton $stopButton -startButtonId $startButtonId -additionalCheck {
    (-not [string]::IsNullOrWhiteSpace((Get-ComboSelectionText $sourceCombo))) -and
    (-not [string]::IsNullOrWhiteSpace((Get-ControlTextById $hwnd $automationCaptureLabelId))) -and
    (-not [string]::IsNullOrWhiteSpace((Get-ControlTextById $hwnd $automationDeviceCountLineId)))
  } -maxPolls $maxPolls
  $started = $runningStart.Started
  $runningBaselineSeen = $runningStart.RunningSeen

  Invoke-ComboSelectionChangeSync $hwnd $sourceComboId 1
  $loopbackSeen = $false
  $runningPreserved = $false
  $summarySeen = $false
  $summaryDriftSeen = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $sourceText = Get-ComboSelectionText $sourceCombo
    $captureSelected = Get-ComboSelectionText $captureCombo
    $renderSelected = Get-ComboSelectionText $renderCombo
    $captureItems = Get-ComboItems $captureCombo
    $renderItems = Get-ComboItems $renderCombo
    $captureLabel = Get-ControlTextById $hwnd $automationCaptureLabelId
    $deviceCountLine = Get-ControlTextById $hwnd $automationDeviceCountLineId
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $loopbackSeen =
      $sourceText -eq "System Loopback" -and
      $captureSelected -eq $renderSelected -and
      (Test-StringArrayEquality $captureItems $renderItems) -and
      $captureLabel -eq "Loopback Capture Device" -and
      $deviceCountLine -like "Loopback capture devices:*"
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    $summarySeen =
      $summaryText.IndexOf("Loopback capture uses render endpoints as capture sources.", [System.StringComparison]::Ordinal) -ge 0
    $summaryDriftSeen =
      $summaryText.IndexOf("Running session note: the current capture device pick applies to the next rebuilt or restarted session, while the already-active stream still uses the previous capture device.", [System.StringComparison]::Ordinal) -ge 0
    if ($loopbackSeen -and $runningPreserved -and $summarySeen -and $summaryDriftSeen) {
      break
    }
  }

  Invoke-ComboSelectionChangeSync $hwnd $sourceComboId 0
  $runningRestoredSeen = $false
  $stableRestoredSamples = 0
  $restoredSummaryText = ""
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $restoredSourceText = Get-ComboSelectionText $sourceCombo
    $restoredCaptureLabel = Get-ControlTextById $hwnd $automationCaptureLabelId
    $restoredDeviceCountLine = Get-ControlTextById $hwnd $automationDeviceCountLineId
    $restoredSummaryText = Get-ControlTextById $hwnd $summaryControlId
    $runningStillPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    $summaryRestoredSeen =
      $restoredSummaryText.IndexOf("Capture: WASAPI / $baselineSource /", [System.StringComparison]::Ordinal) -ge 0
    $restoredCaptureLabel = Get-ControlTextById $hwnd $automationCaptureLabelId
    $surfaceRestoredSeen =
      $restoredCaptureLabel -eq $baselineCaptureLabel -and
      $restoredDeviceCountLine -eq $baselineDeviceCountLine
    if (($restoredSourceText -eq $baselineSource -or $summaryRestoredSeen) -and
        $surfaceRestoredSeen -and
        $runningStillPreserved -and
        $restoredCaptureLabel -eq $baselineCaptureLabel -and
        $restoredDeviceCountLine -eq $baselineDeviceCountLine) {
      $stableRestoredSamples += 1
      if ($stableRestoredSamples -ge 3) {
        $runningRestoredSeen = $true
        break
      }
    } else {
      $stableRestoredSamples = 0
    }
  }

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  $restored = $false
  $finalSourceText = ""
  $finalCaptureSelected = ""
  $finalCaptureLabel = ""
  $finalDeviceCountLine = ""
  $stableFinalRestoredSamples = 0
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $sourceText = Get-ComboSelectionText $sourceCombo
    $captureSelected = Get-ComboSelectionText $captureCombo
    $captureItems = Get-ComboItems $captureCombo
    $captureLabel = Get-ControlTextById $hwnd $automationCaptureLabelId
    $deviceCountLine = Get-ControlTextById $hwnd $automationDeviceCountLineId
    $finalSourceText = $sourceText
    $finalCaptureSelected = $captureSelected
    $finalCaptureLabel = $captureLabel
    $finalDeviceCountLine = $deviceCountLine
    $idleSurfaceSeen =
      [Native]::IsWindowEnabled($startButton) -and
      (-not [Native]::IsWindowEnabled($stopButton))
    if ($idleSurfaceSeen -and
        $sourceText -eq $baselineSource -and
        $captureSelected -eq $baselineCaptureSelected -and
        (Test-StringArrayEquality $captureItems $baselineCaptureItems) -and
        $captureLabel -eq $baselineCaptureLabel -and
        $deviceCountLine -eq $baselineDeviceCountLine) {
      $stableFinalRestoredSamples += 1
      if ($stableFinalRestoredSamples -ge 3) {
        $restored = $true
        break
      }
    } else {
      $stableFinalRestoredSamples = 0
    }
  }

  [pscustomobject]@{
    RunningBaselineSeen = $runningBaselineSeen
    LoopbackSeen = $loopbackSeen
    RunningPreserved = $runningPreserved
    SummarySeen = $summarySeen
    SummaryDriftSeen = $summaryDriftSeen
    RunningRestoredSeen = $runningRestoredSeen
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    FinalSourceText = $finalSourceText
    FinalCaptureSelected = $finalCaptureSelected
    FinalCaptureLabel = $finalCaptureLabel
    FinalDeviceCountLine = $finalDeviceCountLine
    SummaryText = $summaryText
    RestoredSummaryText = $restoredSummaryText
  }
}
function Invoke-FollowDefaultsSourceModeWhileRunningCheck([IntPtr]$hwnd, [int]$sourceComboId, [int]$followDefaultsCheckboxId, [int]$captureComboId, [int]$renderComboId, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 120) {
  $sourceCombo = [Native]::GetDlgItem($hwnd, $sourceComboId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904
  $cbGetCurSel = 0x0147

  $baselineSourceIndex = [int][Native]::SendMessage($sourceCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineFollowDefaults = Get-CheckState $followDefaultsCheckbox
  $baselineCaptureItems = Get-ComboItems $captureCombo
  $baselineSourceText = Get-ComboItemText $sourceCombo $baselineSourceIndex
  $runningSessionNote = "Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream."

  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }

  $runningStart = Start-SessionAndWait -hwnd $hwnd -startButton $startButton -stopButton $stopButton -startButtonId $startButtonId -additionalCheck {
    (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
    (-not (Get-ControlEnabledSafe $captureCombo)) -and
    (-not (Get-ControlEnabledSafe $renderCombo)) -and
    (-not [string]::IsNullOrWhiteSpace((Get-ComboSelectionText $sourceCombo)))
  } -maxPolls $maxPolls
  $started = $runningStart.Started
  $runningBaselineSeen = $runningStart.RunningSeen

  Invoke-ComboSelectionChange $hwnd $sourceComboId 1
  $loopbackSeen = $false
  $summarySeen = $false
  $diagnosticsSeen = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $sourceText = Get-ComboSelectionText $sourceCombo
    $captureSelected = Get-ComboSelectionText $captureCombo
    $renderSelected = Get-ComboSelectionText $renderCombo
    $captureItems = Get-ComboItems $captureCombo
    $renderItems = Get-ComboItems $renderCombo
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $loopbackSeen =
      $sourceText -eq "System Loopback" -and
      $captureSelected -eq $renderSelected -and
      (Test-StringArrayEquality $captureItems $renderItems)
    $summarySeen =
      $summaryText.IndexOf("Device selection follows current system defaults; manual device picks are inactive and loopback capture follows the current default render endpoint", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults, and loopback capture follows the current default render endpoint", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    if ($loopbackSeen -and $summarySeen -and $diagnosticsSeen -and $runningPreserved) {
      break
    }
  }

  if ($baselineSourceIndex -ne 1) {
    Invoke-ComboSelectionChangeSync $hwnd $sourceComboId $baselineSourceIndex
    for ($i = 0; $i -lt $maxPolls; $i++) {
      Start-Sleep -Milliseconds 100
      $restoredSourceText = Get-ComboSelectionText $sourceCombo
      $restoredCaptureItems = Get-ComboItems $captureCombo
      if ($restoredSourceText -eq $baselineSourceText -and
          (Test-StringArrayEquality $restoredCaptureItems $baselineCaptureItems)) {
        break
      }
    }
  }
  if ($baselineFollowDefaults -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }

  $runningRestoredSeen = $false
  $stableRunningRestoredSamples = 0
  $restoredSummaryText = ""
  $restored = $false
  $finalSourceText = ""
  $finalFollowDefaultsState = -1
  $finalCaptureEnabled = $false
  $finalRenderEnabled = $false
  $finalSummaryText = ""
  $finalDiagnosticsText = ""
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $sourceText = Get-ComboSelectionText $sourceCombo
    $captureItems = Get-ComboItems $captureCombo
    $restoredSummaryText = Get-ControlTextById $hwnd $summaryControlId
    $runningStillPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    $finalSourceText = $sourceText
    $finalFollowDefaultsState = Get-CheckState $followDefaultsCheckbox
    $finalCaptureEnabled = [Native]::IsWindowEnabled($captureCombo)
    $finalRenderEnabled = [Native]::IsWindowEnabled($renderCombo)
    $finalSummaryText = Get-ControlTextById $hwnd $summaryControlId
    $finalDiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $deviceStateMatches = $false
    if ($finalFollowDefaultsState -eq 1) {
      $deviceStateMatches =
        (-not $finalCaptureEnabled) -and
        (-not $finalRenderEnabled)
    } else {
      $deviceStateMatches =
        $finalCaptureEnabled -and
        $finalRenderEnabled
    }
    $expectedSummarySource = "Capture: WASAPI / $baselineSourceText /"
    $expectedFollowDefaultsLine = if ($baselineFollowDefaults -eq 1) { "Follow defaults: On" } else { "Follow defaults: Off" }
    $summaryMatches =
      $finalSummaryText.IndexOf($expectedSummarySource, [System.StringComparison]::Ordinal) -ge 0 -and
      $finalSummaryText.IndexOf($expectedFollowDefaultsLine, [System.StringComparison]::Ordinal) -ge 0 -and
      $finalSummaryText.IndexOf($runningSessionNote, [System.StringComparison]::Ordinal) -ge 0
    if ($baselineSourceText -ne "System Loopback") {
      $summaryMatches =
        $summaryMatches -and
        $finalSummaryText.IndexOf("Loopback capture uses render endpoints as capture sources.", [System.StringComparison]::Ordinal) -lt 0 -and
        $finalSummaryText.IndexOf("Loopback note:", [System.StringComparison]::Ordinal) -lt 0
    }
    if (($sourceText -eq $baselineSourceText -or $summaryMatches) -and
        $deviceStateMatches -and
        $runningStillPreserved) {
      $stableRunningRestoredSamples += 1
      if ($stableRunningRestoredSamples -ge 3) {
        $runningRestoredSeen = $true
      }
    } else {
      $stableRunningRestoredSamples = 0
    }
    if ($sourceText -eq $baselineSourceText -and
        $finalFollowDefaultsState -eq $baselineFollowDefaults -and
        (Test-StringArrayEquality $captureItems $baselineCaptureItems) -and
        $deviceStateMatches -and
        $summaryMatches) {
      $restored = $true
      break
    }
  }

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
    if (-not $stoppedCleanly) {
      Start-Sleep -Milliseconds 200
      $stoppedCleanly = Wait-ForIdleAfterStop $hwnd $startButton $stopButton ($maxPolls + 30)
    }
    if (-not $stoppedCleanly -and $runningRestoredSeen) {
      $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId ($maxPolls + 30)
    }
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    RunningBaselineSeen = $runningBaselineSeen
    LoopbackSeen = $loopbackSeen
    SummarySeen = $summarySeen
    DiagnosticsSeen = $diagnosticsSeen
    RunningPreserved = $runningPreserved
    RunningRestoredSeen = $runningRestoredSeen
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    FinalSourceText = $finalSourceText
    FinalFollowDefaultsState = $finalFollowDefaultsState
    FinalCaptureEnabled = $finalCaptureEnabled
    FinalRenderEnabled = $finalRenderEnabled
    FinalSummaryText = $finalSummaryText
    FinalDiagnosticsText = $finalDiagnosticsText
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
    RestoredSummaryText = $restoredSummaryText
  }
}
function Invoke-FollowDefaultsRenderBackendWhileRunningCheck([IntPtr]$hwnd, [int]$renderBackendComboId, [int]$followDefaultsCheckboxId, [int]$captureComboId, [int]$renderComboId, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 120) {
  $renderBackendCombo = [Native]::GetDlgItem($hwnd, $renderBackendComboId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904
  $cbGetCurSel = 0x0147

  $baselineRenderBackendIndex = [int][Native]::SendMessage($renderBackendCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineFollowDefaults = Get-CheckState $followDefaultsCheckbox
  $baselineBackendText = Get-ComboItemText $renderBackendCombo $baselineRenderBackendIndex

  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }

  $runningStart = Start-SessionAndWait -hwnd $hwnd -startButton $startButton -stopButton $stopButton -startButtonId $startButtonId -additionalCheck {
    (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
    (-not (Get-ControlEnabledSafe $captureCombo)) -and
    (-not (Get-ControlEnabledSafe $renderCombo)) -and
    (-not [string]::IsNullOrWhiteSpace((Get-ComboSelectionText $renderBackendCombo)))
  } -maxPolls $maxPolls
  $started = $runningStart.Started
  $runningBaselineSeen = $runningStart.RunningSeen

  Invoke-ComboSelectionChangeSync $hwnd $renderBackendComboId 1
  $backendSeen = $false
  $summarySeen = $false
  $diagnosticsSeen = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $backendText = Get-ComboSelectionText $renderBackendCombo
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $backendSeen = $backendText -eq "WAVE API"
    $summarySeen =
      $summaryText.IndexOf("Device selection follows current system defaults; manual device picks are inactive", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    if ($backendSeen -and $summarySeen -and $diagnosticsSeen -and $runningPreserved) {
      break
    }
  }

  if ($baselineRenderBackendIndex -ne 1) {
    Invoke-ComboSelectionChangeSync $hwnd $renderBackendComboId $baselineRenderBackendIndex
    for ($i = 0; $i -lt $maxPolls; $i++) {
      Start-Sleep -Milliseconds 100
      $restoredBackendText = Get-ComboSelectionText $renderBackendCombo
      if ($restoredBackendText -eq $baselineBackendText) {
        break
      }
    }
  }
  $backendRestored = Wait-ForComboSelection $hwnd $renderBackendComboId $baselineBackendText $maxPolls
  if ($baselineFollowDefaults -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }

  $restored = $false
  $finalBackendText = ""
  $finalFollowDefaultsState = -1
  $finalCaptureEnabled = $false
  $finalRenderEnabled = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $backendText = Get-ComboSelectionText $renderBackendCombo
    $finalBackendText = $backendText
    $finalFollowDefaultsState = Get-CheckState $followDefaultsCheckbox
    $finalCaptureEnabled = [Native]::IsWindowEnabled($captureCombo)
    $finalRenderEnabled = [Native]::IsWindowEnabled($renderCombo)
    $deviceStateMatches = $false
    if ($finalFollowDefaultsState -eq 1) {
      $deviceStateMatches =
        (-not $finalCaptureEnabled) -and
        (-not $finalRenderEnabled)
    } else {
      $deviceStateMatches =
        $finalCaptureEnabled -and
        $finalRenderEnabled
    }
    if ($backendText -eq $baselineBackendText -and
        $finalFollowDefaultsState -eq $baselineFollowDefaults -and
        $deviceStateMatches) {
      $restored = $true
      break
    }
  }

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    RunningBaselineSeen = $runningBaselineSeen
    BackendSeen = $backendSeen
    SummarySeen = $summarySeen
    DiagnosticsSeen = $diagnosticsSeen
    RunningPreserved = $runningPreserved
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    FinalBackendText = $finalBackendText
    FinalFollowDefaultsState = $finalFollowDefaultsState
    FinalCaptureEnabled = $finalCaptureEnabled
    FinalRenderEnabled = $finalRenderEnabled
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-FollowDefaultsCaptureBackendWhileRunningCheck([IntPtr]$hwnd, [int]$captureBackendComboId, [int]$followDefaultsCheckboxId, [int]$captureComboId, [int]$renderComboId, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 120) {
  $captureBackendCombo = [Native]::GetDlgItem($hwnd, $captureBackendComboId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904
  $cbGetCurSel = 0x0147

  $baselineCaptureBackendIndex = [int][Native]::SendMessage($captureBackendCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineFollowDefaults = Get-CheckState $followDefaultsCheckbox
  $baselineBackendText = Get-ComboItemText $captureBackendCombo $baselineCaptureBackendIndex

  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }

  $runningStart = Start-SessionAndWait -hwnd $hwnd -startButton $startButton -stopButton $stopButton -startButtonId $startButtonId -additionalCheck {
    (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
    (-not (Get-ControlEnabledSafe $captureCombo)) -and
    (-not (Get-ControlEnabledSafe $renderCombo)) -and
    (-not [string]::IsNullOrWhiteSpace((Get-ComboSelectionText $captureBackendCombo)))
  } -maxPolls $maxPolls
  $started = $runningStart.Started
  $runningBaselineSeen = $runningStart.RunningSeen

  Invoke-ComboSelectionChange $hwnd $captureBackendComboId 1
  $backendSeen = $false
  $summarySeen = $false
  $diagnosticsSeen = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $backendText = Get-ComboSelectionText $captureBackendCombo
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $backendSeen = $backendText -eq "WAVE API"
    $summarySeen =
      $summaryText.IndexOf("Device selection follows current system defaults; manual device picks are inactive", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    if ($backendSeen -and $summarySeen -and $diagnosticsSeen -and $runningPreserved) {
      break
    }
  }

  if ($baselineCaptureBackendIndex -ne 1) {
    Invoke-ComboSelectionChange $hwnd $captureBackendComboId $baselineCaptureBackendIndex
    for ($i = 0; $i -lt $maxPolls; $i++) {
      Start-Sleep -Milliseconds 100
      $restoredBackendText = Get-ComboSelectionText $captureBackendCombo
      if ($restoredBackendText -eq $baselineBackendText) {
        break
      }
    }
  }
  $backendRestored = Wait-ForComboSelection $hwnd $captureBackendComboId $baselineBackendText $maxPolls
  if ($baselineFollowDefaults -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }

  $restored = $false
  $finalBackendText = ""
  $finalFollowDefaultsState = -1
  $finalCaptureEnabled = $false
  $finalRenderEnabled = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $backendText = Get-ComboSelectionText $captureBackendCombo
    $finalBackendText = $backendText
    $finalFollowDefaultsState = Get-CheckState $followDefaultsCheckbox
    $finalCaptureEnabled = [Native]::IsWindowEnabled($captureCombo)
    $finalRenderEnabled = [Native]::IsWindowEnabled($renderCombo)
    $deviceStateMatches = $false
    if ($finalFollowDefaultsState -eq 1) {
      $deviceStateMatches =
        (-not $finalCaptureEnabled) -and
        (-not $finalRenderEnabled)
    } else {
      $deviceStateMatches =
        $finalCaptureEnabled -and
        $finalRenderEnabled
    }
    if ($backendText -eq $baselineBackendText -and
        $finalFollowDefaultsState -eq $baselineFollowDefaults -and
        $deviceStateMatches) {
      $restored = $true
      break
    }
  }

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    RunningBaselineSeen = $runningBaselineSeen
    BackendSeen = $backendSeen
    SummarySeen = $summarySeen
    DiagnosticsSeen = $diagnosticsSeen
    RunningPreserved = $runningPreserved
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    FinalBackendText = $finalBackendText
    FinalFollowDefaultsState = $finalFollowDefaultsState
    FinalCaptureEnabled = $finalCaptureEnabled
    FinalRenderEnabled = $finalRenderEnabled
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-FollowDefaultsMonitorOffWhileRunningCheck([IntPtr]$hwnd, [int]$followDefaultsCheckboxId, [int]$monitorCheckboxId, [int]$captureComboId, [int]$renderComboId, [int]$renderBackendComboId, [int]$renderRateComboId, [int]$renderChannelsComboId, [int]$renderTypeComboId, [int]$renderShareComboId, [int]$renderDriveComboId, [int]$renderBufferEditId, [int]$delayEditId, [int]$autoAlignCheckboxId, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 120) {
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $monitorCheckbox = [Native]::GetDlgItem($hwnd, $monitorCheckboxId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $renderBackendCombo = [Native]::GetDlgItem($hwnd, $renderBackendComboId)
  $renderRateCombo = [Native]::GetDlgItem($hwnd, $renderRateComboId)
  $renderChannelsCombo = [Native]::GetDlgItem($hwnd, $renderChannelsComboId)
  $renderTypeCombo = [Native]::GetDlgItem($hwnd, $renderTypeComboId)
  $renderShareCombo = [Native]::GetDlgItem($hwnd, $renderShareComboId)
  $renderDriveCombo = [Native]::GetDlgItem($hwnd, $renderDriveComboId)
  $renderBufferEdit = [Native]::GetDlgItem($hwnd, $renderBufferEditId)
  $delayEdit = [Native]::GetDlgItem($hwnd, $delayEditId)
  $autoAlignCheckbox = [Native]::GetDlgItem($hwnd, $autoAlignCheckboxId)
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $baselineFollowDefaults = Get-CheckState $followDefaultsCheckbox
  $baselineMonitor = Get-CheckState $monitorCheckbox
  $baselineAutoAlign = Get-CheckState $autoAlignCheckbox

  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }
  if ((Get-CheckState $monitorCheckbox) -ne 1) {
    Invoke-CheckboxClick $monitorCheckbox
    Start-Sleep -Milliseconds 200
  }
  $normalizedAutoAlign = Set-CheckboxState $autoAlignCheckbox 0 $maxPolls

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton) -and
        (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
        (-not [Native]::IsWindowEnabled($captureCombo)) -and
        (-not [Native]::IsWindowEnabled($renderCombo))) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $monitorCheckbox
  $summarySeen = $false
  $diagnosticsSeen = $false
  $renderSurfaceDisabledSeen = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $summarySeen =
      $summaryText.IndexOf("Device selection follows current system defaults; manual device picks are inactive", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Render monitor playback is turned off for the next rebuilt or restarted session; the already-active render stream is still running.", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Render monitor playback is disabled in the current configuration, but the already-active session still has render monitoring until the next rebuild or restart.", [System.StringComparison]::Ordinal) -ge 0
    $renderSurfaceDisabledSeen =
      (-not [Native]::IsWindowEnabled($renderBackendCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo)) -and
      (-not [Native]::IsWindowEnabled($renderRateCombo)) -and
      (-not [Native]::IsWindowEnabled($renderChannelsCombo)) -and
      (-not [Native]::IsWindowEnabled($renderTypeCombo)) -and
      (-not [Native]::IsWindowEnabled($renderShareCombo)) -and
      (-not [Native]::IsWindowEnabled($renderDriveCombo)) -and
      (-not [Native]::IsWindowEnabled($renderBufferEdit)) -and
      (-not [Native]::IsWindowEnabled($delayEdit)) -and
      (-not [Native]::IsWindowEnabled($autoAlignCheckbox))
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    if ($summarySeen -and $diagnosticsSeen -and $renderSurfaceDisabledSeen -and $runningPreserved) {
      break
    }
  }

  if ($baselineMonitor -ne (Get-CheckState $monitorCheckbox)) {
    Invoke-CheckboxClick $monitorCheckbox
  }
  if ($baselineFollowDefaults -ne (Get-CheckState $followDefaultsCheckbox)) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }
  $monitorRestored = Wait-ForCheckboxState $monitorCheckbox $baselineMonitor $maxPolls
  $followDefaultsRestored = Wait-ForCheckboxState $followDefaultsCheckbox $baselineFollowDefaults $maxPolls
  $autoAlignRestored = Set-CheckboxState $autoAlignCheckbox $baselineAutoAlign $maxPolls

  $restored = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $deviceStateMatches = $false
    if ((Get-CheckState $followDefaultsCheckbox) -eq 1) {
      $deviceStateMatches =
        (-not [Native]::IsWindowEnabled($captureCombo)) -and
        (-not [Native]::IsWindowEnabled($renderCombo))
    } else {
      $deviceStateMatches =
        [Native]::IsWindowEnabled($captureCombo) -and
        [Native]::IsWindowEnabled($renderCombo)
    }
    if ((Get-CheckState $monitorCheckbox) -eq $baselineMonitor -and
        (Get-CheckState $followDefaultsCheckbox) -eq $baselineFollowDefaults -and
        $deviceStateMatches) {
      $restored = $true
      break
    }
  }

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    NormalizedAutoAlign = $normalizedAutoAlign
    RunningBaselineSeen = $runningBaselineSeen
    SummarySeen = $summarySeen
    DiagnosticsSeen = $diagnosticsSeen
    RenderSurfaceDisabledSeen = $renderSurfaceDisabledSeen
    RunningPreserved = $runningPreserved
    MonitorRestored = $monitorRestored
    FollowDefaultsRestored = $followDefaultsRestored
    AutoAlignRestored = $autoAlignRestored
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-RenderBackendWhileRunningCheck([IntPtr]$hwnd, [int]$renderBackendComboId, [int]$renderComboId, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 120) {
  $renderBackendCombo = [Native]::GetDlgItem($hwnd, $renderBackendComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904
  $cbGetCurSel = 0x0147

  $baselineRenderBackendIndex = [int][Native]::SendMessage($renderBackendCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineRenderSelected = Get-ComboSelectionText $renderCombo
  $baselineRenderItems = Get-ComboItems $renderCombo
  $oldActiveRenderId = ""

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ($oldActiveRenderId -eq "") {
      $oldActiveRenderIdMatch = [regex]::Match((Get-ControlTextById $hwnd $diagnosticsControlId), "Active session requested render id: (.+)")
      if ($oldActiveRenderIdMatch.Success) {
        $oldActiveRenderId = $oldActiveRenderIdMatch.Groups[1].Value.Trim()
      }
    }
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $renderBackendComboId 1
  $backendSeen = $false
  $runningPreserved = $false
  $summarySeen = $false
  $summaryDriftSeen = $false
  $diagnosticsSeen = $false
  $selectedRenderIdSeen = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $backendText = Get-ComboSelectionText $renderBackendCombo
    $renderSelected = Get-ComboSelectionText $renderCombo
    $renderItems = Get-ComboItems $renderCombo
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $backendSeen =
      $backendText -eq "WAVE API" -and
      $renderSelected -ne "" -and
      $renderItems.Count -gt 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    $summarySeen =
      $summaryText.IndexOf("Render: WAVE API /", [System.StringComparison]::Ordinal) -ge 0
    $summaryDriftSeen =
      $summaryText.IndexOf("Running session note: the current render device pick applies to the next rebuilt or restarted session, while the already-active stream still uses the previous render device.", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Active render mode: WASAPI Shared / Event", [System.StringComparison]::Ordinal) -ge 0
    $selectedRenderIdMatch = [regex]::Match($diagnosticsText, "Selected render id: (.+)")
    $selectedRenderIdSeen =
      $selectedRenderIdMatch.Success -and
      ($selectedRenderIdMatch.Groups[1].Value.Trim() -ne "") -and
      ($oldActiveRenderId -eq "" -or
       $selectedRenderIdMatch.Groups[1].Value.Trim() -ne $oldActiveRenderId)
    if ($backendSeen -and $runningPreserved -and $summarySeen -and $summaryDriftSeen -and $diagnosticsSeen -and $selectedRenderIdSeen) {
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $renderBackendComboId $baselineRenderBackendIndex
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $restoredBackendText = Get-ComboSelectionText $renderBackendCombo
    $restoredRenderSelected = Get-ComboSelectionText $renderCombo
    $restoredRenderItems = Get-ComboItems $renderCombo
    if ($restoredBackendText -eq (Get-ComboItemText $renderBackendCombo $baselineRenderBackendIndex) -and
        $restoredRenderSelected -eq $baselineRenderSelected -and
        (Test-StringArrayEquality $restoredRenderItems $baselineRenderItems)) {
      break
    }
  }
  $restored = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $backendText = Get-ComboSelectionText $renderBackendCombo
    $renderSelected = Get-ComboSelectionText $renderCombo
    $renderItems = Get-ComboItems $renderCombo
    if ($backendText -eq (Get-ComboItemText $renderBackendCombo $baselineRenderBackendIndex) -and
        $renderSelected -eq $baselineRenderSelected -and
        (Test-StringArrayEquality $renderItems $baselineRenderItems)) {
      $restored = $true
      break
    }
  }

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    RunningBaselineSeen = $runningBaselineSeen
    BackendSeen = $backendSeen
    RunningPreserved = $runningPreserved
    SummarySeen = $summarySeen
    SummaryDriftSeen = $summaryDriftSeen
    DiagnosticsSeen = $diagnosticsSeen
    SelectedRenderIdSeen = $selectedRenderIdSeen
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-CaptureBackendWhileRunningCheck([IntPtr]$hwnd, [int]$captureBackendComboId, [int]$captureComboId, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 120) {
  $captureBackendCombo = [Native]::GetDlgItem($hwnd, $captureBackendComboId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904
  $cbGetCurSel = 0x0147

  $baselineCaptureBackendIndex = [int][Native]::SendMessage($captureBackendCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineCaptureSelected = Get-ComboSelectionText $captureCombo
  $baselineCaptureItems = Get-ComboItems $captureCombo
  $oldActiveCaptureId = ""

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ($oldActiveCaptureId -eq "") {
      $oldActiveCaptureIdMatch = [regex]::Match((Get-ControlTextById $hwnd $diagnosticsControlId), "Active session requested capture id: (.+)")
      if ($oldActiveCaptureIdMatch.Success) {
        $oldActiveCaptureId = $oldActiveCaptureIdMatch.Groups[1].Value.Trim()
      }
    }
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureBackendComboId 1
  $backendSeen = $false
  $runningPreserved = $false
  $summarySeen = $false
  $summaryDriftSeen = $false
  $diagnosticsSeen = $false
  $selectedCaptureIdSeen = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $backendText = Get-ComboSelectionText $captureBackendCombo
    $captureSelected = Get-ComboSelectionText $captureCombo
    $captureItems = Get-ComboItems $captureCombo
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $backendSeen =
      $backendText -eq "WAVE API" -and
      $captureSelected -ne "" -and
      $captureItems.Count -gt 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    $summarySeen =
      $summaryText.IndexOf("Capture: WAVE API / Microphone /", [System.StringComparison]::Ordinal) -ge 0
    $summaryDriftSeen =
      $summaryText.IndexOf("Running session note: the current capture device pick applies to the next rebuilt or restarted session, while the already-active stream still uses the previous capture device.", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Active capture mode: WASAPI Shared / Event", [System.StringComparison]::Ordinal) -ge 0
    $selectedCaptureIdMatch = [regex]::Match($diagnosticsText, "Selected capture id: (.+)")
    $selectedCaptureIdSeen =
      $selectedCaptureIdMatch.Success -and
      ($selectedCaptureIdMatch.Groups[1].Value.Trim() -ne "") -and
      ($oldActiveCaptureId -eq "" -or
       $selectedCaptureIdMatch.Groups[1].Value.Trim() -ne $oldActiveCaptureId)
    if ($backendSeen -and $runningPreserved -and $summarySeen -and $summaryDriftSeen -and $diagnosticsSeen -and $selectedCaptureIdSeen) {
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureBackendComboId $baselineCaptureBackendIndex
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $restoredBackendText = Get-ComboSelectionText $captureBackendCombo
    $restoredCaptureSelected = Get-ComboSelectionText $captureCombo
    $restoredCaptureItems = Get-ComboItems $captureCombo
    if ($restoredBackendText -eq (Get-ComboItemText $captureBackendCombo $baselineCaptureBackendIndex) -and
        $restoredCaptureSelected -eq $baselineCaptureSelected -and
        (Test-StringArrayEquality $restoredCaptureItems $baselineCaptureItems)) {
      break
    }
  }
  $restored = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $backendText = Get-ComboSelectionText $captureBackendCombo
    $captureSelected = Get-ComboSelectionText $captureCombo
    $captureItems = Get-ComboItems $captureCombo
    if ($backendText -eq (Get-ComboItemText $captureBackendCombo $baselineCaptureBackendIndex) -and
        $captureSelected -eq $baselineCaptureSelected -and
        (Test-StringArrayEquality $captureItems $baselineCaptureItems)) {
      $restored = $true
      break
    }
  }

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    RunningBaselineSeen = $runningBaselineSeen
    BackendSeen = $backendSeen
    RunningPreserved = $runningPreserved
    SummarySeen = $summarySeen
    SummaryDriftSeen = $summaryDriftSeen
    DiagnosticsSeen = $diagnosticsSeen
    SelectedCaptureIdSeen = $selectedCaptureIdSeen
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-ManualDeviceChangeWhileRunningCheck([IntPtr]$hwnd, [int]$captureComboId, [int]$renderComboId, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 120) {
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $captureCount = Get-ComboCount $captureCombo
  $renderCount = Get-ComboCount $renderCombo
  if ($captureCount -lt 2 -or $renderCount -lt 2) {
    return [pscustomobject]@{
      Eligible = $false
      RunningBaselineSeen = $false
      CaptureChangedSeen = $false
      RenderChangedSeen = $false
      SummarySeen = $false
      RunningPreserved = $false
      Restored = $false
      StoppedCleanly = $false
      SummaryText = Get-ControlTextById $hwnd $summaryControlId
      DiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    }
  }

  $cbGetCurSel = 0x0147
  $baselineCaptureIndex = [int][Native]::SendMessage($captureCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineRenderIndex = [int][Native]::SendMessage($renderCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineCaptureText = Get-ComboItemText $captureCombo $baselineCaptureIndex
  $baselineRenderText = Get-ComboItemText $renderCombo $baselineRenderIndex
  $targetCaptureIndex = 1
  $targetRenderIndex = 1
  if ($targetCaptureIndex -eq $baselineCaptureIndex -and $captureCount -gt 2) {
    $targetCaptureIndex = 2
  }
  if ($targetRenderIndex -eq $baselineRenderIndex -and $renderCount -gt 2) {
    $targetRenderIndex = 2
  }
  $targetCaptureText = Get-ComboItemText $captureCombo $targetCaptureIndex
  $targetRenderText = Get-ComboItemText $renderCombo $targetRenderIndex

  $oldActiveCaptureId = ""
  $oldActiveRenderId = ""
  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    if ($oldActiveCaptureId -eq "") {
      $oldActiveCaptureIdMatch = [regex]::Match($diagnosticsText, "Active session requested capture id: (.+)")
      if ($oldActiveCaptureIdMatch.Success) {
        $oldActiveCaptureId = $oldActiveCaptureIdMatch.Groups[1].Value.Trim()
      }
    }
    if ($oldActiveRenderId -eq "") {
      $oldActiveRenderIdMatch = [regex]::Match($diagnosticsText, "Active session requested render id: (.+)")
      if ($oldActiveRenderIdMatch.Success) {
        $oldActiveRenderId = $oldActiveRenderIdMatch.Groups[1].Value.Trim()
      }
    }
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureComboId $targetCaptureIndex
  [void](Wait-ForComboSelection $hwnd $captureComboId $targetCaptureText $maxPolls)
  Invoke-ComboSelectionChange $hwnd $renderComboId $targetRenderIndex
  [void](Wait-ForComboSelection $hwnd $renderComboId $targetRenderText $maxPolls)

  $captureChangedSeen = $false
  $renderChangedSeen = $false
  $summarySeen = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $selectedCaptureIdMatch = [regex]::Match($diagnosticsText, "Selected capture id: (.+)")
    $selectedRenderIdMatch = [regex]::Match($diagnosticsText, "Selected render id: (.+)")
    $captureChangedSeen =
      $selectedCaptureIdMatch.Success -and
      ($selectedCaptureIdMatch.Groups[1].Value.Trim() -ne "") -and
      ($oldActiveCaptureId -eq "" -or
       $selectedCaptureIdMatch.Groups[1].Value.Trim() -ne $oldActiveCaptureId)
    $renderChangedSeen =
      $selectedRenderIdMatch.Success -and
      ($selectedRenderIdMatch.Groups[1].Value.Trim() -ne "") -and
      ($oldActiveRenderId -eq "" -or
       $selectedRenderIdMatch.Groups[1].Value.Trim() -ne $oldActiveRenderId)
    $summarySeen =
      $summaryText.IndexOf("Running session note: current capture/render device picks apply to the next rebuilt or restarted session, while the already-active stream still uses the previous devices.", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      $diagnosticsText.IndexOf("Active session requested capture id: $oldActiveCaptureId", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active session requested render id: $oldActiveRenderId", [System.StringComparison]::Ordinal) -ge 0 -and
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    if ($captureChangedSeen -and $renderChangedSeen -and $summarySeen -and $runningPreserved) {
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureComboId $baselineCaptureIndex
  $captureRestored = Wait-ForComboSelection $hwnd $captureComboId $baselineCaptureText $maxPolls
  Invoke-ComboSelectionChange $hwnd $renderComboId $baselineRenderIndex
  $renderRestored = Wait-ForComboSelection $hwnd $renderComboId $baselineRenderText $maxPolls

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    Eligible = $true
    RunningBaselineSeen = $runningBaselineSeen
    CaptureChangedSeen = $captureChangedSeen
    RenderChangedSeen = $renderChangedSeen
    SummarySeen = $summarySeen
    RunningPreserved = $runningPreserved
    Restored = $captureRestored -and $renderRestored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-ManualDeviceChangeThenRefreshWhileRunningCheck([IntPtr]$hwnd, [int]$captureComboId, [int]$renderComboId, [int]$refreshButtonId, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 120) {
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $refreshButton = [Native]::GetDlgItem($hwnd, $refreshButtonId)
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $diagnosticsControlId = 1904

  $captureCount = Get-ComboCount $captureCombo
  $renderCount = Get-ComboCount $renderCombo
  if ($captureCount -lt 2 -or $renderCount -lt 2) {
    return [pscustomobject]@{
      Eligible = $false
      RunningBaselineSeen = $false
      PersistedSeen = $false
      RunningPreserved = $false
      Restored = $false
      StoppedCleanly = $false
      DiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    }
  }

  $cbGetCurSel = 0x0147
  $baselineCaptureIndex = [int][Native]::SendMessage($captureCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineRenderIndex = [int][Native]::SendMessage($renderCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineCaptureText = Get-ComboItemText $captureCombo $baselineCaptureIndex
  $baselineRenderText = Get-ComboItemText $renderCombo $baselineRenderIndex
  $targetCaptureIndex = 1
  $targetRenderIndex = 1
  if ($targetCaptureIndex -eq $baselineCaptureIndex -and $captureCount -gt 2) {
    $targetCaptureIndex = 2
  }
  if ($targetRenderIndex -eq $baselineRenderIndex -and $renderCount -gt 2) {
    $targetRenderIndex = 2
  }
  $targetCaptureText = Get-ComboItemText $captureCombo $targetCaptureIndex
  $targetRenderText = Get-ComboItemText $renderCombo $targetRenderIndex

  $oldActiveCaptureId = ""
  $oldActiveRenderId = ""
  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    if ($oldActiveCaptureId -eq "") {
      $oldActiveCaptureIdMatch = [regex]::Match($diagnosticsText, "Active session requested capture id: (.+)")
      if ($oldActiveCaptureIdMatch.Success) {
        $oldActiveCaptureId = $oldActiveCaptureIdMatch.Groups[1].Value.Trim()
      }
    }
    if ($oldActiveRenderId -eq "") {
      $oldActiveRenderIdMatch = [regex]::Match($diagnosticsText, "Active session requested render id: (.+)")
      if ($oldActiveRenderIdMatch.Success) {
        $oldActiveRenderId = $oldActiveRenderIdMatch.Groups[1].Value.Trim()
      }
    }
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureComboId $targetCaptureIndex
  [void](Wait-ForComboSelection $hwnd $captureComboId $targetCaptureText $maxPolls)
  Invoke-ComboSelectionChange $hwnd $renderComboId $targetRenderIndex
  [void](Wait-ForComboSelection $hwnd $renderComboId $targetRenderText $maxPolls)
  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$refreshButtonId, [IntPtr]::Zero)

  $persistedSeen = $false
  $runningPreserved = $false
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $capturePersisted = (Get-ComboSelectionText $captureCombo) -eq $targetCaptureText
    $renderPersisted = (Get-ComboSelectionText $renderCombo) -eq $targetRenderText
    $persistedSeen =
      $capturePersisted -and
      $renderPersisted -and
      $diagnosticsText.IndexOf("Selected capture id:", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Selected render id:", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      $diagnosticsText.IndexOf("Active session requested capture id: $oldActiveCaptureId", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active session requested render id: $oldActiveRenderId", [System.StringComparison]::Ordinal) -ge 0 -and
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    if ($persistedSeen -and $runningPreserved) {
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureComboId $baselineCaptureIndex
  $captureRestored = Wait-ForComboSelection $hwnd $captureComboId $baselineCaptureText $maxPolls
  Invoke-ComboSelectionChange $hwnd $renderComboId $baselineRenderIndex
  $renderRestored = Wait-ForComboSelection $hwnd $renderComboId $baselineRenderText $maxPolls

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    Eligible = $true
    RunningBaselineSeen = $runningBaselineSeen
    PersistedSeen = $persistedSeen
    RunningPreserved = $runningPreserved
    Restored = $captureRestored -and $renderRestored
    StoppedCleanly = $stoppedCleanly
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-ProbeUiCheck([IntPtr]$hwnd, [int]$buttonId, [int]$pairedButtonId, [int]$startButtonId, [int]$stopButtonId, [string]$runningTitle, [string]$runningText, [string]$idleText, [object[]]$watchedControls, [int]$maxPolls = 240) {
  $probeBtn=[Native]::GetDlgItem($hwnd,$buttonId)
  $pairedProbeBtn=[Native]::GetDlgItem($hwnd,$pairedButtonId)
  $startBtn=[Native]::GetDlgItem($hwnd,$startButtonId)
  $stopBtn=[Native]::GetDlgItem($hwnd,$stopButtonId)
  $watchedHandles=[ordered]@{}
  $watchedDisabledSeen=[ordered]@{}
  $watchedRestored=[ordered]@{}
  $baselineEnabled=[ordered]@{}
  $baselineValue=[ordered]@{}
  $finalEnabled=[ordered]@{}
  $finalValue=[ordered]@{}
  $availabilityIssues = New-Object System.Collections.Generic.List[string]
  foreach($control in $watchedControls){
    $handle=[Native]::GetDlgItem($hwnd,[int]$control.Id)
    $watchedHandles[$control.Name]=$handle
    if($handle -eq [IntPtr]::Zero){
      $availabilityIssues.Add("$($control.Name)=missing")
      continue
    }
    if(-not [Native]::IsWindowEnabled($handle)){
      $availabilityIssues.Add("$($control.Name)=initially-disabled")
    }
    $snapshot = Get-ControlSnapshot $handle $control.Type
    $baselineEnabled[$control.Name] = $snapshot.Enabled
    $baselineValue[$control.Name] = $snapshot.Value
    $watchedDisabledSeen[$control.Name]=$false
    $watchedRestored[$control.Name]=$false
  }
  [void][Native]::PostMessage($hwnd,0x0111,[IntPtr]$buttonId,[IntPtr]::Zero)
  $titleBusySeen=$false
  $probeTextBusySeen=$false
  $probeDisabledSeen=$false
  $pairedProbeDisabledSeen=$false
  $startDisabledSeen=$false
  $stopStayedDisabled=$true
  $restored=$false
  $baselineRestored=$false
  for($i=0;$i -lt $maxPolls;$i++){
    Start-Sleep -Milliseconds 100
    $title=Get-Text $hwnd
    $btnText=Get-Text $probeBtn
    $probeEnabled=[Native]::IsWindowEnabled($probeBtn)
    $pairedProbeEnabled = if ($pairedProbeBtn -eq [IntPtr]::Zero) { $true } else { [Native]::IsWindowEnabled($pairedProbeBtn) }
    $startEnabled=[Native]::IsWindowEnabled($startBtn)
    $stopEnabled = if ($stopBtn -eq [IntPtr]::Zero) { $false } else { [Native]::IsWindowEnabled($stopBtn) }
    $currentEnabled=[ordered]@{}
    $currentValue=[ordered]@{}
    if($title -like "*$runningTitle*"){ $titleBusySeen=$true }
    if($btnText -eq $runningText){ $probeTextBusySeen=$true }
    if(-not $probeEnabled){ $probeDisabledSeen=$true }
    if($pairedProbeBtn -ne [IntPtr]::Zero -and -not $pairedProbeEnabled){ $pairedProbeDisabledSeen=$true }
    if(-not $startEnabled){ $startDisabledSeen=$true }
    if($stopEnabled){ $stopStayedDisabled=$false }
    foreach($control in $watchedControls){
      $handle = $watchedHandles[$control.Name]
      if($handle -eq [IntPtr]::Zero){
        continue
      }
      $snapshot = Get-ControlSnapshot $handle $control.Type
      $currentEnabled[$control.Name] = $snapshot.Enabled
      $currentValue[$control.Name] = $snapshot.Value
      if(-not $snapshot.Enabled){
        $watchedDisabledSeen[$control.Name]=$true
      }
      if($watchedDisabledSeen[$control.Name] -and
         $snapshot.Enabled -eq $baselineEnabled[$control.Name] -and
         $snapshot.Value -eq $baselineValue[$control.Name]){
        $watchedRestored[$control.Name]=$true
      }
    }
    if(($titleBusySeen -or $probeTextBusySeen -or $probeDisabledSeen) -and
       $probeEnabled -and $btnText -eq $idleText -and
       $title -notlike "*$runningTitle*" -and
       (Test-SnapshotEquality $baselineEnabled $currentEnabled) -and
       (Test-SnapshotEquality $baselineValue $currentValue)){
      $baselineRestored=$true
      $finalEnabled = $currentEnabled
      $finalValue = $currentValue
      $restored=$true
      break
    }
    foreach($entry in $watchedHandles.GetEnumerator()){
      if($entry.Value -eq [IntPtr]::Zero){
        continue
      }
      if(-not $finalEnabled.Contains($entry.Key) -and $currentEnabled.Contains($entry.Key)){
        $finalEnabled[$entry.Key] = $currentEnabled[$entry.Key]
      }
      if(-not $finalValue.Contains($entry.Key) -and $currentValue.Contains($entry.Key)){
        $finalValue[$entry.Key] = $currentValue[$entry.Key]
      }
    }
  }
  if (-not $baselineRestored) {
    foreach($control in $watchedControls){
      $handle = $watchedHandles[$control.Name]
      if($handle -eq [IntPtr]::Zero){
        continue
      }
      $snapshot = Get-ControlSnapshot $handle $control.Type
      $finalEnabled[$control.Name] = $snapshot.Enabled
      $finalValue[$control.Name] = $snapshot.Value
    }
  }
  [pscustomobject]@{
    TitleBusySeen = $titleBusySeen
    ProbeTextBusySeen = $probeTextBusySeen
    ProbeDisabledSeen = $probeDisabledSeen
    PairedProbeDisabledSeen = $pairedProbeDisabledSeen
    StartDisabledSeen = $startDisabledSeen
    StopStayedDisabled = $stopStayedDisabled
    WatchedDisabledSeen = $watchedDisabledSeen
    WatchedRestored = $watchedRestored
    BaselineEnabled = $baselineEnabled
    BaselineValue = $baselineValue
    FinalEnabled = $finalEnabled
    FinalValue = $finalValue
    BaselineRestored = $baselineRestored
    AvailabilityIssues = @($availabilityIssues)
    Restored = $restored
  }
}
function Invoke-RunningSessionProbeCycleCheck([IntPtr]$hwnd, [int]$probeButtonId, [int]$pairedProbeButtonId, [int]$startButtonId, [int]$stopButtonId, [string]$runningProbeTitle, [string]$runningProbeText, [string]$idleProbeText, [object[]]$watchedControls, [int]$maxPolls = 240) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)

  $started = $false
  $runningBaselineSeen = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $title = Get-Text $hwnd
    if ($title -like "*| Running |*" -and
        (-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningBaselineSeen = $true
      break
    }
  }

  $probeCycle = Invoke-ProbeUiCheck $hwnd $probeButtonId $pairedProbeButtonId $startButtonId $stopButtonId $runningProbeTitle $runningProbeText $idleProbeText $watchedControls $maxPolls
  $stopAvailableSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ([Native]::IsWindowEnabled($stopButton)) {
      $stopAvailableSeen = $true
      break
    }
  }
  $runningRestored = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $title = Get-Text $hwnd
    if ($title -like "*| Running |*" -and
        (-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningRestored = $true
      break
    }
  }

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    RunningBaselineSeen = $runningBaselineSeen
    ProbeCycle = $probeCycle
    StopAvailableSeen = $stopAvailableSeen
    RunningRestored = $runningRestored
    StoppedCleanly = $stoppedCleanly
  }
}
function Invoke-RefreshWhileRunningCheck([IntPtr]$hwnd, [int]$refreshButtonId, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 120) {
  $refreshButton = [Native]::GetDlgItem($hwnd, $refreshButtonId)
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $summaryControlId = 1903

  $started = $false
  $runningBaselineSeen = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $baselineTitle = ""
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $title = Get-Text $hwnd
    if ($title -like "*| Running |*" -and
        (-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $baselineTitle = $title
      $runningBaselineSeen = $true
      break
    }
  }

  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$refreshButtonId, [IntPtr]::Zero)
  $runningPreserved = $false
  $titlePreserved = $false
  $runningNoteSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $title = Get-Text $hwnd
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    if ($title -eq $baselineTitle) {
      $titlePreserved = $true
    }
    if ($summaryText.IndexOf("Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.", [System.StringComparison]::Ordinal) -ge 0) {
      $runningNoteSeen = $true
    }
    if ($title -like "*| Running |*" -and
        (-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton) -and
        $titlePreserved -and
        $runningNoteSeen) {
      $runningPreserved = $true
      break
    }
  }

  $refreshStayedEnabled = [Native]::IsWindowEnabled($refreshButton)
  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    RunningBaselineSeen = $runningBaselineSeen
    RunningPreserved = $runningPreserved
    TitlePreserved = $titlePreserved
    RunningNoteSeen = $runningNoteSeen
    RefreshStayedEnabled = $refreshStayedEnabled
    StoppedCleanly = $stoppedCleanly
  }
}
function Invoke-ManualDeviceSelectionPersistenceCheck([IntPtr]$hwnd, [int]$captureComboId, [int]$renderComboId, [int]$refreshButtonId, [int]$maxPolls = 120) {
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $diagnosticsControlId = 1904

  $captureCount = Get-ComboCount $captureCombo
  $renderCount = Get-ComboCount $renderCombo
  if ($captureCount -lt 2 -or $renderCount -lt 2) {
    return [pscustomobject]@{
      Eligible = $false
      CaptureSelectedSeen = $false
      RenderSelectedSeen = $false
      CapturePersisted = $false
      RenderPersisted = $false
      Restored = $false
      DiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    }
  }

  $cbGetCurSel = 0x0147
  $baselineCaptureIndex = [int][Native]::SendMessage($captureCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineRenderIndex = [int][Native]::SendMessage($renderCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $targetCaptureIndex = 1
  $targetRenderIndex = 1
  $targetCaptureText = Get-ComboItemText $captureCombo $targetCaptureIndex
  $targetRenderText = Get-ComboItemText $renderCombo $targetRenderIndex

  Invoke-ComboSelectionChange $hwnd $captureComboId $targetCaptureIndex
  $captureSelectedSeen = Wait-ForComboSelection $hwnd $captureComboId $targetCaptureText $maxPolls
  Invoke-ComboSelectionChange $hwnd $renderComboId $targetRenderIndex
  $renderSelectedSeen = Wait-ForComboSelection $hwnd $renderComboId $targetRenderText $maxPolls
  $beforeDiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId

  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$refreshButtonId, [IntPtr]::Zero)

  $capturePersisted = $false
  $renderPersisted = $false
  $observedCaptureText = ""
  $observedRenderText = ""
  $afterDiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $afterDiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $observedCaptureText = Get-ComboSelectionText $captureCombo
    $observedRenderText = Get-ComboSelectionText $renderCombo
    $capturePersisted = $observedCaptureText -eq $targetCaptureText
    $renderPersisted = $observedRenderText -eq $targetRenderText
    if ($capturePersisted -and $renderPersisted) {
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureComboId $baselineCaptureIndex
  $captureRestored = Wait-ForComboSelection $hwnd $captureComboId (Get-ComboItemText $captureCombo $baselineCaptureIndex) $maxPolls
  Invoke-ComboSelectionChange $hwnd $renderComboId $baselineRenderIndex
  $renderRestored = Wait-ForComboSelection $hwnd $renderComboId (Get-ComboItemText $renderCombo $baselineRenderIndex) $maxPolls

  [pscustomobject]@{
    Eligible = $true
    CaptureSelectedSeen = $captureSelectedSeen
    RenderSelectedSeen = $renderSelectedSeen
    CapturePersisted = $capturePersisted
    RenderPersisted = $renderPersisted
    Restored = $captureRestored -and $renderRestored
    DiagnosticsText = $afterDiagnosticsText
  }
}
function Invoke-RunningCaptureConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$captureSampleRateComboId, [int]$autoAlignCheckboxId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $captureSampleRateCombo = [Native]::GetDlgItem($hwnd, $captureSampleRateComboId)
  $autoAlignCheckbox = [Native]::GetDlgItem($hwnd, $autoAlignCheckboxId)
  $diagnosticsControlId = 1904
  $baselineAutoAlignCheck = Get-CheckState $autoAlignCheckbox

  $baselineIndex = Find-ComboItemIndex $captureSampleRateCombo "48000"
  $driftIndex = Find-ComboItemIndex $captureSampleRateCombo "44100"
  if ($baselineIndex -lt 0 -or $driftIndex -lt 0) {
    return [pscustomobject]@{
      BaselineSet = $false
      NormalizedAutoAlign = $false
      RunningBaselineSeen = $false
      DriftSeen = $false
      RequestedPreserved = $false
      RunningPreserved = $false
      Restored = $false
      AutoAlignRestored = $false
      StoppedCleanly = $false
      DiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureSampleRateComboId $baselineIndex
  $captureRateSet = Wait-ForComboSelection $hwnd $captureSampleRateComboId "48000" $maxPolls
  $normalizedAutoAlign = Set-CheckboxState $autoAlignCheckbox 0 $maxPolls
  $baselineSet = $captureRateSet -and $normalizedAutoAlign

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureSampleRateComboId $driftIndex
  $driftSeen = $false
  $requestedPreserved = $false
  $runningPreserved = $false
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $driftSeen =
      $diagnosticsText.IndexOf("Current configured capture: 44100 Hz / 2 ch / Float32", [System.StringComparison]::Ordinal) -ge 0
    $requestedPreserved =
      $diagnosticsText.IndexOf("Active session requested capture: 48000 Hz / 2 ch / Float32", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    if ($driftSeen -and $requestedPreserved -and $runningPreserved) {
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureSampleRateComboId $baselineIndex
  $captureRateRestored = Wait-ForComboSelection $hwnd $captureSampleRateComboId "48000" $maxPolls
  $autoAlignRestored = Set-CheckboxState $autoAlignCheckbox $baselineAutoAlignCheck $maxPolls
  $restored = $captureRateRestored -and $autoAlignRestored

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    BaselineSet = $baselineSet
    NormalizedAutoAlign = $normalizedAutoAlign
    RunningBaselineSeen = $runningBaselineSeen
    DriftSeen = $driftSeen
    RequestedPreserved = $requestedPreserved
    RunningPreserved = $runningPreserved
    Restored = $restored
    AutoAlignRestored = $autoAlignRestored
    StoppedCleanly = $stoppedCleanly
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-RunningRenderConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$renderSampleRateComboId, [int]$autoAlignCheckboxId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $renderSampleRateCombo = [Native]::GetDlgItem($hwnd, $renderSampleRateComboId)
  $autoAlignCheckbox = [Native]::GetDlgItem($hwnd, $autoAlignCheckboxId)
  $diagnosticsControlId = 1904
  $baselineAutoAlignCheck = Get-CheckState $autoAlignCheckbox

  $baselineIndex = Find-ComboItemIndex $renderSampleRateCombo "48000"
  $driftIndex = Find-ComboItemIndex $renderSampleRateCombo "44100"
  if ($baselineIndex -lt 0 -or $driftIndex -lt 0) {
    return [pscustomobject]@{
      BaselineSet = $false
      NormalizedAutoAlign = $false
      RunningBaselineSeen = $false
      DriftSeen = $false
      RequestedPreserved = $false
      RunningPreserved = $false
      Restored = $false
      AutoAlignRestored = $false
      StoppedCleanly = $false
      DiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    }
  }

  Invoke-ComboSelectionChange $hwnd $renderSampleRateComboId $baselineIndex
  $renderRateSet = Wait-ForComboSelection $hwnd $renderSampleRateComboId "48000" $maxPolls
  $normalizedAutoAlign = Set-CheckboxState $autoAlignCheckbox 0 $maxPolls
  $baselineSet = $renderRateSet -and $normalizedAutoAlign

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $renderSampleRateComboId $driftIndex
  $driftSeen = $false
  $requestedPreserved = $false
  $runningPreserved = $false
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $driftSeen =
      $diagnosticsText.IndexOf("Current configured render: 44100 Hz / 2 ch / Float32", [System.StringComparison]::Ordinal) -ge 0
    $requestedPreserved =
      $diagnosticsText.IndexOf("Active session requested render: 48000 Hz / 2 ch / Float32", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    if ($driftSeen -and $requestedPreserved -and $runningPreserved) {
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $renderSampleRateComboId $baselineIndex
  $renderRateRestored = Wait-ForComboSelection $hwnd $renderSampleRateComboId "48000" $maxPolls
  $autoAlignRestored = Set-CheckboxState $autoAlignCheckbox $baselineAutoAlignCheck $maxPolls
  $restored = $renderRateRestored -and $autoAlignRestored

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    BaselineSet = $baselineSet
    NormalizedAutoAlign = $normalizedAutoAlign
    RunningBaselineSeen = $runningBaselineSeen
    DriftSeen = $driftSeen
    RequestedPreserved = $requestedPreserved
    RunningPreserved = $runningPreserved
    Restored = $restored
    AutoAlignRestored = $autoAlignRestored
    StoppedCleanly = $stoppedCleanly
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-FollowDefaultsRunningCaptureConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$followDefaultsCheckboxId, [int]$captureComboId, [int]$renderComboId, [int]$captureSampleRateComboId, [int]$autoAlignCheckboxId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $captureSampleRateCombo = [Native]::GetDlgItem($hwnd, $captureSampleRateComboId)
  $autoAlignCheckbox = [Native]::GetDlgItem($hwnd, $autoAlignCheckboxId)
  $diagnosticsControlId = 1904
  $baselineFollowDefaults = Get-CheckState $followDefaultsCheckbox
  $baselineAutoAlignCheck = Get-CheckState $autoAlignCheckbox

  $baselineIndex = Find-ComboItemIndex $captureSampleRateCombo "48000"
  $driftIndex = Find-ComboItemIndex $captureSampleRateCombo "44100"
  if ($baselineIndex -lt 0 -or $driftIndex -lt 0) {
    return [pscustomobject]@{
      BaselineSet = $false
      NormalizedAutoAlign = $false
      RunningBaselineSeen = $false
      DriftSeen = $false
      RequestedPreserved = $false
      RunningPreserved = $false
      FollowDefaultsRestored = $false
      Restored = $false
      AutoAlignRestored = $false
      StoppedCleanly = $false
      DiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    }
  }

  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }
  Invoke-ComboSelectionChange $hwnd $captureSampleRateComboId $baselineIndex
  $captureRateSet = Wait-ForComboSelection $hwnd $captureSampleRateComboId "48000" $maxPolls
  $normalizedAutoAlign = Set-CheckboxState $autoAlignCheckbox 0 $maxPolls
  $baselineSet = $captureRateSet -and $normalizedAutoAlign

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton) -and
        (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
        (-not [Native]::IsWindowEnabled($captureCombo)) -and
        (-not [Native]::IsWindowEnabled($renderCombo))) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureSampleRateComboId $driftIndex
  $driftSeen = $false
  $requestedPreserved = $false
  $runningPreserved = $false
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $driftSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Current configured capture: 44100 Hz / 2 ch / Float32", [System.StringComparison]::Ordinal) -ge 0
    $requestedPreserved =
      $diagnosticsText.IndexOf("Active session requested capture: 48000 Hz / 2 ch / Float32", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    if ($driftSeen -and $requestedPreserved -and $runningPreserved) {
      break
    }
  }

  if ($baselineFollowDefaults -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }
  $followDefaultsRestored = Wait-ForCheckboxState $followDefaultsCheckbox $baselineFollowDefaults $maxPolls
  Invoke-ComboSelectionChange $hwnd $captureSampleRateComboId $baselineIndex
  $captureRateRestored = Wait-ForComboSelection $hwnd $captureSampleRateComboId "48000" $maxPolls
  $autoAlignRestored = Set-CheckboxState $autoAlignCheckbox $baselineAutoAlignCheck $maxPolls
  $restored = $followDefaultsRestored -and $captureRateRestored -and $autoAlignRestored

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    BaselineSet = $baselineSet
    NormalizedAutoAlign = $normalizedAutoAlign
    RunningBaselineSeen = $runningBaselineSeen
    DriftSeen = $driftSeen
    RequestedPreserved = $requestedPreserved
    RunningPreserved = $runningPreserved
    FollowDefaultsRestored = $followDefaultsRestored
    Restored = $restored
    AutoAlignRestored = $autoAlignRestored
    StoppedCleanly = $stoppedCleanly
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-FollowDefaultsRunningRenderConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$followDefaultsCheckboxId, [int]$captureComboId, [int]$renderComboId, [int]$renderSampleRateComboId, [int]$autoAlignCheckboxId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $renderSampleRateCombo = [Native]::GetDlgItem($hwnd, $renderSampleRateComboId)
  $autoAlignCheckbox = [Native]::GetDlgItem($hwnd, $autoAlignCheckboxId)
  $diagnosticsControlId = 1904
  $baselineFollowDefaults = Get-CheckState $followDefaultsCheckbox
  $baselineAutoAlignCheck = Get-CheckState $autoAlignCheckbox

  $baselineIndex = Find-ComboItemIndex $renderSampleRateCombo "48000"
  $driftIndex = Find-ComboItemIndex $renderSampleRateCombo "44100"
  if ($baselineIndex -lt 0 -or $driftIndex -lt 0) {
    return [pscustomobject]@{
      BaselineSet = $false
      NormalizedAutoAlign = $false
      RunningBaselineSeen = $false
      DriftSeen = $false
      RequestedPreserved = $false
      RunningPreserved = $false
      FollowDefaultsRestored = $false
      Restored = $false
      AutoAlignRestored = $false
      StoppedCleanly = $false
      DiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    }
  }

  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }
  Invoke-ComboSelectionChange $hwnd $renderSampleRateComboId $baselineIndex
  $renderRateSet = Wait-ForComboSelection $hwnd $renderSampleRateComboId "48000" $maxPolls
  $normalizedAutoAlign = Set-CheckboxState $autoAlignCheckbox 0 $maxPolls
  $baselineSet = $renderRateSet -and $normalizedAutoAlign

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton) -and
        (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
        (-not [Native]::IsWindowEnabled($captureCombo)) -and
        (-not [Native]::IsWindowEnabled($renderCombo))) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $renderSampleRateComboId $driftIndex
  $driftSeen = $false
  $requestedPreserved = $false
  $runningPreserved = $false
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $driftSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Current configured render: 44100 Hz / 2 ch / Float32", [System.StringComparison]::Ordinal) -ge 0
    $requestedPreserved =
      $diagnosticsText.IndexOf("Active session requested render: 48000 Hz / 2 ch / Float32", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    if ($driftSeen -and $requestedPreserved -and $runningPreserved) {
      break
    }
  }

  if ($baselineFollowDefaults -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }
  $followDefaultsRestored = Wait-ForCheckboxState $followDefaultsCheckbox $baselineFollowDefaults $maxPolls
  Invoke-ComboSelectionChange $hwnd $renderSampleRateComboId $baselineIndex
  $renderRateRestored = Wait-ForComboSelection $hwnd $renderSampleRateComboId "48000" $maxPolls
  $autoAlignRestored = Set-CheckboxState $autoAlignCheckbox $baselineAutoAlignCheck $maxPolls
  $restored = $followDefaultsRestored -and $renderRateRestored -and $autoAlignRestored

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    BaselineSet = $baselineSet
    NormalizedAutoAlign = $normalizedAutoAlign
    RunningBaselineSeen = $runningBaselineSeen
    DriftSeen = $driftSeen
    RequestedPreserved = $requestedPreserved
    RunningPreserved = $runningPreserved
    FollowDefaultsRestored = $followDefaultsRestored
    Restored = $restored
    AutoAlignRestored = $autoAlignRestored
    StoppedCleanly = $stoppedCleanly
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-RunningAutoAlignConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$captureSampleRateComboId, [int]$captureChannelsComboId, [int]$captureTypeComboId, [int]$renderSampleRateComboId, [int]$renderChannelsComboId, [int]$renderTypeComboId, [int]$autoAlignCheckboxId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $captureSampleRateCombo = [Native]::GetDlgItem($hwnd, $captureSampleRateComboId)
  $captureChannelsCombo = [Native]::GetDlgItem($hwnd, $captureChannelsComboId)
  $captureTypeCombo = [Native]::GetDlgItem($hwnd, $captureTypeComboId)
  $renderSampleRateCombo = [Native]::GetDlgItem($hwnd, $renderSampleRateComboId)
  $renderChannelsCombo = [Native]::GetDlgItem($hwnd, $renderChannelsComboId)
  $renderTypeCombo = [Native]::GetDlgItem($hwnd, $renderTypeComboId)
  $autoAlignCheckbox = [Native]::GetDlgItem($hwnd, $autoAlignCheckboxId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904
  $cbGetCurSel = 0x0147

  $originalCaptureRate = [int][Native]::SendMessage($captureSampleRateCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $originalCaptureChannels = [int][Native]::SendMessage($captureChannelsCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $originalCaptureType = [int][Native]::SendMessage($captureTypeCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $originalRenderRate = [int][Native]::SendMessage($renderSampleRateCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $originalRenderChannels = [int][Native]::SendMessage($renderChannelsCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $originalRenderType = [int][Native]::SendMessage($renderTypeCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $originalAutoAlign = Get-CheckState $autoAlignCheckbox

  $baselineCaptureRate = Find-ComboItemIndex $captureSampleRateCombo "44100"
  $baselineCaptureChannels = Find-ComboItemIndex $captureChannelsCombo "1"
  $baselineCaptureType = Find-ComboItemIndex $captureTypeCombo "PCM16"
  $baselineRenderRate = Find-ComboItemIndex $renderSampleRateCombo "48000"
  $baselineRenderChannels = Find-ComboItemIndex $renderChannelsCombo "2"
  $baselineRenderType = Find-ComboItemIndex $renderTypeCombo "Float32"
  if ($baselineCaptureRate -lt 0 -or
      $baselineCaptureChannels -lt 0 -or
      $baselineCaptureType -lt 0 -or
      $baselineRenderRate -lt 0 -or
      $baselineRenderChannels -lt 0 -or
      $baselineRenderType -lt 0) {
    return [pscustomobject]@{
      BaselineSet = $false
      RunningBaselineSeen = $false
      EffectiveRequestSeen = $false
      ActiveRequestPreserved = $false
      RunningPreserved = $false
      Restored = $false
      StoppedCleanly = $false
      DiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureSampleRateComboId $baselineCaptureRate
  $captureRateSet = Wait-ForComboSelection $hwnd $captureSampleRateComboId "44100" $maxPolls
  Invoke-ComboSelectionChange $hwnd $captureChannelsComboId $baselineCaptureChannels
  $captureChannelsSet = Wait-ForComboSelection $hwnd $captureChannelsComboId "1" $maxPolls
  Invoke-ComboSelectionChange $hwnd $captureTypeComboId $baselineCaptureType
  $captureTypeSet = Wait-ForComboSelection $hwnd $captureTypeComboId "PCM16" $maxPolls
  Invoke-ComboSelectionChange $hwnd $renderSampleRateComboId $baselineRenderRate
  $renderRateSet = Wait-ForComboSelection $hwnd $renderSampleRateComboId "48000" $maxPolls
  Invoke-ComboSelectionChange $hwnd $renderChannelsComboId $baselineRenderChannels
  $renderChannelsSet = Wait-ForComboSelection $hwnd $renderChannelsComboId "2" $maxPolls
  Invoke-ComboSelectionChange $hwnd $renderTypeComboId $baselineRenderType
  $renderTypeSet = Wait-ForComboSelection $hwnd $renderTypeComboId "Float32" $maxPolls
  $autoAlignCleared = Set-CheckboxState $autoAlignCheckbox 0 $maxPolls
  $baselineSet = $captureRateSet -and $captureChannelsSet -and $captureTypeSet -and
                 $renderRateSet -and $renderChannelsSet -and $renderTypeSet -and $autoAlignCleared

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $autoAlignCheckbox
  $effectiveRequestSeen = $false
  $summaryEffectiveRequestSeen = $false
  $activeRequestPreserved = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $summaryEffectiveRequestSeen =
      $summaryText.IndexOf("Effective render request: 44100 Hz / 1 ch / PCM16", [System.StringComparison]::Ordinal) -ge 0
    $effectiveRequestSeen =
      $diagnosticsText.IndexOf("Effective configured render request: 44100 Hz / 1 ch / PCM16", [System.StringComparison]::Ordinal) -ge 0
    $activeRequestPreserved =
      $diagnosticsText.IndexOf("Active session requested render: 48000 Hz / 2 ch / Float32", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    if ($summaryEffectiveRequestSeen -and $effectiveRequestSeen -and $activeRequestPreserved -and $runningPreserved) {
      break
    }
  }

  if ($originalCaptureRate -ge 0) {
    Invoke-ComboSelectionChange $hwnd $captureSampleRateComboId $originalCaptureRate
  }
  $captureRateRestored = $originalCaptureRate -lt 0 -or
                         (Wait-ForComboSelection $hwnd $captureSampleRateComboId (Get-ComboItemText $captureSampleRateCombo $originalCaptureRate) $maxPolls)
  if ($originalCaptureChannels -ge 0) {
    Invoke-ComboSelectionChange $hwnd $captureChannelsComboId $originalCaptureChannels
  }
  $captureChannelsRestored = $originalCaptureChannels -lt 0 -or
                             (Wait-ForComboSelection $hwnd $captureChannelsComboId (Get-ComboItemText $captureChannelsCombo $originalCaptureChannels) $maxPolls)
  if ($originalCaptureType -ge 0) {
    Invoke-ComboSelectionChange $hwnd $captureTypeComboId $originalCaptureType
  }
  $captureTypeRestored = $originalCaptureType -lt 0 -or
                         (Wait-ForComboSelection $hwnd $captureTypeComboId (Get-ComboItemText $captureTypeCombo $originalCaptureType) $maxPolls)
  if ($originalRenderRate -ge 0) {
    Invoke-ComboSelectionChange $hwnd $renderSampleRateComboId $originalRenderRate
  }
  $renderRateRestored = $originalRenderRate -lt 0 -or
                        (Wait-ForComboSelection $hwnd $renderSampleRateComboId (Get-ComboItemText $renderSampleRateCombo $originalRenderRate) $maxPolls)
  if ($originalRenderChannels -ge 0) {
    Invoke-ComboSelectionChange $hwnd $renderChannelsComboId $originalRenderChannels
  }
  $renderChannelsRestored = $originalRenderChannels -lt 0 -or
                            (Wait-ForComboSelection $hwnd $renderChannelsComboId (Get-ComboItemText $renderChannelsCombo $originalRenderChannels) $maxPolls)
  if ($originalRenderType -ge 0) {
    Invoke-ComboSelectionChange $hwnd $renderTypeComboId $originalRenderType
  }
  $renderTypeRestored = $originalRenderType -lt 0 -or
                        (Wait-ForComboSelection $hwnd $renderTypeComboId (Get-ComboItemText $renderTypeCombo $originalRenderType) $maxPolls)
  $autoAlignRestored = Set-CheckboxState $autoAlignCheckbox $originalAutoAlign $maxPolls
  $restored = $captureRateRestored -and $captureChannelsRestored -and $captureTypeRestored -and
              $renderRateRestored -and $renderChannelsRestored -and $renderTypeRestored -and $autoAlignRestored

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    BaselineSet = $baselineSet
    RunningBaselineSeen = $runningBaselineSeen
    SummaryEffectiveRequestSeen = $summaryEffectiveRequestSeen
    EffectiveRequestSeen = $effectiveRequestSeen
    ActiveRequestPreserved = $activeRequestPreserved
    RunningPreserved = $runningPreserved
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-FollowDefaultsRunningAutoAlignConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$followDefaultsCheckboxId, [int]$captureComboId, [int]$renderComboId, [int]$captureSampleRateComboId, [int]$captureChannelsComboId, [int]$captureTypeComboId, [int]$renderSampleRateComboId, [int]$renderChannelsComboId, [int]$renderTypeComboId, [int]$autoAlignCheckboxId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $captureSampleRateCombo = [Native]::GetDlgItem($hwnd, $captureSampleRateComboId)
  $captureChannelsCombo = [Native]::GetDlgItem($hwnd, $captureChannelsComboId)
  $captureTypeCombo = [Native]::GetDlgItem($hwnd, $captureTypeComboId)
  $renderSampleRateCombo = [Native]::GetDlgItem($hwnd, $renderSampleRateComboId)
  $renderChannelsCombo = [Native]::GetDlgItem($hwnd, $renderChannelsComboId)
  $renderTypeCombo = [Native]::GetDlgItem($hwnd, $renderTypeComboId)
  $autoAlignCheckbox = [Native]::GetDlgItem($hwnd, $autoAlignCheckboxId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904
  $cbGetCurSel = 0x0147

  $baselineFollowDefaults = Get-CheckState $followDefaultsCheckbox
  $originalCaptureRate = [int][Native]::SendMessage($captureSampleRateCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $originalCaptureChannels = [int][Native]::SendMessage($captureChannelsCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $originalCaptureType = [int][Native]::SendMessage($captureTypeCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $originalRenderRate = [int][Native]::SendMessage($renderSampleRateCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $originalRenderChannels = [int][Native]::SendMessage($renderChannelsCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $originalRenderType = [int][Native]::SendMessage($renderTypeCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $originalAutoAlign = Get-CheckState $autoAlignCheckbox

  $baselineCaptureRate = Find-ComboItemIndex $captureSampleRateCombo "44100"
  $baselineCaptureChannels = Find-ComboItemIndex $captureChannelsCombo "1"
  $baselineCaptureType = Find-ComboItemIndex $captureTypeCombo "PCM16"
  $baselineRenderRate = Find-ComboItemIndex $renderSampleRateCombo "48000"
  $baselineRenderChannels = Find-ComboItemIndex $renderChannelsCombo "2"
  $baselineRenderType = Find-ComboItemIndex $renderTypeCombo "Float32"
  if ($baselineCaptureRate -lt 0 -or
      $baselineCaptureChannels -lt 0 -or
      $baselineCaptureType -lt 0 -or
      $baselineRenderRate -lt 0 -or
      $baselineRenderChannels -lt 0 -or
      $baselineRenderType -lt 0) {
    return [pscustomobject]@{
      BaselineSet = $false
      RunningBaselineSeen = $false
      SummaryEffectiveRequestSeen = $false
      EffectiveRequestSeen = $false
      ActiveRequestPreserved = $false
      RunningPreserved = $false
      FollowDefaultsRestored = $false
      Restored = $false
      StoppedCleanly = $false
      SummaryText = Get-ControlTextById $hwnd $summaryControlId
      DiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    }
  }

  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }
  Invoke-ComboSelectionChange $hwnd $captureSampleRateComboId $baselineCaptureRate
  $captureRateSet = Wait-ForComboSelection $hwnd $captureSampleRateComboId "44100" $maxPolls
  Invoke-ComboSelectionChange $hwnd $captureChannelsComboId $baselineCaptureChannels
  $captureChannelsSet = Wait-ForComboSelection $hwnd $captureChannelsComboId "1" $maxPolls
  Invoke-ComboSelectionChange $hwnd $captureTypeComboId $baselineCaptureType
  $captureTypeSet = Wait-ForComboSelection $hwnd $captureTypeComboId "PCM16" $maxPolls
  Invoke-ComboSelectionChange $hwnd $renderSampleRateComboId $baselineRenderRate
  $renderRateSet = Wait-ForComboSelection $hwnd $renderSampleRateComboId "48000" $maxPolls
  Invoke-ComboSelectionChange $hwnd $renderChannelsComboId $baselineRenderChannels
  $renderChannelsSet = Wait-ForComboSelection $hwnd $renderChannelsComboId "2" $maxPolls
  Invoke-ComboSelectionChange $hwnd $renderTypeComboId $baselineRenderType
  $renderTypeSet = Wait-ForComboSelection $hwnd $renderTypeComboId "Float32" $maxPolls
  $autoAlignCleared = Set-CheckboxState $autoAlignCheckbox 0 $maxPolls
  $baselineSet = $captureRateSet -and $captureChannelsSet -and $captureTypeSet -and
                 $renderRateSet -and $renderChannelsSet -and $renderTypeSet -and $autoAlignCleared

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton) -and
        (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
        (-not [Native]::IsWindowEnabled($captureCombo)) -and
        (-not [Native]::IsWindowEnabled($renderCombo))) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $autoAlignCheckbox
  $effectiveRequestSeen = $false
  $summaryEffectiveRequestSeen = $false
  $activeRequestPreserved = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $summaryEffectiveRequestSeen =
      $summaryText.IndexOf("Follow defaults: On", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Effective render request: 44100 Hz / 1 ch / PCM16", [System.StringComparison]::Ordinal) -ge 0
    $effectiveRequestSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Effective configured render request: 44100 Hz / 1 ch / PCM16", [System.StringComparison]::Ordinal) -ge 0
    $activeRequestPreserved =
      $diagnosticsText.IndexOf("Active session requested render: 48000 Hz / 2 ch / Float32", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    if ($summaryEffectiveRequestSeen -and $effectiveRequestSeen -and $activeRequestPreserved -and $runningPreserved) {
      break
    }
  }

  if ($baselineFollowDefaults -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }
  $followDefaultsRestored = Wait-ForCheckboxState $followDefaultsCheckbox $baselineFollowDefaults $maxPolls
  if ($originalCaptureRate -ge 0) {
    Invoke-ComboSelectionChange $hwnd $captureSampleRateComboId $originalCaptureRate
  }
  $captureRateRestored = $originalCaptureRate -lt 0 -or
                         (Wait-ForComboSelection $hwnd $captureSampleRateComboId (Get-ComboItemText $captureSampleRateCombo $originalCaptureRate) $maxPolls)
  if ($originalCaptureChannels -ge 0) {
    Invoke-ComboSelectionChange $hwnd $captureChannelsComboId $originalCaptureChannels
  }
  $captureChannelsRestored = $originalCaptureChannels -lt 0 -or
                             (Wait-ForComboSelection $hwnd $captureChannelsComboId (Get-ComboItemText $captureChannelsCombo $originalCaptureChannels) $maxPolls)
  if ($originalCaptureType -ge 0) {
    Invoke-ComboSelectionChange $hwnd $captureTypeComboId $originalCaptureType
  }
  $captureTypeRestored = $originalCaptureType -lt 0 -or
                         (Wait-ForComboSelection $hwnd $captureTypeComboId (Get-ComboItemText $captureTypeCombo $originalCaptureType) $maxPolls)
  if ($originalRenderRate -ge 0) {
    Invoke-ComboSelectionChange $hwnd $renderSampleRateComboId $originalRenderRate
  }
  $renderRateRestored = $originalRenderRate -lt 0 -or
                        (Wait-ForComboSelection $hwnd $renderSampleRateComboId (Get-ComboItemText $renderSampleRateCombo $originalRenderRate) $maxPolls)
  if ($originalRenderChannels -ge 0) {
    Invoke-ComboSelectionChange $hwnd $renderChannelsComboId $originalRenderChannels
  }
  $renderChannelsRestored = $originalRenderChannels -lt 0 -or
                            (Wait-ForComboSelection $hwnd $renderChannelsComboId (Get-ComboItemText $renderChannelsCombo $originalRenderChannels) $maxPolls)
  if ($originalRenderType -ge 0) {
    Invoke-ComboSelectionChange $hwnd $renderTypeComboId $originalRenderType
  }
  $renderTypeRestored = $originalRenderType -lt 0 -or
                        (Wait-ForComboSelection $hwnd $renderTypeComboId (Get-ComboItemText $renderTypeCombo $originalRenderType) $maxPolls)
  $autoAlignRestored = Set-CheckboxState $autoAlignCheckbox $originalAutoAlign $maxPolls
  $restored = $followDefaultsRestored -and $captureRateRestored -and $captureChannelsRestored -and
              $captureTypeRestored -and $renderRateRestored -and $renderChannelsRestored -and $renderTypeRestored -and $autoAlignRestored

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    BaselineSet = $baselineSet
    RunningBaselineSeen = $runningBaselineSeen
    SummaryEffectiveRequestSeen = $summaryEffectiveRequestSeen
    EffectiveRequestSeen = $effectiveRequestSeen
    ActiveRequestPreserved = $activeRequestPreserved
    RunningPreserved = $runningPreserved
    FollowDefaultsRestored = $followDefaultsRestored
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-RunningDumpConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$dumpCheckboxId, [int]$dumpPathEditId, [int]$dumpTypeComboId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $dumpCheckbox = [Native]::GetDlgItem($hwnd, $dumpCheckboxId)
  $dumpPathEdit = [Native]::GetDlgItem($hwnd, $dumpPathEditId)
  $dumpTypeCombo = [Native]::GetDlgItem($hwnd, $dumpTypeComboId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $baselineDumpCheck = Get-CheckState $dumpCheckbox
  $baselineDumpPath = Get-Text $dumpPathEdit
  $baselineDumpTypeText = Get-ComboSelectionText $dumpTypeCombo
  $baselineDumpTypeIndex = Find-ComboItemIndex $dumpTypeCombo $baselineDumpTypeText
  $wavIndex = Find-ComboItemIndex $dumpTypeCombo "WAV"
  if ($wavIndex -lt 0) {
    return [pscustomobject]@{
      BaselineSet = $false
      RunningBaselineSeen = $false
      SummarySeen = $false
      DiagnosticsSeen = $false
      RunningPreserved = $false
      Restored = $false
      StoppedCleanly = $false
      SummaryText = Get-ControlTextById $hwnd $summaryControlId
      DiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    }
  }

  $baselineDumpEnabled = Set-CheckboxState $dumpCheckbox 1 $maxPolls
  if ($baselineDumpTypeIndex -ne $wavIndex) {
    Invoke-ComboSelectionChange $hwnd $dumpTypeComboId $wavIndex
  }
  $baselineDumpTypeSeen = Wait-ForComboSelection $hwnd $dumpTypeComboId "WAV" $maxPolls
  Invoke-EditTextChange $hwnd $dumpPathEditId ""
  $baselineSummary = Wait-ForControlTextContains $hwnd $summaryControlId @("Dump: On (WAV)", "Dump path: Auto temp file") $maxPolls
  $baselineSet = $baselineDumpEnabled -and $baselineDumpTypeSeen -and $baselineSummary.Seen

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  $activeDumpBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $runningBaselineSeen =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    $activeDumpBaselineSeen =
      $diagnosticsText.IndexOf("Active dump path:", [System.StringComparison]::Ordinal) -ge 0
    if ($runningBaselineSeen -and $activeDumpBaselineSeen) {
      $runningBaselineSeen = $true
      break
    }
  }

  $nextDumpPath = "C:\temp\next.wav"
  Invoke-EditTextChange $hwnd $dumpPathEditId $nextDumpPath

  $summarySeen = $false
  $diagnosticsSeen = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $summarySeen =
      $summaryText.IndexOf("Dump: On (WAV)", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Dump path: C:\temp\next.wav", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Active dump path:", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active dump path: C:\temp\next.wav", [System.StringComparison]::Ordinal) -lt 0 -and
      $diagnosticsText.IndexOf("Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    if ($summarySeen -and $diagnosticsSeen -and $runningPreserved) {
      break
    }
  }

  if ((Get-CheckState $dumpCheckbox) -ne $baselineDumpCheck) {
    Invoke-CheckboxClick $dumpCheckbox
  }
  $dumpRestored = Wait-ForCheckboxState $dumpCheckbox $baselineDumpCheck $maxPolls
  if ($baselineDumpTypeIndex -ge 0) {
    Invoke-ComboSelectionChange $hwnd $dumpTypeComboId $baselineDumpTypeIndex
  }
  $dumpTypeRestored = $true
  if ($baselineDumpTypeIndex -ge 0) {
    $dumpTypeRestored = Wait-ForComboSelection $hwnd $dumpTypeComboId $baselineDumpTypeText $maxPolls
  }
  Invoke-EditTextChange $hwnd $dumpPathEditId $baselineDumpPath
  $dumpPathRestored = Wait-ForControlTextEquals $hwnd $dumpPathEditId $baselineDumpPath $maxPolls
  $restored = $dumpRestored -and $dumpTypeRestored -and $dumpPathRestored.Seen

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    BaselineSet = $baselineSet
    RunningBaselineSeen = $runningBaselineSeen
    SummarySeen = $summarySeen
    DiagnosticsSeen = $diagnosticsSeen
    RunningPreserved = $runningPreserved
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-RunningTimingConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$delayEditId, [int]$captureBufferEditId, [int]$renderBufferEditId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $delayEdit = [Native]::GetDlgItem($hwnd, $delayEditId)
  $captureBufferEdit = [Native]::GetDlgItem($hwnd, $captureBufferEditId)
  $renderBufferEdit = [Native]::GetDlgItem($hwnd, $renderBufferEditId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $baselineDelay = Get-Text $delayEdit
  $baselineCaptureBuffer = Get-Text $captureBufferEdit
  $baselineRenderBuffer = Get-Text $renderBufferEdit

  Invoke-EditTextChange $hwnd $delayEditId "120"
  $baselineDelaySeen = (Wait-ForControlTextEquals $hwnd $delayEditId "120" $maxPolls).Seen
  Invoke-EditTextChange $hwnd $captureBufferEditId "40"
  $baselineCaptureBufferSeen = (Wait-ForControlTextEquals $hwnd $captureBufferEditId "40" $maxPolls).Seen
  Invoke-EditTextChange $hwnd $renderBufferEditId "40"
  $baselineRenderBufferSeen = (Wait-ForControlTextEquals $hwnd $renderBufferEditId "40" $maxPolls).Seen
  $baselineSummary = Wait-ForControlTextContains $hwnd $summaryControlId @("Monitor delay: 120 ms", "Capture buffer: 40 ms", "Render buffer: 40 ms") $maxPolls
  $baselineSet = $baselineDelaySeen -and $baselineCaptureBufferSeen -and $baselineRenderBufferSeen -and $baselineSummary.Seen

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  $activeTimingBaselineSeen = $false
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $runningBaselineSeen =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    $activeTimingBaselineSeen =
      $diagnosticsText.IndexOf("Active monitor delay: 120 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active capture buffer: 40 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active render buffer: 40 ms", [System.StringComparison]::Ordinal) -ge 0
    if ($runningBaselineSeen -and $activeTimingBaselineSeen) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-EditTextChange $hwnd $delayEditId "250"
  Invoke-EditTextChange $hwnd $captureBufferEditId "60"
  Invoke-EditTextChange $hwnd $renderBufferEditId "80"

  $summarySeen = $false
  $diagnosticsSeen = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $summarySeen =
      $summaryText.IndexOf("Monitor delay: 250 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Capture buffer: 60 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Render buffer: 80 ms", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Active monitor delay: 120 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active capture buffer: 40 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active render buffer: 40 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active monitor delay: 250 ms", [System.StringComparison]::Ordinal) -lt 0 -and
      $diagnosticsText.IndexOf("Active capture buffer: 60 ms", [System.StringComparison]::Ordinal) -lt 0 -and
      $diagnosticsText.IndexOf("Active render buffer: 80 ms", [System.StringComparison]::Ordinal) -lt 0 -and
      $diagnosticsText.IndexOf("Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    if ($summarySeen -and $diagnosticsSeen -and $runningPreserved) {
      break
    }
  }

  Invoke-EditTextChange $hwnd $delayEditId $baselineDelay
  $delayRestored = (Wait-ForControlTextEquals $hwnd $delayEditId $baselineDelay $maxPolls).Seen
  Invoke-EditTextChange $hwnd $captureBufferEditId $baselineCaptureBuffer
  $captureBufferRestored = (Wait-ForControlTextEquals $hwnd $captureBufferEditId $baselineCaptureBuffer $maxPolls).Seen
  Invoke-EditTextChange $hwnd $renderBufferEditId $baselineRenderBuffer
  $renderBufferRestored = (Wait-ForControlTextEquals $hwnd $renderBufferEditId $baselineRenderBuffer $maxPolls).Seen
  $restored = $delayRestored -and $captureBufferRestored -and $renderBufferRestored

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    BaselineSet = $baselineSet
    RunningBaselineSeen = $runningBaselineSeen
    SummarySeen = $summarySeen
    DiagnosticsSeen = $diagnosticsSeen
    RunningPreserved = $runningPreserved
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-FollowDefaultsRunningDumpConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$followDefaultsCheckboxId, [int]$captureComboId, [int]$renderComboId, [int]$dumpCheckboxId, [int]$dumpPathEditId, [int]$dumpTypeComboId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $dumpCheckbox = [Native]::GetDlgItem($hwnd, $dumpCheckboxId)
  $dumpPathEdit = [Native]::GetDlgItem($hwnd, $dumpPathEditId)
  $dumpTypeCombo = [Native]::GetDlgItem($hwnd, $dumpTypeComboId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $baselineFollowDefaults = Get-CheckState $followDefaultsCheckbox
  $baselineDumpCheck = Get-CheckState $dumpCheckbox
  $baselineDumpPath = Get-Text $dumpPathEdit
  $baselineDumpTypeText = Get-ComboSelectionText $dumpTypeCombo
  $baselineDumpTypeIndex = Find-ComboItemIndex $dumpTypeCombo $baselineDumpTypeText
  $wavIndex = Find-ComboItemIndex $dumpTypeCombo "WAV"
  if ($wavIndex -lt 0) {
    return [pscustomobject]@{
      BaselineSet = $false
      RunningBaselineSeen = $false
      SummarySeen = $false
      DiagnosticsSeen = $false
      RunningPreserved = $false
      FollowDefaultsRestored = $false
      Restored = $false
      StoppedCleanly = $false
      SummaryText = Get-ControlTextById $hwnd $summaryControlId
      DiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    }
  }

  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }
  $baselineDumpEnabled = Set-CheckboxState $dumpCheckbox 1 $maxPolls
  if ($baselineDumpTypeIndex -ne $wavIndex) {
    Invoke-ComboSelectionChange $hwnd $dumpTypeComboId $wavIndex
  }
  $baselineDumpTypeSeen = Wait-ForComboSelection $hwnd $dumpTypeComboId "WAV" $maxPolls
  Invoke-EditTextChange $hwnd $dumpPathEditId ""
  $baselineSummary = Wait-ForControlTextContains $hwnd $summaryControlId @("Follow defaults: On", "Dump: On (WAV)", "Dump path: Auto temp file") $maxPolls
  $baselineSet = $baselineDumpEnabled -and $baselineDumpTypeSeen -and $baselineSummary.Seen

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  $activeDumpBaselineSeen = $false
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $runningBaselineSeen =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    $activeDumpBaselineSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active dump path:", [System.StringComparison]::Ordinal) -ge 0
    if ($runningBaselineSeen -and $activeDumpBaselineSeen) {
      $runningBaselineSeen = $true
      break
    }
  }

  $nextDumpPath = "C:\temp\next.wav"
  Invoke-EditTextChange $hwnd $dumpPathEditId $nextDumpPath

  $summarySeen = $false
  $diagnosticsSeen = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $summarySeen =
      $summaryText.IndexOf("Follow defaults: On", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Dump: On (WAV)", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Dump path: C:\temp\next.wav", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active dump path:", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active dump path: C:\temp\next.wav", [System.StringComparison]::Ordinal) -lt 0 -and
      $diagnosticsText.IndexOf("Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    if ($summarySeen -and $diagnosticsSeen -and $runningPreserved) {
      break
    }
  }

  if ($baselineFollowDefaults -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }
  $followDefaultsRestored = Wait-ForCheckboxState $followDefaultsCheckbox $baselineFollowDefaults $maxPolls
  if ((Get-CheckState $dumpCheckbox) -ne $baselineDumpCheck) {
    Invoke-CheckboxClick $dumpCheckbox
  }
  $dumpRestored = Wait-ForCheckboxState $dumpCheckbox $baselineDumpCheck $maxPolls
  if ($baselineDumpTypeIndex -ge 0) {
    Invoke-ComboSelectionChange $hwnd $dumpTypeComboId $baselineDumpTypeIndex
  }
  $dumpTypeRestored = $baselineDumpTypeIndex -lt 0 -or
                      (Wait-ForComboSelection $hwnd $dumpTypeComboId $baselineDumpTypeText $maxPolls)
  Invoke-EditTextChange $hwnd $dumpPathEditId $baselineDumpPath
  $dumpPathRestored = (Wait-ForControlTextEquals $hwnd $dumpPathEditId $baselineDumpPath $maxPolls).Seen
  $restored = $followDefaultsRestored -and $dumpRestored -and $dumpTypeRestored -and $dumpPathRestored

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    BaselineSet = $baselineSet
    RunningBaselineSeen = $runningBaselineSeen
    SummarySeen = $summarySeen
    DiagnosticsSeen = $diagnosticsSeen
    RunningPreserved = $runningPreserved
    FollowDefaultsRestored = $followDefaultsRestored
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-FollowDefaultsRunningTimingConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$followDefaultsCheckboxId, [int]$captureComboId, [int]$renderComboId, [int]$delayEditId, [int]$captureBufferEditId, [int]$renderBufferEditId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $delayEdit = [Native]::GetDlgItem($hwnd, $delayEditId)
  $captureBufferEdit = [Native]::GetDlgItem($hwnd, $captureBufferEditId)
  $renderBufferEdit = [Native]::GetDlgItem($hwnd, $renderBufferEditId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $baselineFollowDefaults = Get-CheckState $followDefaultsCheckbox
  $baselineDelay = Get-Text $delayEdit
  $baselineCaptureBuffer = Get-Text $captureBufferEdit
  $baselineRenderBuffer = Get-Text $renderBufferEdit

  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }
  Invoke-EditTextChange $hwnd $delayEditId "120"
  $baselineDelaySeen = (Wait-ForControlTextEquals $hwnd $delayEditId "120" $maxPolls).Seen
  Invoke-EditTextChange $hwnd $captureBufferEditId "40"
  $baselineCaptureBufferSeen = (Wait-ForControlTextEquals $hwnd $captureBufferEditId "40" $maxPolls).Seen
  Invoke-EditTextChange $hwnd $renderBufferEditId "40"
  $baselineRenderBufferSeen = (Wait-ForControlTextEquals $hwnd $renderBufferEditId "40" $maxPolls).Seen
  $baselineSummary = Wait-ForControlTextContains $hwnd $summaryControlId @("Follow defaults: On", "Monitor delay: 120 ms", "Capture buffer: 40 ms", "Render buffer: 40 ms") $maxPolls
  $baselineSet = $baselineDelaySeen -and $baselineCaptureBufferSeen -and $baselineRenderBufferSeen -and $baselineSummary.Seen

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  $activeTimingBaselineSeen = $false
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $runningBaselineSeen =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    $activeTimingBaselineSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active monitor delay: 120 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active capture buffer: 40 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active render buffer: 40 ms", [System.StringComparison]::Ordinal) -ge 0
    if ($runningBaselineSeen -and $activeTimingBaselineSeen) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-EditTextChange $hwnd $delayEditId "250"
  Invoke-EditTextChange $hwnd $captureBufferEditId "60"
  Invoke-EditTextChange $hwnd $renderBufferEditId "80"

  $summarySeen = $false
  $diagnosticsSeen = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $summarySeen =
      $summaryText.IndexOf("Follow defaults: On", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Monitor delay: 250 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Capture buffer: 60 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Render buffer: 80 ms", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active monitor delay: 120 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active capture buffer: 40 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active render buffer: 40 ms", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active monitor delay: 250 ms", [System.StringComparison]::Ordinal) -lt 0 -and
      $diagnosticsText.IndexOf("Active capture buffer: 60 ms", [System.StringComparison]::Ordinal) -lt 0 -and
      $diagnosticsText.IndexOf("Active render buffer: 80 ms", [System.StringComparison]::Ordinal) -lt 0 -and
      $diagnosticsText.IndexOf("Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    if ($summarySeen -and $diagnosticsSeen -and $runningPreserved) {
      break
    }
  }

  if ($baselineFollowDefaults -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }
  $followDefaultsRestored = Wait-ForCheckboxState $followDefaultsCheckbox $baselineFollowDefaults $maxPolls
  Invoke-EditTextChange $hwnd $delayEditId $baselineDelay
  $delayRestored = (Wait-ForControlTextEquals $hwnd $delayEditId $baselineDelay $maxPolls).Seen
  Invoke-EditTextChange $hwnd $captureBufferEditId $baselineCaptureBuffer
  $captureBufferRestored = (Wait-ForControlTextEquals $hwnd $captureBufferEditId $baselineCaptureBuffer $maxPolls).Seen
  Invoke-EditTextChange $hwnd $renderBufferEditId $baselineRenderBuffer
  $renderBufferRestored = (Wait-ForControlTextEquals $hwnd $renderBufferEditId $baselineRenderBuffer $maxPolls).Seen
  $restored = $followDefaultsRestored -and $delayRestored -and $captureBufferRestored -and $renderBufferRestored

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    BaselineSet = $baselineSet
    RunningBaselineSeen = $runningBaselineSeen
    SummarySeen = $summarySeen
    DiagnosticsSeen = $diagnosticsSeen
    RunningPreserved = $runningPreserved
    FollowDefaultsRestored = $followDefaultsRestored
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-RunningWasapiModeConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$captureShareComboId, [int]$captureDriveComboId, [int]$renderShareComboId, [int]$renderDriveComboId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $captureShareCombo = [Native]::GetDlgItem($hwnd, $captureShareComboId)
  $captureDriveCombo = [Native]::GetDlgItem($hwnd, $captureDriveComboId)
  $renderShareCombo = [Native]::GetDlgItem($hwnd, $renderShareComboId)
  $renderDriveCombo = [Native]::GetDlgItem($hwnd, $renderDriveComboId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $cbGetCurSel = 0x0147
  $baselineCaptureShareIndex = [int][Native]::SendMessage($captureShareCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineCaptureDriveIndex = [int][Native]::SendMessage($captureDriveCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineRenderShareIndex = [int][Native]::SendMessage($renderShareCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineRenderDriveIndex = [int][Native]::SendMessage($renderDriveCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)

  $baselineCaptureShareSeen = Wait-ForComboSelection $hwnd $captureShareComboId "Shared" $maxPolls
  if (-not $baselineCaptureShareSeen) {
    Invoke-ComboSelectionChange $hwnd $captureShareComboId 0
    $baselineCaptureShareSeen = Wait-ForComboSelection $hwnd $captureShareComboId "Shared" $maxPolls
  }
  $baselineCaptureDriveSeen = Wait-ForComboSelection $hwnd $captureDriveComboId "Event" $maxPolls
  if (-not $baselineCaptureDriveSeen) {
    Invoke-ComboSelectionChange $hwnd $captureDriveComboId 0
    $baselineCaptureDriveSeen = Wait-ForComboSelection $hwnd $captureDriveComboId "Event" $maxPolls
  }
  $baselineRenderShareSeen = Wait-ForComboSelection $hwnd $renderShareComboId "Shared" $maxPolls
  if (-not $baselineRenderShareSeen) {
    Invoke-ComboSelectionChange $hwnd $renderShareComboId 0
    $baselineRenderShareSeen = Wait-ForComboSelection $hwnd $renderShareComboId "Shared" $maxPolls
  }
  $baselineRenderDriveSeen = Wait-ForComboSelection $hwnd $renderDriveComboId "Event" $maxPolls
  if (-not $baselineRenderDriveSeen) {
    Invoke-ComboSelectionChange $hwnd $renderDriveComboId 0
    $baselineRenderDriveSeen = Wait-ForComboSelection $hwnd $renderDriveComboId "Event" $maxPolls
  }
  $baselineSet = $baselineCaptureShareSeen -and $baselineCaptureDriveSeen -and $baselineRenderShareSeen -and $baselineRenderDriveSeen

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  $activeModeBaselineSeen = $false
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $runningBaselineSeen =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    $activeModeBaselineSeen =
      $diagnosticsText.IndexOf("Active capture WASAPI request: Shared / Event", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active render WASAPI request: Shared / Event", [System.StringComparison]::Ordinal) -ge 0
    if ($runningBaselineSeen -and $activeModeBaselineSeen) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureShareComboId 1
  Invoke-ComboSelectionChange $hwnd $captureDriveComboId 1
  Invoke-ComboSelectionChange $hwnd $renderShareComboId 1
  Invoke-ComboSelectionChange $hwnd $renderDriveComboId 1

  $summarySeen = $false
  $diagnosticsSeen = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $summarySeen =
      $summaryText.IndexOf("Capture WASAPI: Exclusive / Timer", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Render WASAPI: Exclusive / Timer", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Active capture WASAPI request: Shared / Event", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active render WASAPI request: Shared / Event", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active capture WASAPI request: Exclusive / Timer", [System.StringComparison]::Ordinal) -lt 0 -and
      $diagnosticsText.IndexOf("Active render WASAPI request: Exclusive / Timer", [System.StringComparison]::Ordinal) -lt 0 -and
      $diagnosticsText.IndexOf("Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    if ($summarySeen -and $diagnosticsSeen -and $runningPreserved) {
      break
    }
  }

  if ($baselineCaptureShareIndex -ge 0) {
    Invoke-ComboSelectionChange $hwnd $captureShareComboId $baselineCaptureShareIndex
  }
  $captureShareRestored = $baselineCaptureShareIndex -lt 0 -or
                          (Wait-ForComboSelection $hwnd $captureShareComboId (Get-ComboItemText $captureShareCombo $baselineCaptureShareIndex) $maxPolls)
  if ($baselineCaptureDriveIndex -ge 0) {
    Invoke-ComboSelectionChange $hwnd $captureDriveComboId $baselineCaptureDriveIndex
  }
  $captureDriveRestored = $baselineCaptureDriveIndex -lt 0 -or
                          (Wait-ForComboSelection $hwnd $captureDriveComboId (Get-ComboItemText $captureDriveCombo $baselineCaptureDriveIndex) $maxPolls)
  if ($baselineRenderShareIndex -ge 0) {
    Invoke-ComboSelectionChange $hwnd $renderShareComboId $baselineRenderShareIndex
  }
  $renderShareRestored = $baselineRenderShareIndex -lt 0 -or
                         (Wait-ForComboSelection $hwnd $renderShareComboId (Get-ComboItemText $renderShareCombo $baselineRenderShareIndex) $maxPolls)
  if ($baselineRenderDriveIndex -ge 0) {
    Invoke-ComboSelectionChange $hwnd $renderDriveComboId $baselineRenderDriveIndex
  }
  $renderDriveRestored = $baselineRenderDriveIndex -lt 0 -or
                         (Wait-ForComboSelection $hwnd $renderDriveComboId (Get-ComboItemText $renderDriveCombo $baselineRenderDriveIndex) $maxPolls)
  $restored = $captureShareRestored -and $captureDriveRestored -and $renderShareRestored -and $renderDriveRestored

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    BaselineSet = $baselineSet
    RunningBaselineSeen = $runningBaselineSeen
    SummarySeen = $summarySeen
    DiagnosticsSeen = $diagnosticsSeen
    RunningPreserved = $runningPreserved
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-FollowDefaultsRunningWasapiModeConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$followDefaultsCheckboxId, [int]$captureComboId, [int]$renderComboId, [int]$captureShareComboId, [int]$captureDriveComboId, [int]$renderShareComboId, [int]$renderDriveComboId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $captureCombo = [Native]::GetDlgItem($hwnd, $captureComboId)
  $renderCombo = [Native]::GetDlgItem($hwnd, $renderComboId)
  $captureShareCombo = [Native]::GetDlgItem($hwnd, $captureShareComboId)
  $captureDriveCombo = [Native]::GetDlgItem($hwnd, $captureDriveComboId)
  $renderShareCombo = [Native]::GetDlgItem($hwnd, $renderShareComboId)
  $renderDriveCombo = [Native]::GetDlgItem($hwnd, $renderDriveComboId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $cbGetCurSel = 0x0147
  $baselineFollowDefaults = Get-CheckState $followDefaultsCheckbox
  $baselineCaptureShareIndex = [int][Native]::SendMessage($captureShareCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineCaptureDriveIndex = [int][Native]::SendMessage($captureDriveCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineRenderShareIndex = [int][Native]::SendMessage($renderShareCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineRenderDriveIndex = [int][Native]::SendMessage($renderDriveCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)

  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }
  $baselineCaptureShareSeen = Wait-ForComboSelection $hwnd $captureShareComboId "Shared" $maxPolls
  if (-not $baselineCaptureShareSeen) {
    Invoke-ComboSelectionChange $hwnd $captureShareComboId 0
    $baselineCaptureShareSeen = Wait-ForComboSelection $hwnd $captureShareComboId "Shared" $maxPolls
  }
  $baselineCaptureDriveSeen = Wait-ForComboSelection $hwnd $captureDriveComboId "Event" $maxPolls
  if (-not $baselineCaptureDriveSeen) {
    Invoke-ComboSelectionChange $hwnd $captureDriveComboId 0
    $baselineCaptureDriveSeen = Wait-ForComboSelection $hwnd $captureDriveComboId "Event" $maxPolls
  }
  $baselineRenderShareSeen = Wait-ForComboSelection $hwnd $renderShareComboId "Shared" $maxPolls
  if (-not $baselineRenderShareSeen) {
    Invoke-ComboSelectionChange $hwnd $renderShareComboId 0
    $baselineRenderShareSeen = Wait-ForComboSelection $hwnd $renderShareComboId "Shared" $maxPolls
  }
  $baselineRenderDriveSeen = Wait-ForComboSelection $hwnd $renderDriveComboId "Event" $maxPolls
  if (-not $baselineRenderDriveSeen) {
    Invoke-ComboSelectionChange $hwnd $renderDriveComboId 0
    $baselineRenderDriveSeen = Wait-ForComboSelection $hwnd $renderDriveComboId "Event" $maxPolls
  }
  $baselineSet = $baselineCaptureShareSeen -and $baselineCaptureDriveSeen -and $baselineRenderShareSeen -and $baselineRenderDriveSeen

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  $activeModeBaselineSeen = $false
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $runningBaselineSeen =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    $activeModeBaselineSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active capture WASAPI request: Shared / Event", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active render WASAPI request: Shared / Event", [System.StringComparison]::Ordinal) -ge 0
    if ($runningBaselineSeen -and $activeModeBaselineSeen) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-ComboSelectionChange $hwnd $captureShareComboId 1
  Invoke-ComboSelectionChange $hwnd $captureDriveComboId 1
  Invoke-ComboSelectionChange $hwnd $renderShareComboId 1
  Invoke-ComboSelectionChange $hwnd $renderDriveComboId 1

  $summarySeen = $false
  $diagnosticsSeen = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $summarySeen =
      $summaryText.IndexOf("Follow defaults: On", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Capture WASAPI: Exclusive / Timer", [System.StringComparison]::Ordinal) -ge 0 -and
      $summaryText.IndexOf("Render WASAPI: Exclusive / Timer", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active capture WASAPI request: Shared / Event", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active render WASAPI request: Shared / Event", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active capture WASAPI request: Exclusive / Timer", [System.StringComparison]::Ordinal) -lt 0 -and
      $diagnosticsText.IndexOf("Active render WASAPI request: Exclusive / Timer", [System.StringComparison]::Ordinal) -lt 0 -and
      $diagnosticsText.IndexOf("Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.", [System.StringComparison]::Ordinal) -ge 0
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton) -and
      (Get-CheckState $followDefaultsCheckbox) -eq 1 -and
      (-not [Native]::IsWindowEnabled($captureCombo)) -and
      (-not [Native]::IsWindowEnabled($renderCombo))
    if ($summarySeen -and $diagnosticsSeen -and $runningPreserved) {
      break
    }
  }

  if ($baselineCaptureShareIndex -ge 0) {
    Invoke-ComboSelectionChange $hwnd $captureShareComboId $baselineCaptureShareIndex
  }
  $captureShareRestored = $baselineCaptureShareIndex -lt 0 -or
                          (Wait-ForComboSelection $hwnd $captureShareComboId (Get-ComboItemText $captureShareCombo $baselineCaptureShareIndex) $maxPolls)
  if ($baselineCaptureDriveIndex -ge 0) {
    Invoke-ComboSelectionChange $hwnd $captureDriveComboId $baselineCaptureDriveIndex
  }
  $captureDriveRestored = $baselineCaptureDriveIndex -lt 0 -or
                          (Wait-ForComboSelection $hwnd $captureDriveComboId (Get-ComboItemText $captureDriveCombo $baselineCaptureDriveIndex) $maxPolls)
  if ($baselineRenderShareIndex -ge 0) {
    Invoke-ComboSelectionChange $hwnd $renderShareComboId $baselineRenderShareIndex
  }
  $renderShareRestored = $baselineRenderShareIndex -lt 0 -or
                         (Wait-ForComboSelection $hwnd $renderShareComboId (Get-ComboItemText $renderShareCombo $baselineRenderShareIndex) $maxPolls)
  if ($baselineRenderDriveIndex -ge 0) {
    Invoke-ComboSelectionChange $hwnd $renderDriveComboId $baselineRenderDriveIndex
  }
  $renderDriveRestored = $baselineRenderDriveIndex -lt 0 -or
                         (Wait-ForComboSelection $hwnd $renderDriveComboId (Get-ComboItemText $renderDriveCombo $baselineRenderDriveIndex) $maxPolls)
  if ($baselineFollowDefaults -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }
  $followDefaultsRestored = Wait-ForCheckboxState $followDefaultsCheckbox $baselineFollowDefaults $maxPolls
  $restored = $captureShareRestored -and $captureDriveRestored -and $renderShareRestored -and $renderDriveRestored -and $followDefaultsRestored

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    BaselineSet = $baselineSet
    RunningBaselineSeen = $runningBaselineSeen
    SummarySeen = $summarySeen
    DiagnosticsSeen = $diagnosticsSeen
    RunningPreserved = $runningPreserved
    FollowDefaultsRestored = $followDefaultsRestored
    Restored = $restored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-FollowDefaultsRefreshResyncCheck([IntPtr]$hwnd, [int]$refreshButtonId, [int]$startButtonId, [int]$stopButtonId, [int]$followDefaultsCheckboxId, [int]$captureDeviceComboId, [int]$renderDeviceComboId, [int]$maxPolls = 120) {
  $refreshButton = [Native]::GetDlgItem($hwnd, $refreshButtonId)
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $captureDeviceCombo = [Native]::GetDlgItem($hwnd, $captureDeviceComboId)
  $renderDeviceCombo = [Native]::GetDlgItem($hwnd, $renderDeviceComboId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $baselineFollowDefaultsCheck = Get-CheckState $followDefaultsCheckbox
  $started = $false
  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }

  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $title = Get-Text $hwnd
    if ($title -like "*| Running |*" -and
        (-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningBaselineSeen = $true
      break
    }
  }

  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$refreshButtonId, [IntPtr]::Zero)
  $trackingPreserved = $false
  $refreshRebuildSeen = $false
  $summarySeen = $false
  $captureStillDisabled = $false
  $renderStillDisabled = $false
  $runningRestored = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $trackingPreserved =
      $diagnosticsText.IndexOf("Device tracking: current capture/render selection follows system defaults", [System.StringComparison]::Ordinal) -ge 0
    $refreshRebuildSeen =
      $diagnosticsText.IndexOf("Last device change: refresh-devices => rebuild-success", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Last rebuild: refresh-devices => success", [System.StringComparison]::Ordinal) -ge 0
    $summarySeen =
      $summaryText.IndexOf("Running session note: the active session was rebuilt successfully after the device change.", [System.StringComparison]::Ordinal) -ge 0
    $captureStillDisabled = -not [Native]::IsWindowEnabled($captureDeviceCombo)
    $renderStillDisabled = -not [Native]::IsWindowEnabled($renderDeviceCombo)
    $runningRestored =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    if ($trackingPreserved -and $refreshRebuildSeen -and $summarySeen -and $captureStillDisabled -and $renderStillDisabled -and $runningRestored) {
      break
    }
  }

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  if ($baselineFollowDefaultsCheck -ne (Get-CheckState $followDefaultsCheckbox)) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }

  $followDefaultsRestored = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((Get-CheckState $followDefaultsCheckbox) -eq $baselineFollowDefaultsCheck) {
      $followDefaultsRestored = $true
      break
    }
  }

  [pscustomobject]@{
    RunningBaselineSeen = $runningBaselineSeen
    TrackingPreserved = $trackingPreserved
    RefreshRebuildSeen = $refreshRebuildSeen
    SummarySeen = $summarySeen
    RunningRestored = $runningRestored
    CaptureStillDisabled = $captureStillDisabled
    RenderStillDisabled = $renderStillDisabled
    StoppedCleanly = $stoppedCleanly
    FollowDefaultsRestored = $followDefaultsRestored
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-QuickProbeResultSemanticsCheck([IntPtr]$hwnd, [int]$probeButtonId, [int]$monitorCheckboxId, [int]$maxPolls = 180) {
  $probeButton = [Native]::GetDlgItem($hwnd, $probeButtonId)
  $monitorCheckbox = [Native]::GetDlgItem($hwnd, $monitorCheckboxId)
  $baselineMonitorCheck = Get-CheckState $monitorCheckbox

  if ($baselineMonitorCheck -ne 1) {
    Invoke-CheckboxClick $monitorCheckbox
    Start-Sleep -Milliseconds 200
  }

  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$probeButtonId, [IntPtr]::Zero)
  $monitorOnResult = Wait-ForProbeTextContains $hwnd @(
    "QuickSummary: success",
    "monitor=on",
    "NegotiatedRender:",
    "RenderFormatMatch:"
  ) $maxPolls

  Invoke-CheckboxClick $monitorCheckbox
  Start-Sleep -Milliseconds 200
  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$probeButtonId, [IntPtr]::Zero)
  $monitorOffResult = Wait-ForProbeTextContains $hwnd @(
    "QuickSummary: success",
    "monitor=off",
    "mode=render-disabled",
    "NegotiatedRender: disabled",
    "RenderFormatMatch: disabled",
    "RenderWave: disabled",
    "RenderRuntime: disabled",
    "RenderWaveNote: render monitoring is disabled because monitor playback is off"
  ) $maxPolls

  $restored = $false
  if ($baselineMonitorCheck -ne (Get-CheckState $monitorCheckbox)) {
    Invoke-CheckboxClick $monitorCheckbox
  }
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 100
    if (Get-CheckState $monitorCheckbox -eq $baselineMonitorCheck) {
      $restored = $true
      break
    }
  }

  [pscustomobject]@{
    MonitorOnSeen = $monitorOnResult.Seen
    MonitorOnProbeText = $monitorOnResult.ProbeText
    MonitorOffSeen = $monitorOffResult.Seen
    MonitorOffProbeText = $monitorOffResult.ProbeText
    Restored = $restored
  }
}
function Invoke-QuickProbeSourceModeFailureCheck([IntPtr]$hwnd, [int]$captureBackendComboId, [int]$sourceComboId, [int]$probeButtonId, [int]$maxPolls = 120) {
  $cbGetCurSel = 0x0147
  $captureBackendCombo = [Native]::GetDlgItem($hwnd, $captureBackendComboId)
  $sourceCombo = [Native]::GetDlgItem($hwnd, $sourceComboId)

  $baselineCaptureBackendIndex = [int][Native]::SendMessage($captureBackendCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineSourceIndex = [int][Native]::SendMessage($sourceCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)

  Invoke-ComboSelectionChange $hwnd $captureBackendComboId 1
  if (-not (Wait-ForComboSelection $hwnd $captureBackendComboId "WAVE API" $maxPolls)) {
    return [pscustomobject]@{
      FailureSeen = $false
      Restored = $false
    }
  }

  Invoke-ComboSelectionChange $hwnd $sourceComboId 1
  if (-not (Wait-ForComboSelection $hwnd $sourceComboId "System Loopback" $maxPolls)) {
    return [pscustomobject]@{
      FailureSeen = $false
      Restored = $false
    }
  }

  [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$probeButtonId, [IntPtr]::Zero)
  $failureResult = Wait-ForProbeTextContains $hwnd @(
    "QuickSummary: failed | dump=none | cap-fmt=not-negotiated | ren-fmt=not-negotiated | mode=not-started",
    "CaptureWave: not-started",
    "RenderWave: not-started",
    "FailureStage: source-mode",
    "Use --capture-backend=wasapi for loopback, or switch --source=mic."
  ) $maxPolls

  Invoke-ComboSelectionChange $hwnd $sourceComboId $baselineSourceIndex
  $sourceRestored = Wait-ForComboSelection $hwnd $sourceComboId (Get-ComboItemText $sourceCombo $baselineSourceIndex) $maxPolls
  Invoke-ComboSelectionChange $hwnd $captureBackendComboId $baselineCaptureBackendIndex
  $captureBackendRestored = Wait-ForComboSelection $hwnd $captureBackendComboId (Get-ComboItemText $captureBackendCombo $baselineCaptureBackendIndex) $maxPolls

  [pscustomobject]@{
    FailureSeen = $failureResult.Seen
    FailureProbeText = $failureResult.ProbeText
    Restored = $sourceRestored -and $captureBackendRestored
  }
}
function Invoke-FollowDefaultsLoopbackSemanticsCheck([IntPtr]$hwnd, [int]$sourceComboId, [int]$followDefaultsCheckboxId, [int]$maxPolls = 120) {
  $cbGetCurSel = 0x0147
  $sourceCombo = [Native]::GetDlgItem($hwnd, $sourceComboId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $baselineSourceIndex = [int][Native]::SendMessage($sourceCombo, $cbGetCurSel, [IntPtr]::Zero, [IntPtr]::Zero)
  $baselineFollowDefaultsCheck = Get-CheckState $followDefaultsCheckbox
  $baselineSummaryText = Get-ControlTextById $hwnd $summaryControlId
  $baselineDiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId

  if ($baselineSourceIndex -ne 1) {
    Invoke-ComboSelectionChange $hwnd $sourceComboId 1
  }
  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }

  $summaryOn = Wait-ForControlTextContains $hwnd $summaryControlId @(
    "Device selection follows current system defaults; manual device picks are inactive and loopback capture follows the current default render endpoint"
  ) $maxPolls
  $diagnosticsOn = Wait-ForControlTextContains $hwnd $diagnosticsControlId @(
    "Device tracking: current capture/render selection follows system defaults, and loopback capture follows the current default render endpoint"
  ) $maxPolls

  if ($baselineFollowDefaultsCheck -ne (Get-CheckState $followDefaultsCheckbox)) {
    Invoke-CheckboxClick $followDefaultsCheckbox
  }
  if ($baselineSourceIndex -ne 1) {
    Invoke-ComboSelectionChange $hwnd $sourceComboId $baselineSourceIndex
  }

  $summaryRestored = Wait-ForControlTextEquals $hwnd $summaryControlId $baselineSummaryText $maxPolls
  $diagnosticsRestored = Wait-ForControlTextEquals $hwnd $diagnosticsControlId $baselineDiagnosticsText $maxPolls

  [pscustomobject]@{
    SummarySeen = $summaryOn.Seen
    SummaryText = $summaryOn.Text
    DiagnosticsSeen = $diagnosticsOn.Seen
    DiagnosticsText = $diagnosticsOn.Text
    SummaryRestored = $summaryRestored.Seen
    DiagnosticsRestored = $diagnosticsRestored.Seen
  }
}
function Invoke-MonitorDisabledSemanticsCheck([IntPtr]$hwnd, [int]$monitorCheckboxId, [int]$maxPolls = 120) {
  $monitorCheckbox = [Native]::GetDlgItem($hwnd, $monitorCheckboxId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $baselineMonitorCheck = Get-CheckState $monitorCheckbox
  if ($baselineMonitorCheck -ne 1) {
    Invoke-CheckboxClick $monitorCheckbox
    [void](Wait-ForControlTextContains $hwnd $summaryControlId @("Monitor: On") 40)
    $baselineMonitorCheck = Get-CheckState $monitorCheckbox
  }

  $baselineSummaryText = Get-ControlTextById $hwnd $summaryControlId
  $baselineDiagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId

  Invoke-CheckboxClick $monitorCheckbox
  $summaryOff = Wait-ForControlTextContains $hwnd $summaryControlId @(
    "Render pipeline disabled for monitoring; render-only settings are inactive"
  ) $maxPolls
  $diagnosticsOff = Wait-ForControlTextContains $hwnd $diagnosticsControlId @(
    "Render pipeline disabled: render-only settings are inactive while monitor playback is off"
  ) $maxPolls

  if ($baselineMonitorCheck -ne (Get-CheckState $monitorCheckbox)) {
    Invoke-CheckboxClick $monitorCheckbox
  }

  $summaryRestored = Wait-ForControlTextEquals $hwnd $summaryControlId $baselineSummaryText $maxPolls
  $diagnosticsRestored = Wait-ForControlTextEquals $hwnd $diagnosticsControlId $baselineDiagnosticsText $maxPolls

  [pscustomobject]@{
    SummarySeen = $summaryOff.Seen
    SummaryText = $summaryOff.Text
    DiagnosticsSeen = $diagnosticsOff.Seen
    DiagnosticsText = $diagnosticsOff.Text
    SummaryRestored = $summaryRestored.Seen
    DiagnosticsRestored = $diagnosticsRestored.Seen
  }
}
function Invoke-MonitorOffWhileRunningConfigDriftCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$monitorCheckboxId, [int]$autoAlignCheckboxId, [int]$renderBackendComboId, [int]$renderDeviceComboId, [int]$renderRateComboId, [int]$renderChannelsComboId, [int]$renderTypeComboId, [int]$renderShareComboId, [int]$renderDriveComboId, [int]$renderBufferEditId, [int]$delayEditId, [int]$maxPolls = 120) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $monitorCheckbox = [Native]::GetDlgItem($hwnd, $monitorCheckboxId)
  $autoAlignCheckbox = [Native]::GetDlgItem($hwnd, $autoAlignCheckboxId)
  $renderBackendCombo = [Native]::GetDlgItem($hwnd, $renderBackendComboId)
  $renderDeviceCombo = [Native]::GetDlgItem($hwnd, $renderDeviceComboId)
  $renderRateCombo = [Native]::GetDlgItem($hwnd, $renderRateComboId)
  $renderChannelsCombo = [Native]::GetDlgItem($hwnd, $renderChannelsComboId)
  $renderTypeCombo = [Native]::GetDlgItem($hwnd, $renderTypeComboId)
  $renderShareCombo = [Native]::GetDlgItem($hwnd, $renderShareComboId)
  $renderDriveCombo = [Native]::GetDlgItem($hwnd, $renderDriveComboId)
  $renderBufferEdit = [Native]::GetDlgItem($hwnd, $renderBufferEditId)
  $delayEdit = [Native]::GetDlgItem($hwnd, $delayEditId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904

  $baselineMonitorCheck = Get-CheckState $monitorCheckbox
  $baselineAutoAlignCheck = Get-CheckState $autoAlignCheckbox
  if ($baselineMonitorCheck -ne 1) {
    Invoke-CheckboxClick $monitorCheckbox
    [void](Wait-ForControlTextContains $hwnd $summaryControlId @("Monitor: On") 40)
    $baselineMonitorCheck = Get-CheckState $monitorCheckbox
  }
  $normalizedAutoAlign = Set-CheckboxState $autoAlignCheckbox 0 $maxPolls

  $started = $false
  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $started = $true
  }

  $runningBaselineSeen = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningBaselineSeen = $true
      break
    }
  }

  Invoke-CheckboxClick $monitorCheckbox
  $summarySeen = $false
  $diagnosticsSeen = $false
  $runningPreserved = $false
  $renderSurfaceDisabledSeen = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $summarySeen =
      $summaryText.IndexOf("Render monitor playback is turned off for the next rebuilt or restarted session; the already-active render stream is still running.", [System.StringComparison]::Ordinal) -ge 0
    $diagnosticsSeen =
      $diagnosticsText.IndexOf("Render monitor playback is disabled in the current configuration, but the already-active session still has render monitoring until the next rebuild or restart.", [System.StringComparison]::Ordinal) -ge 0 -and
      $diagnosticsText.IndexOf("Active render mode:", [System.StringComparison]::Ordinal) -ge 0
    $renderSurfaceDisabledSeen =
      (-not [Native]::IsWindowEnabled($renderBackendCombo)) -and
      (-not [Native]::IsWindowEnabled($renderDeviceCombo)) -and
      (-not [Native]::IsWindowEnabled($renderRateCombo)) -and
      (-not [Native]::IsWindowEnabled($renderChannelsCombo)) -and
      (-not [Native]::IsWindowEnabled($renderTypeCombo)) -and
      (-not [Native]::IsWindowEnabled($renderShareCombo)) -and
      (-not [Native]::IsWindowEnabled($renderDriveCombo)) -and
      (-not [Native]::IsWindowEnabled($renderBufferEdit)) -and
      (-not [Native]::IsWindowEnabled($delayEdit)) -and
      (-not [Native]::IsWindowEnabled($autoAlignCheckbox))
    $runningPreserved =
      (-not [Native]::IsWindowEnabled($startButton)) -and
      [Native]::IsWindowEnabled($stopButton)
    if ($summarySeen -and $diagnosticsSeen -and $renderSurfaceDisabledSeen -and $runningPreserved) {
      break
    }
  }

  if ($baselineMonitorCheck -ne (Get-CheckState $monitorCheckbox)) {
    Invoke-CheckboxClick $monitorCheckbox
  }
  $monitorRestored = Wait-ForCheckboxState $monitorCheckbox $baselineMonitorCheck $maxPolls
  $autoAlignRestored = Set-CheckboxState $autoAlignCheckbox $baselineAutoAlignCheck $maxPolls

  $stoppedCleanly = $false
  if ($started -or $runningBaselineSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  [pscustomobject]@{
    NormalizedAutoAlign = $normalizedAutoAlign
    RunningBaselineSeen = $runningBaselineSeen
    SummarySeen = $summarySeen
    DiagnosticsSeen = $diagnosticsSeen
    RenderSurfaceDisabledSeen = $renderSurfaceDisabledSeen
    RunningPreserved = $runningPreserved
    MonitorRestored = $monitorRestored
    AutoAlignRestored = $autoAlignRestored
    StoppedCleanly = $stoppedCleanly
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-FollowDefaultsDefaultDeviceRebuildCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$followDefaultsCheckboxId, [int]$maxPolls = 160) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $diagnosticsControlId = 1904
  $wmAppDeviceChange = 0x8000 + 100

  $baselineFollowDefaultsCheck = Get-CheckState $followDefaultsCheckbox
  $startedForCheck = $false
  $runningSeen = $false

  if ((Get-CheckState $followDefaultsCheckbox) -ne 1) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }

  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $startedForCheck = $true
  }

  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningSeen = $true
      break
    }
  }

  [void][Native]::PostMessage($hwnd, $wmAppDeviceChange, [IntPtr]4, [IntPtr]0)
  $rebuildSeen = $false
  $runningRestored = $false
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $hasDeviceChangeLine =
      $diagnosticsText.IndexOf("Last device change: default-device-change => rebuild-success", [System.StringComparison]::Ordinal) -ge 0
    $hasRebuildLine =
      $diagnosticsText.IndexOf("Last rebuild: default-device-change => success", [System.StringComparison]::Ordinal) -ge 0
    $isRunningState =
      (-not [Native]::IsWindowEnabled($startButton)) -and [Native]::IsWindowEnabled($stopButton)
    if ($hasDeviceChangeLine -and $hasRebuildLine) {
      $rebuildSeen = $true
    }
    if ($hasDeviceChangeLine -and $hasRebuildLine -and $isRunningState) {
      $runningRestored = $true
      break
    }
  }

  $stoppedCleanly = $false
  if ($startedForCheck -or $runningSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  if ($baselineFollowDefaultsCheck -ne (Get-CheckState $followDefaultsCheckbox)) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }

  $followDefaultsRestored = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((Get-CheckState $followDefaultsCheckbox) -eq $baselineFollowDefaultsCheck) {
      $followDefaultsRestored = $true
      break
    }
  }

  [pscustomobject]@{
    RunningSeen = $runningSeen
    RebuildSeen = $rebuildSeen
    RunningRestored = $runningRestored
    StoppedCleanly = $stoppedCleanly
    FollowDefaultsRestored = $followDefaultsRestored
    DiagnosticsText = $diagnosticsText
  }
}
function Invoke-ManualDeviceNoRebuildCheck([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$followDefaultsCheckboxId, [int]$maxPolls = 160) {
  $startButton = [Native]::GetDlgItem($hwnd, $startButtonId)
  $stopButton = [Native]::GetDlgItem($hwnd, $stopButtonId)
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $summaryControlId = 1903
  $diagnosticsControlId = 1904
  $wmAppDeviceChange = 0x8000 + 100

  $baselineFollowDefaultsCheck = Get-CheckState $followDefaultsCheckbox
  $startedForCheck = $false
  $runningSeen = $false

  if ((Get-CheckState $followDefaultsCheckbox) -ne 0) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }

  if ([Native]::IsWindowEnabled($startButton)) {
    [void][Native]::PostMessage($hwnd, 0x0111, [IntPtr]$startButtonId, [IntPtr]::Zero)
    $startedForCheck = $true
  }

  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((-not [Native]::IsWindowEnabled($startButton)) -and
        [Native]::IsWindowEnabled($stopButton)) {
      $runningSeen = $true
      break
    }
  }

  [void][Native]::PostMessage($hwnd, $wmAppDeviceChange, [IntPtr]4, [IntPtr]0)
  $noRebuildSeen = $false
  $summarySeen = $false
  $runningPreserved = $false
  $summaryText = Get-ControlTextById $hwnd $summaryControlId
  $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
  $hasDeviceChangeLine = $false
  $hasRebuildLine = $false
  $isRunningState = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $summaryText = Get-ControlTextById $hwnd $summaryControlId
    $diagnosticsText = Get-ControlTextById $hwnd $diagnosticsControlId
    $hasDeviceChangeLine =
      $diagnosticsText.IndexOf("Last device change: default-device-change => tracked-no-rebuild", [System.StringComparison]::Ordinal) -ge 0
    $hasRebuildLine =
      $diagnosticsText.IndexOf("Last rebuild:", [System.StringComparison]::Ordinal) -ge 0
    $summarySeen =
      $summaryText.IndexOf("Running session note: the default-device-change was tracked, but the already-active session kept its current devices because follow-defaults is off.", [System.StringComparison]::Ordinal) -ge 0
    $isRunningState =
      (-not [Native]::IsWindowEnabled($startButton)) -and [Native]::IsWindowEnabled($stopButton)
    if ($hasDeviceChangeLine -and (-not $hasRebuildLine) -and $summarySeen -and $isRunningState) {
      $noRebuildSeen = $true
      $runningPreserved = $true
      break
    }
  }

  if (-not $summarySeen -and $hasDeviceChangeLine -and (-not $hasRebuildLine) -and $isRunningState) {
    for ($i = 0; $i -lt $maxPolls; $i++) {
      Start-Sleep -Milliseconds 100
      $summaryText = Get-ControlTextById $hwnd $summaryControlId
      if ($summaryText.IndexOf("Running session note: the default-device-change was tracked, but the already-active session kept its current devices because follow-defaults is off.", [System.StringComparison]::Ordinal) -ge 0) {
        $summarySeen = $true
        $noRebuildSeen = $true
        $runningPreserved = $true
        break
      }
    }
  }

  $stoppedCleanly = $false
  if ($startedForCheck -or $runningSeen) {
    $stoppedCleanly = Stop-SessionAndWait $hwnd $startButton $stopButton $stopButtonId $maxPolls
  } else {
    $stoppedCleanly = $true
  }

  if ($baselineFollowDefaultsCheck -ne (Get-CheckState $followDefaultsCheckbox)) {
    Invoke-CheckboxClick $followDefaultsCheckbox
    Start-Sleep -Milliseconds 200
  }

  $followDefaultsRestored = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((Get-CheckState $followDefaultsCheckbox) -eq $baselineFollowDefaultsCheck) {
      $followDefaultsRestored = $true
      break
    }
  }

  [pscustomobject]@{
    RunningSeen = $runningSeen
    NoRebuildSeen = $noRebuildSeen
    SummarySeen = $summarySeen
    RunningPreserved = $runningPreserved
    StoppedCleanly = $stoppedCleanly
    FollowDefaultsRestored = $followDefaultsRestored
    SummaryText = $summaryText
    DiagnosticsText = $diagnosticsText
  }
}
function Wait-ForCheckboxState([IntPtr]$handle, [int]$expectedState, [int]$maxPolls = 60) {
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    if ((Get-CheckState $handle) -eq $expectedState) {
      return $true
    }
  }
  return $false
}
function Set-CheckboxState([IntPtr]$handle, [int]$expectedState, [int]$maxPolls = 60) {
  if ($handle -eq [IntPtr]::Zero) {
    return $false
  }
  if ((Get-CheckState $handle) -ne $expectedState) {
    Invoke-CheckboxClick $handle
  }
  Wait-ForCheckboxState $handle $expectedState $maxPolls
}
function Invoke-NormalizeBusyProbeSurface([IntPtr]$hwnd, [int]$followDefaultsCheckboxId, [int]$monitorCheckboxId, [int]$autoAlignCheckboxId, [int]$captureDeviceComboId, [int]$renderDeviceComboId, [int[]]$renderFormatControlIds, [int]$maxPolls = 60) {
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $monitorCheckbox = [Native]::GetDlgItem($hwnd, $monitorCheckboxId)
  $autoAlignCheckbox = [Native]::GetDlgItem($hwnd, $autoAlignCheckboxId)
  $captureDeviceCombo = [Native]::GetDlgItem($hwnd, $captureDeviceComboId)
  $renderDeviceCombo = [Native]::GetDlgItem($hwnd, $renderDeviceComboId)
  $renderFormatControls = @()
  foreach ($id in $renderFormatControlIds) {
    $renderFormatControls += [Native]::GetDlgItem($hwnd, $id)
  }

  $baseline = [pscustomobject]@{
    FollowDefaults = Get-CheckState $followDefaultsCheckbox
    Monitor = Get-CheckState $monitorCheckbox
    AutoAlign = Get-CheckState $autoAlignCheckbox
  }

  $stateOk =
      (Set-CheckboxState $followDefaultsCheckbox 0 $maxPolls) -and
      (Set-CheckboxState $monitorCheckbox 1 $maxPolls) -and
      (Set-CheckboxState $autoAlignCheckbox 0 $maxPolls)

  $controlsEnabled = $false
  for ($i = 0; $i -lt $maxPolls; $i++) {
    Start-Sleep -Milliseconds 100
    $allEnabled =
        [Native]::IsWindowEnabled($captureDeviceCombo) -and
        [Native]::IsWindowEnabled($renderDeviceCombo)
    foreach ($control in $renderFormatControls) {
      if ($control -eq [IntPtr]::Zero -or -not [Native]::IsWindowEnabled($control)) {
        $allEnabled = $false
        break
      }
    }
    if ($allEnabled) {
      $controlsEnabled = $true
      break
    }
  }

  [pscustomobject]@{
    Baseline = $baseline
    NormalizedOk = $stateOk -and $controlsEnabled
  }
}
function Invoke-RestoreBusyProbeSurface([IntPtr]$hwnd, [pscustomobject]$baseline, [int]$followDefaultsCheckboxId, [int]$monitorCheckboxId, [int]$autoAlignCheckboxId, [int]$maxPolls = 60) {
  $followDefaultsCheckbox = [Native]::GetDlgItem($hwnd, $followDefaultsCheckboxId)
  $monitorCheckbox = [Native]::GetDlgItem($hwnd, $monitorCheckboxId)
  $autoAlignCheckbox = [Native]::GetDlgItem($hwnd, $autoAlignCheckboxId)

  $followRestored = Set-CheckboxState $followDefaultsCheckbox $baseline.FollowDefaults $maxPolls
  $monitorRestored = Set-CheckboxState $monitorCheckbox $baseline.Monitor $maxPolls
  $autoAlignRestored = Set-CheckboxState $autoAlignCheckbox $baseline.AutoAlign $maxPolls

  $followRestored -and $monitorRestored -and $autoAlignRestored
}
function Assert-IdleSessionSurface([IntPtr]$hwnd, [int]$startButtonId, [int]$stopButtonId, [int]$maxPolls = 60) {
  $startButton = Get-ControlHandle $hwnd $startButtonId
  $stopButton = Get-ControlHandle $hwnd $stopButtonId
  [void](Wait-ForIdleAfterStop $hwnd $startButton $stopButton $maxPolls)
}
$watchedControls = @(
  @{ Name = "Refresh"; Id = 1003; Type = "text" },
  @{ Name = "CaptureBackend"; Id = 1101; Type = "combo" },
  @{ Name = "RenderBackend"; Id = 1102; Type = "combo" },
  @{ Name = "SourceMode"; Id = 1103; Type = "combo" },
  @{ Name = "CaptureDevice"; Id = 1104; Type = "combo" },
  @{ Name = "RenderDevice"; Id = 1105; Type = "combo" },
  @{ Name = "DelayMs"; Id = 1106; Type = "text" },
  @{ Name = "Dump"; Id = 1107; Type = "check" },
  @{ Name = "CaptureRate"; Id = 1108; Type = "combo" },
  @{ Name = "CaptureChannels"; Id = 1109; Type = "combo" },
  @{ Name = "CaptureType"; Id = 1110; Type = "combo" },
  @{ Name = "CaptureShare"; Id = 1111; Type = "combo" },
  @{ Name = "CaptureDrive"; Id = 1112; Type = "combo" },
  @{ Name = "DumpPath"; Id = 1113; Type = "text" },
  @{ Name = "DumpType"; Id = 1114; Type = "combo" },
  @{ Name = "CaptureBuffer"; Id = 1115; Type = "text" },
  @{ Name = "RenderBuffer"; Id = 1116; Type = "text" },
  @{ Name = "Monitor"; Id = 1117; Type = "check" },
  @{ Name = "FollowDefaults"; Id = 1118; Type = "check" },
  @{ Name = "RenderRate"; Id = 1119; Type = "combo" },
  @{ Name = "RenderChannels"; Id = 1120; Type = "combo" },
  @{ Name = "RenderType"; Id = 1121; Type = "combo" },
  @{ Name = "AutoAlign"; Id = 1122; Type = "check" },
  @{ Name = "RenderShare"; Id = 1123; Type = "combo" },
  @{ Name = "RenderDrive"; Id = 1124; Type = "combo" }
)
Get-Process winaudio -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $guiExe -PassThru
$hwnd = Wait-ForWindowHandle $p 100
if($p.HasExited -or $hwnd -eq [IntPtr]::Zero){
  Write-Host "GUI status: EXITED_OR_WINDOW_NOT_FOUND"
  throw "GUI smoke failed: window did not become available."
} else {
  Write-Host "GUI status: RUNNING"
  $sourceMode = Invoke-SourceModeToggleCheck $hwnd 1103 1104 1105 80
  Write-Host "GUI source-mode baseline: $($sourceMode.BaselineSource)"
  Write-Host "GUI source-mode baseline capture label: $($sourceMode.BaselineCaptureLabel)"
  Write-Host "GUI source-mode baseline device count line: $($sourceMode.BaselineDeviceCountLine)"
  Write-Host "GUI source-mode loopback capture remap seen: $($sourceMode.LoopbackSeen)"
  Write-Host "GUI source-mode loopback label seen: $($sourceMode.LoopbackLabelSeen)"
  Write-Host "GUI source-mode loopback device-count line seen: $($sourceMode.LoopbackDeviceCountSeen)"
  Write-Host "GUI source-mode loopback combo items match render list: $($sourceMode.LoopbackItemsMatchSeen)"
  Write-Host "GUI source-mode label restored: $($sourceMode.LabelRestored)"
  Write-Host "GUI source-mode device-count line restored: $($sourceMode.DeviceCountRestored)"
  Write-Host "GUI source-mode capture items restored: $($sourceMode.CaptureItemsRestored)"
  Write-Host "GUI source-mode restored: $($sourceMode.Restored)"
  if (-not $sourceMode.LoopbackSeen -or
      -not $sourceMode.LoopbackLabelSeen -or
      -not $sourceMode.LoopbackDeviceCountSeen -or
      -not $sourceMode.LoopbackItemsMatchSeen -or
      -not $sourceMode.LabelRestored -or
      -not $sourceMode.DeviceCountRestored -or
      -not $sourceMode.CaptureItemsRestored -or
      -not $sourceMode.Restored) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: source-mode toggle did not remap loopback capture devices correctly."
  }
  $manualSelectionPersistence = Invoke-ManualDeviceSelectionPersistenceCheck $hwnd 1104 1105 1003 120
  Write-Host "GUI manual device selection eligible: $($manualSelectionPersistence.Eligible)"
  Write-Host "GUI manual capture selection seen: $($manualSelectionPersistence.CaptureSelectedSeen)"
  Write-Host "GUI manual render selection seen: $($manualSelectionPersistence.RenderSelectedSeen)"
  Write-Host "GUI manual capture selection persisted: $($manualSelectionPersistence.CapturePersisted)"
  Write-Host "GUI manual render selection persisted: $($manualSelectionPersistence.RenderPersisted)"
  Write-Host "GUI manual device selection restored: $($manualSelectionPersistence.Restored)"
  Write-Host "GUI manual device selection diagnostics after refresh: $($manualSelectionPersistence.DiagnosticsText)"
  if ($manualSelectionPersistence.Eligible -and
      (-not $manualSelectionPersistence.CaptureSelectedSeen -or
       -not $manualSelectionPersistence.RenderSelectedSeen -or
       -not $manualSelectionPersistence.CapturePersisted -or
       -not $manualSelectionPersistence.RenderPersisted -or
       -not $manualSelectionPersistence.Restored)) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: manual device selections did not persist across refresh."
  }
  $sourceModeWhileRunning = Invoke-SourceModeWhileRunningCheck $hwnd 1103 1104 1105 1001 1002 120
  Write-Host "GUI source-mode while-running baseline seen: $($sourceModeWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI source-mode while-running loopback seen: $($sourceModeWhileRunning.LoopbackSeen)"
  Write-Host "GUI source-mode while-running running preserved: $($sourceModeWhileRunning.RunningPreserved)"
  Write-Host "GUI source-mode while-running summary seen: $($sourceModeWhileRunning.SummarySeen)"
  Write-Host "GUI source-mode while-running summary drift seen: $($sourceModeWhileRunning.SummaryDriftSeen)"
  Write-Host "GUI source-mode while-running running restored seen: $($sourceModeWhileRunning.RunningRestoredSeen)"
  Write-Host "GUI source-mode while-running restored: $($sourceModeWhileRunning.Restored)"
  Write-Host "GUI source-mode while-running stopped cleanly: $($sourceModeWhileRunning.StoppedCleanly)"
  Write-Host "GUI source-mode while-running final source text: $($sourceModeWhileRunning.FinalSourceText)"
  Write-Host "GUI source-mode while-running final capture selected: $($sourceModeWhileRunning.FinalCaptureSelected)"
  Write-Host "GUI source-mode while-running final capture label: $($sourceModeWhileRunning.FinalCaptureLabel)"
  Write-Host "GUI source-mode while-running final device-count line: $($sourceModeWhileRunning.FinalDeviceCountLine)"
  Write-Host "GUI source-mode while-running summary text: $($sourceModeWhileRunning.SummaryText)"
  Write-Host "GUI source-mode while-running restored summary text: $($sourceModeWhileRunning.RestoredSummaryText)"
  if (-not $sourceModeWhileRunning.RunningBaselineSeen -or
      -not $sourceModeWhileRunning.LoopbackSeen -or
      -not $sourceModeWhileRunning.RunningPreserved -or
      -not $sourceModeWhileRunning.SummarySeen -or
      -not $sourceModeWhileRunning.SummaryDriftSeen -or
      -not $sourceModeWhileRunning.Restored -or
      -not $sourceModeWhileRunning.StoppedCleanly) {
    Stop-ProcessByIdIfRunning $p.Id
    throw "GUI smoke failed: source-mode change while running did not preserve the active session correctly."
  }
  Assert-IdleSessionSurface $hwnd 1001 1002 60
  $followDefaultsSourceModeWhileRunning = Invoke-FollowDefaultsSourceModeWhileRunningCheck $hwnd 1103 1118 1104 1105 1001 1002 120
  Write-Host "GUI follow-defaults source-mode while-running baseline seen: $($followDefaultsSourceModeWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults source-mode while-running loopback seen: $($followDefaultsSourceModeWhileRunning.LoopbackSeen)"
  Write-Host "GUI follow-defaults source-mode while-running summary seen: $($followDefaultsSourceModeWhileRunning.SummarySeen)"
  Write-Host "GUI follow-defaults source-mode while-running diagnostics seen: $($followDefaultsSourceModeWhileRunning.DiagnosticsSeen)"
  Write-Host "GUI follow-defaults source-mode while-running running preserved: $($followDefaultsSourceModeWhileRunning.RunningPreserved)"
  Write-Host "GUI follow-defaults source-mode while-running restored: $($followDefaultsSourceModeWhileRunning.Restored)"
  Write-Host "GUI follow-defaults source-mode while-running stopped cleanly: $($followDefaultsSourceModeWhileRunning.StoppedCleanly)"
  Write-Host "GUI follow-defaults source-mode while-running final source text: $($followDefaultsSourceModeWhileRunning.FinalSourceText)"
  Write-Host "GUI follow-defaults source-mode while-running final follow-defaults state: $($followDefaultsSourceModeWhileRunning.FinalFollowDefaultsState)"
  Write-Host "GUI follow-defaults source-mode while-running final capture enabled: $($followDefaultsSourceModeWhileRunning.FinalCaptureEnabled)"
  Write-Host "GUI follow-defaults source-mode while-running final render enabled: $($followDefaultsSourceModeWhileRunning.FinalRenderEnabled)"
  Write-Host "GUI follow-defaults source-mode while-running final summary text: $($followDefaultsSourceModeWhileRunning.FinalSummaryText)"
  Write-Host "GUI follow-defaults source-mode while-running final diagnostics text: $($followDefaultsSourceModeWhileRunning.FinalDiagnosticsText)"
  Write-Host "GUI follow-defaults source-mode while-running summary text: $($followDefaultsSourceModeWhileRunning.SummaryText)"
  Write-Host "GUI follow-defaults source-mode while-running diagnostics text: $($followDefaultsSourceModeWhileRunning.DiagnosticsText)"
  if (-not $followDefaultsSourceModeWhileRunning.RunningBaselineSeen -or
      -not $followDefaultsSourceModeWhileRunning.LoopbackSeen -or
      -not $followDefaultsSourceModeWhileRunning.SummarySeen -or
      -not $followDefaultsSourceModeWhileRunning.DiagnosticsSeen -or
      -not $followDefaultsSourceModeWhileRunning.RunningPreserved -or
      -not $followDefaultsSourceModeWhileRunning.Restored -or
      -not $followDefaultsSourceModeWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults source-mode change while running did not preserve the active session correctly."
  }
  Assert-IdleSessionSurface $hwnd 1001 1002 60
  $followDefaultsRenderBackendWhileRunning = Invoke-FollowDefaultsRenderBackendWhileRunningCheck $hwnd 1102 1118 1104 1105 1001 1002 120
  Write-Host "GUI follow-defaults render-backend while-running baseline seen: $($followDefaultsRenderBackendWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults render-backend while-running backend seen: $($followDefaultsRenderBackendWhileRunning.BackendSeen)"
  Write-Host "GUI follow-defaults render-backend while-running summary seen: $($followDefaultsRenderBackendWhileRunning.SummarySeen)"
  Write-Host "GUI follow-defaults render-backend while-running diagnostics seen: $($followDefaultsRenderBackendWhileRunning.DiagnosticsSeen)"
  Write-Host "GUI follow-defaults render-backend while-running running preserved: $($followDefaultsRenderBackendWhileRunning.RunningPreserved)"
  Write-Host "GUI follow-defaults render-backend while-running restored: $($followDefaultsRenderBackendWhileRunning.Restored)"
  Write-Host "GUI follow-defaults render-backend while-running stopped cleanly: $($followDefaultsRenderBackendWhileRunning.StoppedCleanly)"
  Write-Host "GUI follow-defaults render-backend while-running final backend text: $($followDefaultsRenderBackendWhileRunning.FinalBackendText)"
  Write-Host "GUI follow-defaults render-backend while-running final follow-defaults state: $($followDefaultsRenderBackendWhileRunning.FinalFollowDefaultsState)"
  Write-Host "GUI follow-defaults render-backend while-running final capture enabled: $($followDefaultsRenderBackendWhileRunning.FinalCaptureEnabled)"
  Write-Host "GUI follow-defaults render-backend while-running final render enabled: $($followDefaultsRenderBackendWhileRunning.FinalRenderEnabled)"
  Write-Host "GUI follow-defaults render-backend while-running summary text: $($followDefaultsRenderBackendWhileRunning.SummaryText)"
  Write-Host "GUI follow-defaults render-backend while-running diagnostics text: $($followDefaultsRenderBackendWhileRunning.DiagnosticsText)"
  if (-not $followDefaultsRenderBackendWhileRunning.RunningBaselineSeen -or
      -not $followDefaultsRenderBackendWhileRunning.BackendSeen -or
      -not $followDefaultsRenderBackendWhileRunning.SummarySeen -or
      -not $followDefaultsRenderBackendWhileRunning.DiagnosticsSeen -or
      -not $followDefaultsRenderBackendWhileRunning.RunningPreserved -or
      -not $followDefaultsRenderBackendWhileRunning.Restored -or
      -not $followDefaultsRenderBackendWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults render-backend change while running did not preserve the active session correctly."
  }
  Assert-IdleSessionSurface $hwnd 1001 1002 60
  $followDefaultsCaptureBackendWhileRunning = Invoke-FollowDefaultsCaptureBackendWhileRunningCheck $hwnd 1101 1118 1104 1105 1001 1002 120
  Write-Host "GUI follow-defaults capture-backend while-running baseline seen: $($followDefaultsCaptureBackendWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults capture-backend while-running backend seen: $($followDefaultsCaptureBackendWhileRunning.BackendSeen)"
  Write-Host "GUI follow-defaults capture-backend while-running summary seen: $($followDefaultsCaptureBackendWhileRunning.SummarySeen)"
  Write-Host "GUI follow-defaults capture-backend while-running diagnostics seen: $($followDefaultsCaptureBackendWhileRunning.DiagnosticsSeen)"
  Write-Host "GUI follow-defaults capture-backend while-running running preserved: $($followDefaultsCaptureBackendWhileRunning.RunningPreserved)"
  Write-Host "GUI follow-defaults capture-backend while-running restored: $($followDefaultsCaptureBackendWhileRunning.Restored)"
  Write-Host "GUI follow-defaults capture-backend while-running stopped cleanly: $($followDefaultsCaptureBackendWhileRunning.StoppedCleanly)"
  Write-Host "GUI follow-defaults capture-backend while-running final backend text: $($followDefaultsCaptureBackendWhileRunning.FinalBackendText)"
  Write-Host "GUI follow-defaults capture-backend while-running final follow-defaults state: $($followDefaultsCaptureBackendWhileRunning.FinalFollowDefaultsState)"
  Write-Host "GUI follow-defaults capture-backend while-running final capture enabled: $($followDefaultsCaptureBackendWhileRunning.FinalCaptureEnabled)"
  Write-Host "GUI follow-defaults capture-backend while-running final render enabled: $($followDefaultsCaptureBackendWhileRunning.FinalRenderEnabled)"
  Write-Host "GUI follow-defaults capture-backend while-running summary text: $($followDefaultsCaptureBackendWhileRunning.SummaryText)"
  Write-Host "GUI follow-defaults capture-backend while-running diagnostics text: $($followDefaultsCaptureBackendWhileRunning.DiagnosticsText)"
  if (-not $followDefaultsCaptureBackendWhileRunning.RunningBaselineSeen -or
      -not $followDefaultsCaptureBackendWhileRunning.BackendSeen -or
      -not $followDefaultsCaptureBackendWhileRunning.SummarySeen -or
      -not $followDefaultsCaptureBackendWhileRunning.DiagnosticsSeen -or
      -not $followDefaultsCaptureBackendWhileRunning.RunningPreserved -or
      -not $followDefaultsCaptureBackendWhileRunning.Restored -or
      -not $followDefaultsCaptureBackendWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults capture-backend change while running did not preserve the active session correctly."
  }
  Assert-IdleSessionSurface $hwnd 1001 1002 60
  $followDefaultsMonitorWhileRunning = Invoke-FollowDefaultsMonitorOffWhileRunningCheck $hwnd 1118 1117 1104 1105 1102 1119 1120 1121 1123 1124 1116 1106 1122 1001 1002 120
  Write-Host "GUI follow-defaults monitor-off while-running auto-align normalized: $($followDefaultsMonitorWhileRunning.NormalizedAutoAlign)"
  Write-Host "GUI follow-defaults monitor-off while-running baseline seen: $($followDefaultsMonitorWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults monitor-off while-running summary seen: $($followDefaultsMonitorWhileRunning.SummarySeen)"
  Write-Host "GUI follow-defaults monitor-off while-running diagnostics seen: $($followDefaultsMonitorWhileRunning.DiagnosticsSeen)"
  Write-Host "GUI follow-defaults monitor-off while-running render surface disabled seen: $($followDefaultsMonitorWhileRunning.RenderSurfaceDisabledSeen)"
  Write-Host "GUI follow-defaults monitor-off while-running running preserved: $($followDefaultsMonitorWhileRunning.RunningPreserved)"
  Write-Host "GUI follow-defaults monitor-off while-running checkbox restored: $($followDefaultsMonitorWhileRunning.MonitorRestored)"
  Write-Host "GUI follow-defaults monitor-off while-running follow restored: $($followDefaultsMonitorWhileRunning.FollowDefaultsRestored)"
  Write-Host "GUI follow-defaults monitor-off while-running auto-align restored: $($followDefaultsMonitorWhileRunning.AutoAlignRestored)"
  Write-Host "GUI follow-defaults monitor-off while-running restored: $($followDefaultsMonitorWhileRunning.Restored)"
  Write-Host "GUI follow-defaults monitor-off while-running stopped cleanly: $($followDefaultsMonitorWhileRunning.StoppedCleanly)"
  Write-Host "GUI follow-defaults monitor-off while-running summary text: $($followDefaultsMonitorWhileRunning.SummaryText)"
  Write-Host "GUI follow-defaults monitor-off while-running diagnostics text: $($followDefaultsMonitorWhileRunning.DiagnosticsText)"
  if (-not $followDefaultsMonitorWhileRunning.NormalizedAutoAlign -or
      -not $followDefaultsMonitorWhileRunning.RunningBaselineSeen -or
      -not $followDefaultsMonitorWhileRunning.SummarySeen -or
      -not $followDefaultsMonitorWhileRunning.DiagnosticsSeen -or
      -not $followDefaultsMonitorWhileRunning.RenderSurfaceDisabledSeen -or
      -not $followDefaultsMonitorWhileRunning.RunningPreserved -or
      -not $followDefaultsMonitorWhileRunning.MonitorRestored -or
      -not $followDefaultsMonitorWhileRunning.FollowDefaultsRestored -or
      -not $followDefaultsMonitorWhileRunning.AutoAlignRestored -or
      -not $followDefaultsMonitorWhileRunning.Restored -or
      -not $followDefaultsMonitorWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults monitor-off while running did not preserve the active session correctly."
  }
  $renderBackendWhileRunning = Invoke-RenderBackendWhileRunningCheck $hwnd 1102 1105 1001 1002 120
  Write-Host "GUI render-backend while-running baseline seen: $($renderBackendWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI render-backend while-running backend seen: $($renderBackendWhileRunning.BackendSeen)"
  Write-Host "GUI render-backend while-running running preserved: $($renderBackendWhileRunning.RunningPreserved)"
  Write-Host "GUI render-backend while-running summary seen: $($renderBackendWhileRunning.SummarySeen)"
  Write-Host "GUI render-backend while-running summary drift seen: $($renderBackendWhileRunning.SummaryDriftSeen)"
  Write-Host "GUI render-backend while-running diagnostics seen: $($renderBackendWhileRunning.DiagnosticsSeen)"
  Write-Host "GUI render-backend while-running selected render id seen: $($renderBackendWhileRunning.SelectedRenderIdSeen)"
  Write-Host "GUI render-backend while-running restored: $($renderBackendWhileRunning.Restored)"
  Write-Host "GUI render-backend while-running stopped cleanly: $($renderBackendWhileRunning.StoppedCleanly)"
  Write-Host "GUI render-backend while-running summary text: $($renderBackendWhileRunning.SummaryText)"
  Write-Host "GUI render-backend while-running diagnostics text: $($renderBackendWhileRunning.DiagnosticsText)"
  if (-not $renderBackendWhileRunning.RunningBaselineSeen -or
      -not $renderBackendWhileRunning.BackendSeen -or
      -not $renderBackendWhileRunning.RunningPreserved -or
      -not $renderBackendWhileRunning.SummarySeen -or
      -not $renderBackendWhileRunning.SummaryDriftSeen -or
      -not $renderBackendWhileRunning.DiagnosticsSeen -or
      -not $renderBackendWhileRunning.SelectedRenderIdSeen -or
      -not $renderBackendWhileRunning.Restored -or
      -not $renderBackendWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: render-backend change while running did not preserve the active session correctly."
  }
  $captureBackendWhileRunning = Invoke-CaptureBackendWhileRunningCheck $hwnd 1101 1104 1001 1002 120
  Write-Host "GUI capture-backend while-running baseline seen: $($captureBackendWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI capture-backend while-running backend seen: $($captureBackendWhileRunning.BackendSeen)"
  Write-Host "GUI capture-backend while-running running preserved: $($captureBackendWhileRunning.RunningPreserved)"
  Write-Host "GUI capture-backend while-running summary seen: $($captureBackendWhileRunning.SummarySeen)"
  Write-Host "GUI capture-backend while-running summary drift seen: $($captureBackendWhileRunning.SummaryDriftSeen)"
  Write-Host "GUI capture-backend while-running diagnostics seen: $($captureBackendWhileRunning.DiagnosticsSeen)"
  Write-Host "GUI capture-backend while-running selected capture id seen: $($captureBackendWhileRunning.SelectedCaptureIdSeen)"
  Write-Host "GUI capture-backend while-running restored: $($captureBackendWhileRunning.Restored)"
  Write-Host "GUI capture-backend while-running stopped cleanly: $($captureBackendWhileRunning.StoppedCleanly)"
  Write-Host "GUI capture-backend while-running summary text: $($captureBackendWhileRunning.SummaryText)"
  Write-Host "GUI capture-backend while-running diagnostics text: $($captureBackendWhileRunning.DiagnosticsText)"
  if (-not $captureBackendWhileRunning.RunningBaselineSeen -or
      -not $captureBackendWhileRunning.BackendSeen -or
      -not $captureBackendWhileRunning.RunningPreserved -or
      -not $captureBackendWhileRunning.SummarySeen -or
      -not $captureBackendWhileRunning.SummaryDriftSeen -or
      -not $captureBackendWhileRunning.DiagnosticsSeen -or
      -not $captureBackendWhileRunning.SelectedCaptureIdSeen -or
      -not $captureBackendWhileRunning.Restored -or
      -not $captureBackendWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: capture-backend change while running did not preserve the active session correctly."
  }
  $manualDeviceWhileRunning = Invoke-ManualDeviceChangeWhileRunningCheck $hwnd 1104 1105 1001 1002 120
  Write-Host "GUI manual-device while-running eligible: $($manualDeviceWhileRunning.Eligible)"
  Write-Host "GUI manual-device while-running baseline seen: $($manualDeviceWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI manual-device while-running capture changed seen: $($manualDeviceWhileRunning.CaptureChangedSeen)"
  Write-Host "GUI manual-device while-running render changed seen: $($manualDeviceWhileRunning.RenderChangedSeen)"
  Write-Host "GUI manual-device while-running summary seen: $($manualDeviceWhileRunning.SummarySeen)"
  Write-Host "GUI manual-device while-running running preserved: $($manualDeviceWhileRunning.RunningPreserved)"
  Write-Host "GUI manual-device while-running restored: $($manualDeviceWhileRunning.Restored)"
  Write-Host "GUI manual-device while-running stopped cleanly: $($manualDeviceWhileRunning.StoppedCleanly)"
  Write-Host "GUI manual-device while-running summary text: $($manualDeviceWhileRunning.SummaryText)"
  Write-Host "GUI manual-device while-running diagnostics text: $($manualDeviceWhileRunning.DiagnosticsText)"
  if ($manualDeviceWhileRunning.Eligible -and
      (-not $manualDeviceWhileRunning.RunningBaselineSeen -or
       -not $manualDeviceWhileRunning.CaptureChangedSeen -or
       -not $manualDeviceWhileRunning.RenderChangedSeen -or
       -not $manualDeviceWhileRunning.SummarySeen -or
       -not $manualDeviceWhileRunning.RunningPreserved -or
       -not $manualDeviceWhileRunning.Restored -or
       -not $manualDeviceWhileRunning.StoppedCleanly)) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: manual device changes while running did not preserve the active session correctly."
  }
  $manualDeviceRefreshWhileRunning = Invoke-ManualDeviceChangeThenRefreshWhileRunningCheck $hwnd 1104 1105 1003 1001 1002 120
  Write-Host "GUI manual-device refresh while-running eligible: $($manualDeviceRefreshWhileRunning.Eligible)"
  Write-Host "GUI manual-device refresh while-running baseline seen: $($manualDeviceRefreshWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI manual-device refresh while-running persisted seen: $($manualDeviceRefreshWhileRunning.PersistedSeen)"
  Write-Host "GUI manual-device refresh while-running running preserved: $($manualDeviceRefreshWhileRunning.RunningPreserved)"
  Write-Host "GUI manual-device refresh while-running restored: $($manualDeviceRefreshWhileRunning.Restored)"
  Write-Host "GUI manual-device refresh while-running stopped cleanly: $($manualDeviceRefreshWhileRunning.StoppedCleanly)"
  Write-Host "GUI manual-device refresh while-running diagnostics text: $($manualDeviceRefreshWhileRunning.DiagnosticsText)"
  if ($manualDeviceRefreshWhileRunning.Eligible -and
      (-not $manualDeviceRefreshWhileRunning.RunningBaselineSeen -or
       -not $manualDeviceRefreshWhileRunning.PersistedSeen -or
       -not $manualDeviceRefreshWhileRunning.RunningPreserved -or
       -not $manualDeviceRefreshWhileRunning.Restored -or
       -not $manualDeviceRefreshWhileRunning.StoppedCleanly)) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: manual device changes plus refresh while running did not preserve the active session correctly."
  }
  $autoAlign = Invoke-AutoAlignToggleCheck $hwnd 1122 @(1119, 1120, 1121) 60
  $autoAlignNote = ""
  for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Milliseconds 100
    $autoAlignNote = Get-ControlTextById $hwnd 1906
    if ($autoAlignNote -eq "When render auto-align is on, effective render format follows capture.") {
      break
    }
  }
  Write-Host "GUI auto-align toggled render controls seen: $($autoAlign.ToggledSeen)"
  Write-Host "GUI auto-align restored: $($autoAlign.Restored)"
  Write-Host "GUI auto-align explanatory note: $autoAlignNote"
  if (-not $autoAlign.ToggledSeen -or
      -not $autoAlign.Restored -or
      $autoAlignNote -ne "When render auto-align is on, effective render format follows capture.") {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: auto-align toggle did not update render-format controls correctly."
  }
  $autoAlignMonitorComposition = Invoke-AutoAlignMonitorCompositionCheck $hwnd 1122 1117 @(1119, 1120, 1121) 60
  Write-Host "GUI auto-align/monitor normalized baseline ok: $($autoAlignMonitorComposition.NormalizedOk)"
  Write-Host "GUI auto-align/monitor auto-align disabled seen: $($autoAlignMonitorComposition.AutoAlignDisabledSeen)"
  Write-Host "GUI auto-align/monitor monitor-off while auto-align seen: $($autoAlignMonitorComposition.MonitorOffWhileAutoAlignSeen)"
  Write-Host "GUI auto-align/monitor auto-align checkbox disabled seen: $($autoAlignMonitorComposition.AutoAlignCheckboxDisabledSeen)"
  Write-Host "GUI auto-align/monitor monitor restored while auto-align seen: $($autoAlignMonitorComposition.MonitorRestoredWhileAutoAlignSeen)"
  Write-Host "GUI auto-align/monitor normalized restored: $($autoAlignMonitorComposition.RestoredNormalized)"
  Write-Host "GUI auto-align/monitor baseline restored: $($autoAlignMonitorComposition.RestoredBaseline)"
  if (-not $autoAlignMonitorComposition.NormalizedOk -or
      -not $autoAlignMonitorComposition.AutoAlignDisabledSeen -or
      -not $autoAlignMonitorComposition.MonitorOffWhileAutoAlignSeen -or
      -not $autoAlignMonitorComposition.AutoAlignCheckboxDisabledSeen -or
      -not $autoAlignMonitorComposition.MonitorRestoredWhileAutoAlignSeen -or
      -not $autoAlignMonitorComposition.RestoredNormalized -or
      -not $autoAlignMonitorComposition.RestoredBaseline) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: auto-align and monitor composition state did not behave correctly."
  }
  $followDefaults = Invoke-FollowDefaultsToggleCheck $hwnd 1118 @(1104, 1105) 60
  Write-Host "GUI follow-defaults toggled device controls seen: $($followDefaults.ToggledSeen)"
  Write-Host "GUI follow-defaults restored: $($followDefaults.Restored)"
  if (-not $followDefaults.ToggledSeen -or -not $followDefaults.Restored) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults toggle did not update device controls correctly."
  }
  $followDefaultsWhileRunning = Invoke-FollowDefaultsWhileRunningCheck $hwnd 1001 1002 1118 1104 1105 120
  Write-Host "GUI follow-defaults while-running baseline seen: $($followDefaultsWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults while-running disabled seen: $($followDefaultsWhileRunning.DisabledWhileRunningSeen)"
  Write-Host "GUI follow-defaults while-running summary seen: $($followDefaultsWhileRunning.SummarySeen)"
  Write-Host "GUI follow-defaults while-running diagnostics seen: $($followDefaultsWhileRunning.DiagnosticsSeen)"
  Write-Host "GUI follow-defaults while-running restored seen: $($followDefaultsWhileRunning.RestoredRunningSeen)"
  Write-Host "GUI follow-defaults while-running stopped cleanly: $($followDefaultsWhileRunning.StoppedCleanly)"
  Write-Host "GUI follow-defaults while-running summary text: $($followDefaultsWhileRunning.SummaryText)"
  Write-Host "GUI follow-defaults while-running diagnostics text: $($followDefaultsWhileRunning.DiagnosticsText)"
  if (-not $followDefaultsWhileRunning.RunningBaselineSeen -or
      -not $followDefaultsWhileRunning.DisabledWhileRunningSeen -or
      -not $followDefaultsWhileRunning.SummarySeen -or
      -not $followDefaultsWhileRunning.DiagnosticsSeen -or
      -not $followDefaultsWhileRunning.RestoredRunningSeen -or
      -not $followDefaultsWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults toggle did not preserve the running session correctly."
  }
  $followDefaultsCaptureDrift = Invoke-FollowDefaultsRunningCaptureConfigDriftCheck $hwnd 1001 1002 1118 1104 1105 1108 1122 120
  Write-Host "GUI follow-defaults capture drift baseline set: $($followDefaultsCaptureDrift.BaselineSet)"
  Write-Host "GUI follow-defaults capture drift auto-align normalized: $($followDefaultsCaptureDrift.NormalizedAutoAlign)"
  Write-Host "GUI follow-defaults capture drift running baseline seen: $($followDefaultsCaptureDrift.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults capture drift current configured changed: $($followDefaultsCaptureDrift.DriftSeen)"
  Write-Host "GUI follow-defaults capture drift active requested preserved: $($followDefaultsCaptureDrift.RequestedPreserved)"
  Write-Host "GUI follow-defaults capture drift running preserved: $($followDefaultsCaptureDrift.RunningPreserved)"
  Write-Host "GUI follow-defaults capture drift checkbox restored: $($followDefaultsCaptureDrift.FollowDefaultsRestored)"
  Write-Host "GUI follow-defaults capture drift restored: $($followDefaultsCaptureDrift.Restored)"
  Write-Host "GUI follow-defaults capture drift auto-align restored: $($followDefaultsCaptureDrift.AutoAlignRestored)"
  Write-Host "GUI follow-defaults capture drift stopped cleanly: $($followDefaultsCaptureDrift.StoppedCleanly)"
  Write-Host "GUI follow-defaults capture drift diagnostics text: $($followDefaultsCaptureDrift.DiagnosticsText)"
  if (-not $followDefaultsCaptureDrift.BaselineSet -or
      -not $followDefaultsCaptureDrift.NormalizedAutoAlign -or
      -not $followDefaultsCaptureDrift.RunningBaselineSeen -or
      -not $followDefaultsCaptureDrift.DriftSeen -or
      -not $followDefaultsCaptureDrift.RequestedPreserved -or
      -not $followDefaultsCaptureDrift.RunningPreserved -or
      -not $followDefaultsCaptureDrift.FollowDefaultsRestored -or
      -not $followDefaultsCaptureDrift.Restored -or
      -not $followDefaultsCaptureDrift.AutoAlignRestored -or
      -not $followDefaultsCaptureDrift.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults capture configuration edits did not preserve tracked active-session request semantics."
  }
  $followDefaultsRenderDrift = Invoke-FollowDefaultsRunningRenderConfigDriftCheck $hwnd 1001 1002 1118 1104 1105 1119 1122 120
  Write-Host "GUI follow-defaults render drift baseline set: $($followDefaultsRenderDrift.BaselineSet)"
  Write-Host "GUI follow-defaults render drift auto-align normalized: $($followDefaultsRenderDrift.NormalizedAutoAlign)"
  Write-Host "GUI follow-defaults render drift running baseline seen: $($followDefaultsRenderDrift.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults render drift current configured changed: $($followDefaultsRenderDrift.DriftSeen)"
  Write-Host "GUI follow-defaults render drift active requested preserved: $($followDefaultsRenderDrift.RequestedPreserved)"
  Write-Host "GUI follow-defaults render drift running preserved: $($followDefaultsRenderDrift.RunningPreserved)"
  Write-Host "GUI follow-defaults render drift checkbox restored: $($followDefaultsRenderDrift.FollowDefaultsRestored)"
  Write-Host "GUI follow-defaults render drift restored: $($followDefaultsRenderDrift.Restored)"
  Write-Host "GUI follow-defaults render drift auto-align restored: $($followDefaultsRenderDrift.AutoAlignRestored)"
  Write-Host "GUI follow-defaults render drift stopped cleanly: $($followDefaultsRenderDrift.StoppedCleanly)"
  Write-Host "GUI follow-defaults render drift diagnostics text: $($followDefaultsRenderDrift.DiagnosticsText)"
  if (-not $followDefaultsRenderDrift.BaselineSet -or
      -not $followDefaultsRenderDrift.NormalizedAutoAlign -or
      -not $followDefaultsRenderDrift.RunningBaselineSeen -or
      -not $followDefaultsRenderDrift.DriftSeen -or
      -not $followDefaultsRenderDrift.RequestedPreserved -or
      -not $followDefaultsRenderDrift.RunningPreserved -or
      -not $followDefaultsRenderDrift.FollowDefaultsRestored -or
      -not $followDefaultsRenderDrift.Restored -or
      -not $followDefaultsRenderDrift.AutoAlignRestored -or
      -not $followDefaultsRenderDrift.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults render configuration edits did not preserve tracked active-session request semantics."
  }
  $followDefaultsAutoAlignDrift = Invoke-FollowDefaultsRunningAutoAlignConfigDriftCheck $hwnd 1001 1002 1118 1104 1105 1108 1109 1110 1119 1120 1121 1122 120
  Write-Host "GUI follow-defaults auto-align drift baseline set: $($followDefaultsAutoAlignDrift.BaselineSet)"
  Write-Host "GUI follow-defaults auto-align drift running baseline seen: $($followDefaultsAutoAlignDrift.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults auto-align drift summary effective request seen: $($followDefaultsAutoAlignDrift.SummaryEffectiveRequestSeen)"
  Write-Host "GUI follow-defaults auto-align drift effective request seen: $($followDefaultsAutoAlignDrift.EffectiveRequestSeen)"
  Write-Host "GUI follow-defaults auto-align drift active request preserved: $($followDefaultsAutoAlignDrift.ActiveRequestPreserved)"
  Write-Host "GUI follow-defaults auto-align drift running preserved: $($followDefaultsAutoAlignDrift.RunningPreserved)"
  Write-Host "GUI follow-defaults auto-align drift checkbox restored: $($followDefaultsAutoAlignDrift.FollowDefaultsRestored)"
  Write-Host "GUI follow-defaults auto-align drift restored: $($followDefaultsAutoAlignDrift.Restored)"
  Write-Host "GUI follow-defaults auto-align drift stopped cleanly: $($followDefaultsAutoAlignDrift.StoppedCleanly)"
  Write-Host "GUI follow-defaults auto-align drift summary text: $($followDefaultsAutoAlignDrift.SummaryText)"
  Write-Host "GUI follow-defaults auto-align drift diagnostics text: $($followDefaultsAutoAlignDrift.DiagnosticsText)"
  if (-not $followDefaultsAutoAlignDrift.BaselineSet -or
      -not $followDefaultsAutoAlignDrift.RunningBaselineSeen -or
      -not $followDefaultsAutoAlignDrift.SummaryEffectiveRequestSeen -or
      -not $followDefaultsAutoAlignDrift.EffectiveRequestSeen -or
      -not $followDefaultsAutoAlignDrift.ActiveRequestPreserved -or
      -not $followDefaultsAutoAlignDrift.RunningPreserved -or
      -not $followDefaultsAutoAlignDrift.FollowDefaultsRestored -or
      -not $followDefaultsAutoAlignDrift.Restored -or
      -not $followDefaultsAutoAlignDrift.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults auto-align configuration edits did not preserve tracked active-session effective-request semantics."
  }
  $followDefaultsTimingWhileRunning = Invoke-FollowDefaultsRunningTimingConfigDriftCheck $hwnd 1001 1002 1118 1104 1105 1106 1115 1116 120
  Write-Host "GUI follow-defaults timing while-running baseline set: $($followDefaultsTimingWhileRunning.BaselineSet)"
  Write-Host "GUI follow-defaults timing while-running baseline seen: $($followDefaultsTimingWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults timing while-running summary seen: $($followDefaultsTimingWhileRunning.SummarySeen)"
  Write-Host "GUI follow-defaults timing while-running diagnostics seen: $($followDefaultsTimingWhileRunning.DiagnosticsSeen)"
  Write-Host "GUI follow-defaults timing while-running running preserved: $($followDefaultsTimingWhileRunning.RunningPreserved)"
  Write-Host "GUI follow-defaults timing while-running checkbox restored: $($followDefaultsTimingWhileRunning.FollowDefaultsRestored)"
  Write-Host "GUI follow-defaults timing while-running restored: $($followDefaultsTimingWhileRunning.Restored)"
  Write-Host "GUI follow-defaults timing while-running stopped cleanly: $($followDefaultsTimingWhileRunning.StoppedCleanly)"
  Write-Host "GUI follow-defaults timing while-running summary text: $($followDefaultsTimingWhileRunning.SummaryText)"
  Write-Host "GUI follow-defaults timing while-running diagnostics text: $($followDefaultsTimingWhileRunning.DiagnosticsText)"
  if (-not $followDefaultsTimingWhileRunning.BaselineSet -or
      -not $followDefaultsTimingWhileRunning.RunningBaselineSeen -or
      -not $followDefaultsTimingWhileRunning.SummarySeen -or
      -not $followDefaultsTimingWhileRunning.DiagnosticsSeen -or
      -not $followDefaultsTimingWhileRunning.RunningPreserved -or
      -not $followDefaultsTimingWhileRunning.FollowDefaultsRestored -or
      -not $followDefaultsTimingWhileRunning.Restored -or
      -not $followDefaultsTimingWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults timing configuration edits did not preserve tracked active-session timing semantics."
  }
  $followDefaultsWasapiModeWhileRunning = Invoke-FollowDefaultsRunningWasapiModeConfigDriftCheck $hwnd 1001 1002 1118 1104 1105 1111 1112 1123 1124 120
  Write-Host "GUI follow-defaults WASAPI-mode while-running baseline set: $($followDefaultsWasapiModeWhileRunning.BaselineSet)"
  Write-Host "GUI follow-defaults WASAPI-mode while-running baseline seen: $($followDefaultsWasapiModeWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults WASAPI-mode while-running summary seen: $($followDefaultsWasapiModeWhileRunning.SummarySeen)"
  Write-Host "GUI follow-defaults WASAPI-mode while-running diagnostics seen: $($followDefaultsWasapiModeWhileRunning.DiagnosticsSeen)"
  Write-Host "GUI follow-defaults WASAPI-mode while-running running preserved: $($followDefaultsWasapiModeWhileRunning.RunningPreserved)"
  Write-Host "GUI follow-defaults WASAPI-mode while-running checkbox restored: $($followDefaultsWasapiModeWhileRunning.FollowDefaultsRestored)"
  Write-Host "GUI follow-defaults WASAPI-mode while-running restored: $($followDefaultsWasapiModeWhileRunning.Restored)"
  Write-Host "GUI follow-defaults WASAPI-mode while-running stopped cleanly: $($followDefaultsWasapiModeWhileRunning.StoppedCleanly)"
  Write-Host "GUI follow-defaults WASAPI-mode while-running summary text: $($followDefaultsWasapiModeWhileRunning.SummaryText)"
  Write-Host "GUI follow-defaults WASAPI-mode while-running diagnostics text: $($followDefaultsWasapiModeWhileRunning.DiagnosticsText)"
  if (-not $followDefaultsWasapiModeWhileRunning.BaselineSet -or
      -not $followDefaultsWasapiModeWhileRunning.RunningBaselineSeen -or
      -not $followDefaultsWasapiModeWhileRunning.SummarySeen -or
      -not $followDefaultsWasapiModeWhileRunning.DiagnosticsSeen -or
      -not $followDefaultsWasapiModeWhileRunning.RunningPreserved -or
      -not $followDefaultsWasapiModeWhileRunning.FollowDefaultsRestored -or
      -not $followDefaultsWasapiModeWhileRunning.Restored -or
      -not $followDefaultsWasapiModeWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults WASAPI mode configuration edits did not preserve tracked active-session mode semantics."
  }
  $followDefaultsCaptureDrift = Invoke-FollowDefaultsRunningCaptureConfigDriftCheck $hwnd 1001 1002 1118 1104 1105 1108 1122 120
  Write-Host "GUI follow-defaults capture drift baseline set: $($followDefaultsCaptureDrift.BaselineSet)"
  Write-Host "GUI follow-defaults capture drift auto-align normalized: $($followDefaultsCaptureDrift.NormalizedAutoAlign)"
  Write-Host "GUI follow-defaults capture drift running baseline seen: $($followDefaultsCaptureDrift.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults capture drift current configured changed: $($followDefaultsCaptureDrift.DriftSeen)"
  Write-Host "GUI follow-defaults capture drift active requested preserved: $($followDefaultsCaptureDrift.RequestedPreserved)"
  Write-Host "GUI follow-defaults capture drift running preserved: $($followDefaultsCaptureDrift.RunningPreserved)"
  Write-Host "GUI follow-defaults capture drift checkbox restored: $($followDefaultsCaptureDrift.FollowDefaultsRestored)"
  Write-Host "GUI follow-defaults capture drift restored: $($followDefaultsCaptureDrift.Restored)"
  Write-Host "GUI follow-defaults capture drift auto-align restored: $($followDefaultsCaptureDrift.AutoAlignRestored)"
  Write-Host "GUI follow-defaults capture drift stopped cleanly: $($followDefaultsCaptureDrift.StoppedCleanly)"
  Write-Host "GUI follow-defaults capture drift diagnostics text: $($followDefaultsCaptureDrift.DiagnosticsText)"
  if (-not $followDefaultsCaptureDrift.BaselineSet -or
      -not $followDefaultsCaptureDrift.NormalizedAutoAlign -or
      -not $followDefaultsCaptureDrift.RunningBaselineSeen -or
      -not $followDefaultsCaptureDrift.DriftSeen -or
      -not $followDefaultsCaptureDrift.RequestedPreserved -or
      -not $followDefaultsCaptureDrift.RunningPreserved -or
      -not $followDefaultsCaptureDrift.FollowDefaultsRestored -or
      -not $followDefaultsCaptureDrift.Restored -or
      -not $followDefaultsCaptureDrift.AutoAlignRestored -or
      -not $followDefaultsCaptureDrift.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults capture configuration edits did not preserve tracked active-session request semantics."
  }
  $followDefaultsRenderDrift = Invoke-FollowDefaultsRunningRenderConfigDriftCheck $hwnd 1001 1002 1118 1104 1105 1119 1122 120
  Write-Host "GUI follow-defaults render drift baseline set: $($followDefaultsRenderDrift.BaselineSet)"
  Write-Host "GUI follow-defaults render drift auto-align normalized: $($followDefaultsRenderDrift.NormalizedAutoAlign)"
  Write-Host "GUI follow-defaults render drift running baseline seen: $($followDefaultsRenderDrift.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults render drift current configured changed: $($followDefaultsRenderDrift.DriftSeen)"
  Write-Host "GUI follow-defaults render drift active requested preserved: $($followDefaultsRenderDrift.RequestedPreserved)"
  Write-Host "GUI follow-defaults render drift running preserved: $($followDefaultsRenderDrift.RunningPreserved)"
  Write-Host "GUI follow-defaults render drift checkbox restored: $($followDefaultsRenderDrift.FollowDefaultsRestored)"
  Write-Host "GUI follow-defaults render drift restored: $($followDefaultsRenderDrift.Restored)"
  Write-Host "GUI follow-defaults render drift auto-align restored: $($followDefaultsRenderDrift.AutoAlignRestored)"
  Write-Host "GUI follow-defaults render drift stopped cleanly: $($followDefaultsRenderDrift.StoppedCleanly)"
  Write-Host "GUI follow-defaults render drift diagnostics text: $($followDefaultsRenderDrift.DiagnosticsText)"
  if (-not $followDefaultsRenderDrift.BaselineSet -or
      -not $followDefaultsRenderDrift.NormalizedAutoAlign -or
      -not $followDefaultsRenderDrift.RunningBaselineSeen -or
      -not $followDefaultsRenderDrift.DriftSeen -or
      -not $followDefaultsRenderDrift.RequestedPreserved -or
      -not $followDefaultsRenderDrift.RunningPreserved -or
      -not $followDefaultsRenderDrift.FollowDefaultsRestored -or
      -not $followDefaultsRenderDrift.Restored -or
      -not $followDefaultsRenderDrift.AutoAlignRestored -or
      -not $followDefaultsRenderDrift.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults render configuration edits did not preserve tracked active-session request semantics."
  }
  $followDefaultsDumpWhileRunning = Invoke-FollowDefaultsRunningDumpConfigDriftCheck $hwnd 1001 1002 1118 1104 1105 1107 1113 1114 120
  Write-Host "GUI follow-defaults dump while-running baseline set: $($followDefaultsDumpWhileRunning.BaselineSet)"
  Write-Host "GUI follow-defaults dump while-running baseline seen: $($followDefaultsDumpWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults dump while-running summary seen: $($followDefaultsDumpWhileRunning.SummarySeen)"
  Write-Host "GUI follow-defaults dump while-running diagnostics seen: $($followDefaultsDumpWhileRunning.DiagnosticsSeen)"
  Write-Host "GUI follow-defaults dump while-running running preserved: $($followDefaultsDumpWhileRunning.RunningPreserved)"
  Write-Host "GUI follow-defaults dump while-running checkbox restored: $($followDefaultsDumpWhileRunning.FollowDefaultsRestored)"
  Write-Host "GUI follow-defaults dump while-running restored: $($followDefaultsDumpWhileRunning.Restored)"
  Write-Host "GUI follow-defaults dump while-running stopped cleanly: $($followDefaultsDumpWhileRunning.StoppedCleanly)"
  Write-Host "GUI follow-defaults dump while-running summary text: $($followDefaultsDumpWhileRunning.SummaryText)"
  Write-Host "GUI follow-defaults dump while-running diagnostics text: $($followDefaultsDumpWhileRunning.DiagnosticsText)"
  if (-not $followDefaultsDumpWhileRunning.BaselineSet -or
      -not $followDefaultsDumpWhileRunning.RunningBaselineSeen -or
      -not $followDefaultsDumpWhileRunning.SummarySeen -or
      -not $followDefaultsDumpWhileRunning.DiagnosticsSeen -or
      -not $followDefaultsDumpWhileRunning.RunningPreserved -or
      -not $followDefaultsDumpWhileRunning.FollowDefaultsRestored -or
      -not $followDefaultsDumpWhileRunning.Restored -or
      -not $followDefaultsDumpWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults dump configuration edits did not preserve tracked active-session dump semantics."
  }
  $followDefaultsSemantics = Invoke-FollowDefaultsLoopbackSemanticsCheck $hwnd 1103 1118 120
  Write-Host "GUI follow-defaults loopback summary seen: $($followDefaultsSemantics.SummarySeen)"
  Write-Host "GUI follow-defaults loopback diagnostics seen: $($followDefaultsSemantics.DiagnosticsSeen)"
  Write-Host "GUI follow-defaults loopback summary restored: $($followDefaultsSemantics.SummaryRestored)"
  Write-Host "GUI follow-defaults loopback diagnostics restored: $($followDefaultsSemantics.DiagnosticsRestored)"
  if (-not $followDefaultsSemantics.SummarySeen -or
      -not $followDefaultsSemantics.DiagnosticsSeen -or
      -not $followDefaultsSemantics.SummaryRestored -or
      -not $followDefaultsSemantics.DiagnosticsRestored) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults loopback semantics did not update summary/diagnostics correctly."
  }
  $monitor = Invoke-MonitorToggleCheck $hwnd 1117 @(1102, 1105, 1106, 1119, 1120, 1121, 1123, 1124, 1116, 1122) 60
  Write-Host "GUI monitor disabled render controls seen: $($monitor.DisabledSeen)"
  Write-Host "GUI monitor restored: $($monitor.Restored)"
  if (-not $monitor.DisabledSeen -or -not $monitor.Restored) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: monitor toggle did not update render controls correctly."
  }
  $followDefaultsMonitorComposition = Invoke-FollowDefaultsMonitorCompositionCheck $hwnd 1118 1117 1104 1105 60
  Write-Host "GUI follow-defaults/monitor normalized baseline ok: $($followDefaultsMonitorComposition.NormalizedOk)"
  Write-Host "GUI follow-defaults/monitor monitor-off seen: $($followDefaultsMonitorComposition.MonitorOffSeen)"
  Write-Host "GUI follow-defaults/monitor combined disabled seen: $($followDefaultsMonitorComposition.CombinedDisabledSeen)"
  Write-Host "GUI follow-defaults/monitor follow released seen: $($followDefaultsMonitorComposition.FollowReleasedSeen)"
  Write-Host "GUI follow-defaults/monitor normalized restored: $($followDefaultsMonitorComposition.RestoredNormalized)"
  Write-Host "GUI follow-defaults/monitor baseline restored: $($followDefaultsMonitorComposition.RestoredBaseline)"
  if (-not $followDefaultsMonitorComposition.NormalizedOk -or
      -not $followDefaultsMonitorComposition.MonitorOffSeen -or
      -not $followDefaultsMonitorComposition.CombinedDisabledSeen -or
      -not $followDefaultsMonitorComposition.FollowReleasedSeen -or
      -not $followDefaultsMonitorComposition.RestoredNormalized -or
      -not $followDefaultsMonitorComposition.RestoredBaseline) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults and monitor composition state did not behave correctly."
  }
  $monitorSemantics = Invoke-MonitorDisabledSemanticsCheck $hwnd 1117 120
  Write-Host "GUI monitor-off summary seen: $($monitorSemantics.SummarySeen)"
  Write-Host "GUI monitor-off diagnostics seen: $($monitorSemantics.DiagnosticsSeen)"
  Write-Host "GUI monitor-off summary restored: $($monitorSemantics.SummaryRestored)"
  Write-Host "GUI monitor-off diagnostics restored: $($monitorSemantics.DiagnosticsRestored)"
  if (-not $monitorSemantics.SummarySeen -or
      -not $monitorSemantics.DiagnosticsSeen -or
      -not $monitorSemantics.SummaryRestored -or
      -not $monitorSemantics.DiagnosticsRestored) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: monitor-off semantics did not update summary/diagnostics correctly."
  }
  $monitorWhileRunning = Invoke-MonitorOffWhileRunningConfigDriftCheck $hwnd 1001 1002 1117 1122 1102 1105 1119 1120 1121 1123 1124 1116 1106 120
  Write-Host "GUI monitor-off while-running auto-align normalized: $($monitorWhileRunning.NormalizedAutoAlign)"
  Write-Host "GUI monitor-off while-running baseline seen: $($monitorWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI monitor-off while-running summary seen: $($monitorWhileRunning.SummarySeen)"
  Write-Host "GUI monitor-off while-running diagnostics seen: $($monitorWhileRunning.DiagnosticsSeen)"
  Write-Host "GUI monitor-off while-running render surface disabled seen: $($monitorWhileRunning.RenderSurfaceDisabledSeen)"
  Write-Host "GUI monitor-off while-running running preserved: $($monitorWhileRunning.RunningPreserved)"
  Write-Host "GUI monitor-off while-running checkbox restored: $($monitorWhileRunning.MonitorRestored)"
  Write-Host "GUI monitor-off while-running auto-align restored: $($monitorWhileRunning.AutoAlignRestored)"
  Write-Host "GUI monitor-off while-running stopped cleanly: $($monitorWhileRunning.StoppedCleanly)"
  Write-Host "GUI monitor-off while-running summary text: $($monitorWhileRunning.SummaryText)"
  Write-Host "GUI monitor-off while-running diagnostics text: $($monitorWhileRunning.DiagnosticsText)"
  if (-not $monitorWhileRunning.NormalizedAutoAlign -or
      -not $monitorWhileRunning.RunningBaselineSeen -or
      -not $monitorWhileRunning.SummarySeen -or
      -not $monitorWhileRunning.DiagnosticsSeen -or
      -not $monitorWhileRunning.RenderSurfaceDisabledSeen -or
      -not $monitorWhileRunning.RunningPreserved -or
      -not $monitorWhileRunning.MonitorRestored -or
      -not $monitorWhileRunning.AutoAlignRestored -or
      -not $monitorWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: monitor-off while-running config drift semantics did not behave correctly."
  }
  $sessionButtons = Invoke-SessionButtonCheck $hwnd 1001 1002 80
  Write-Host "GUI session buttons idle baseline ok: $($sessionButtons.IdleBaselineOk)"
  Write-Host "GUI session buttons idle title ok: $($sessionButtons.IdleTitleOk)"
  Write-Host "GUI session buttons baseline title: $($sessionButtons.BaselineTitle)"
  Write-Host "GUI session buttons running state seen: $($sessionButtons.RunningStateSeen)"
  Write-Host "GUI session buttons running title seen: $($sessionButtons.RunningTitleSeen)"
  Write-Host "GUI session buttons title restored: $($sessionButtons.TitleRestored)"
  Write-Host "GUI session buttons restored: $($sessionButtons.Restored)"
  if (-not $sessionButtons.IdleBaselineOk -or
      -not $sessionButtons.IdleTitleOk -or
      -not $sessionButtons.RunningStateSeen -or
      -not $sessionButtons.RunningTitleSeen -or
      -not $sessionButtons.TitleRestored -or
      -not $sessionButtons.Restored) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: start/stop availability did not track session state correctly."
  }
  $refreshWhileRunning = Invoke-RefreshWhileRunningCheck $hwnd 1003 1001 1002 120
  Write-Host "GUI refresh-while-running baseline seen: $($refreshWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI refresh-while-running preserved: $($refreshWhileRunning.RunningPreserved)"
  Write-Host "GUI refresh-while-running title preserved: $($refreshWhileRunning.TitlePreserved)"
  Write-Host "GUI refresh-while-running note seen: $($refreshWhileRunning.RunningNoteSeen)"
  Write-Host "GUI refresh-while-running button stayed enabled: $($refreshWhileRunning.RefreshStayedEnabled)"
  Write-Host "GUI refresh-while-running stopped cleanly: $($refreshWhileRunning.StoppedCleanly)"
  if (-not $refreshWhileRunning.RunningBaselineSeen -or
      -not $refreshWhileRunning.RunningPreserved -or
      -not $refreshWhileRunning.TitlePreserved -or
      -not $refreshWhileRunning.RunningNoteSeen -or
      -not $refreshWhileRunning.RefreshStayedEnabled -or
      -not $refreshWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: refresh while running did not preserve the active session correctly."
  }
  $runningCaptureDrift = Invoke-RunningCaptureConfigDriftCheck $hwnd 1001 1002 1108 1122 120
  Write-Host "GUI running capture drift baseline set: $($runningCaptureDrift.BaselineSet)"
  Write-Host "GUI running capture drift auto-align normalized: $($runningCaptureDrift.NormalizedAutoAlign)"
  Write-Host "GUI running capture drift running baseline seen: $($runningCaptureDrift.RunningBaselineSeen)"
  Write-Host "GUI running capture drift current configured changed: $($runningCaptureDrift.DriftSeen)"
  Write-Host "GUI running capture drift active requested preserved: $($runningCaptureDrift.RequestedPreserved)"
  Write-Host "GUI running capture drift running preserved: $($runningCaptureDrift.RunningPreserved)"
  Write-Host "GUI running capture drift restored: $($runningCaptureDrift.Restored)"
  Write-Host "GUI running capture drift auto-align restored: $($runningCaptureDrift.AutoAlignRestored)"
  Write-Host "GUI running capture drift stopped cleanly: $($runningCaptureDrift.StoppedCleanly)"
  Write-Host "GUI running capture drift diagnostics text: $($runningCaptureDrift.DiagnosticsText)"
  if (-not $runningCaptureDrift.BaselineSet -or
      -not $runningCaptureDrift.NormalizedAutoAlign -or
      -not $runningCaptureDrift.RunningBaselineSeen -or
      -not $runningCaptureDrift.DriftSeen -or
      -not $runningCaptureDrift.RequestedPreserved -or
      -not $runningCaptureDrift.RunningPreserved -or
      -not $runningCaptureDrift.Restored -or
      -not $runningCaptureDrift.AutoAlignRestored -or
      -not $runningCaptureDrift.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: running capture configuration edits did not preserve active-session diagnostics semantics."
  }
  $runningRenderDrift = Invoke-RunningRenderConfigDriftCheck $hwnd 1001 1002 1119 1122 120
  Write-Host "GUI running render drift baseline set: $($runningRenderDrift.BaselineSet)"
  Write-Host "GUI running render drift auto-align normalized: $($runningRenderDrift.NormalizedAutoAlign)"
  Write-Host "GUI running render drift running baseline seen: $($runningRenderDrift.RunningBaselineSeen)"
  Write-Host "GUI running render drift current configured changed: $($runningRenderDrift.DriftSeen)"
  Write-Host "GUI running render drift active requested preserved: $($runningRenderDrift.RequestedPreserved)"
  Write-Host "GUI running render drift running preserved: $($runningRenderDrift.RunningPreserved)"
  Write-Host "GUI running render drift restored: $($runningRenderDrift.Restored)"
  Write-Host "GUI running render drift auto-align restored: $($runningRenderDrift.AutoAlignRestored)"
  Write-Host "GUI running render drift stopped cleanly: $($runningRenderDrift.StoppedCleanly)"
  Write-Host "GUI running render drift diagnostics text: $($runningRenderDrift.DiagnosticsText)"
  if (-not $runningRenderDrift.BaselineSet -or
      -not $runningRenderDrift.NormalizedAutoAlign -or
      -not $runningRenderDrift.RunningBaselineSeen -or
      -not $runningRenderDrift.DriftSeen -or
      -not $runningRenderDrift.RequestedPreserved -or
      -not $runningRenderDrift.RunningPreserved -or
      -not $runningRenderDrift.Restored -or
      -not $runningRenderDrift.AutoAlignRestored -or
      -not $runningRenderDrift.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: running render configuration edits did not preserve active-session diagnostics semantics."
  }
  $runningDumpDrift = Invoke-RunningDumpConfigDriftCheck $hwnd 1001 1002 1107 1113 1114 120
  Write-Host "GUI running dump drift baseline set: $($runningDumpDrift.BaselineSet)"
  Write-Host "GUI running dump drift running baseline seen: $($runningDumpDrift.RunningBaselineSeen)"
  Write-Host "GUI running dump drift summary seen: $($runningDumpDrift.SummarySeen)"
  Write-Host "GUI running dump drift diagnostics seen: $($runningDumpDrift.DiagnosticsSeen)"
  Write-Host "GUI running dump drift running preserved: $($runningDumpDrift.RunningPreserved)"
  Write-Host "GUI running dump drift restored: $($runningDumpDrift.Restored)"
  Write-Host "GUI running dump drift stopped cleanly: $($runningDumpDrift.StoppedCleanly)"
  Write-Host "GUI running dump drift summary text: $($runningDumpDrift.SummaryText)"
  Write-Host "GUI running dump drift diagnostics text: $($runningDumpDrift.DiagnosticsText)"
  if (-not $runningDumpDrift.BaselineSet -or
      -not $runningDumpDrift.RunningBaselineSeen -or
      -not $runningDumpDrift.SummarySeen -or
      -not $runningDumpDrift.DiagnosticsSeen -or
      -not $runningDumpDrift.RunningPreserved -or
      -not $runningDumpDrift.Restored -or
      -not $runningDumpDrift.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: running dump configuration edits did not preserve active-session dump semantics."
  }
  $runningTimingDrift = Invoke-RunningTimingConfigDriftCheck $hwnd 1001 1002 1106 1115 1116 120
  Write-Host "GUI running timing drift baseline set: $($runningTimingDrift.BaselineSet)"
  Write-Host "GUI running timing drift running baseline seen: $($runningTimingDrift.RunningBaselineSeen)"
  Write-Host "GUI running timing drift summary seen: $($runningTimingDrift.SummarySeen)"
  Write-Host "GUI running timing drift diagnostics seen: $($runningTimingDrift.DiagnosticsSeen)"
  Write-Host "GUI running timing drift running preserved: $($runningTimingDrift.RunningPreserved)"
  Write-Host "GUI running timing drift restored: $($runningTimingDrift.Restored)"
  Write-Host "GUI running timing drift stopped cleanly: $($runningTimingDrift.StoppedCleanly)"
  Write-Host "GUI running timing drift summary text: $($runningTimingDrift.SummaryText)"
  Write-Host "GUI running timing drift diagnostics text: $($runningTimingDrift.DiagnosticsText)"
  if (-not $runningTimingDrift.BaselineSet -or
      -not $runningTimingDrift.RunningBaselineSeen -or
      -not $runningTimingDrift.SummarySeen -or
      -not $runningTimingDrift.DiagnosticsSeen -or
      -not $runningTimingDrift.RunningPreserved -or
      -not $runningTimingDrift.Restored -or
      -not $runningTimingDrift.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: running timing/buffer configuration edits did not preserve active-session timing semantics."
  }
  $runningWasapiModeDrift = Invoke-RunningWasapiModeConfigDriftCheck $hwnd 1001 1002 1111 1112 1123 1124 120
  Write-Host "GUI running WASAPI-mode drift baseline set: $($runningWasapiModeDrift.BaselineSet)"
  Write-Host "GUI running WASAPI-mode drift running baseline seen: $($runningWasapiModeDrift.RunningBaselineSeen)"
  Write-Host "GUI running WASAPI-mode drift summary seen: $($runningWasapiModeDrift.SummarySeen)"
  Write-Host "GUI running WASAPI-mode drift diagnostics seen: $($runningWasapiModeDrift.DiagnosticsSeen)"
  Write-Host "GUI running WASAPI-mode drift running preserved: $($runningWasapiModeDrift.RunningPreserved)"
  Write-Host "GUI running WASAPI-mode drift restored: $($runningWasapiModeDrift.Restored)"
  Write-Host "GUI running WASAPI-mode drift stopped cleanly: $($runningWasapiModeDrift.StoppedCleanly)"
  Write-Host "GUI running WASAPI-mode drift summary text: $($runningWasapiModeDrift.SummaryText)"
  Write-Host "GUI running WASAPI-mode drift diagnostics text: $($runningWasapiModeDrift.DiagnosticsText)"
  if (-not $runningWasapiModeDrift.BaselineSet -or
      -not $runningWasapiModeDrift.RunningBaselineSeen -or
      -not $runningWasapiModeDrift.SummarySeen -or
      -not $runningWasapiModeDrift.DiagnosticsSeen -or
      -not $runningWasapiModeDrift.RunningPreserved -or
      -not $runningWasapiModeDrift.Restored -or
      -not $runningWasapiModeDrift.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: running WASAPI share/drive configuration edits did not preserve active-session mode semantics."
  }
  $runningAutoAlignDrift = Invoke-RunningAutoAlignConfigDriftCheck $hwnd 1001 1002 1108 1109 1110 1119 1120 1121 1122 120
  Write-Host "GUI running auto-align drift baseline set: $($runningAutoAlignDrift.BaselineSet)"
  Write-Host "GUI running auto-align drift running baseline seen: $($runningAutoAlignDrift.RunningBaselineSeen)"
  Write-Host "GUI running auto-align drift summary effective request seen: $($runningAutoAlignDrift.SummaryEffectiveRequestSeen)"
  Write-Host "GUI running auto-align drift effective request seen: $($runningAutoAlignDrift.EffectiveRequestSeen)"
  Write-Host "GUI running auto-align drift active request preserved: $($runningAutoAlignDrift.ActiveRequestPreserved)"
  Write-Host "GUI running auto-align drift running preserved: $($runningAutoAlignDrift.RunningPreserved)"
  Write-Host "GUI running auto-align drift restored: $($runningAutoAlignDrift.Restored)"
  Write-Host "GUI running auto-align drift stopped cleanly: $($runningAutoAlignDrift.StoppedCleanly)"
  Write-Host "GUI running auto-align drift summary text: $($runningAutoAlignDrift.SummaryText)"
  Write-Host "GUI running auto-align drift diagnostics text: $($runningAutoAlignDrift.DiagnosticsText)"
  if (-not $runningAutoAlignDrift.BaselineSet -or
      -not $runningAutoAlignDrift.RunningBaselineSeen -or
      -not $runningAutoAlignDrift.SummaryEffectiveRequestSeen -or
      -not $runningAutoAlignDrift.EffectiveRequestSeen -or
      -not $runningAutoAlignDrift.ActiveRequestPreserved -or
      -not $runningAutoAlignDrift.RunningPreserved -or
      -not $runningAutoAlignDrift.Restored -or
      -not $runningAutoAlignDrift.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: running auto-align configuration edits did not preserve effective-request diagnostics semantics."
  }
  $manualDeviceNoRebuild = Invoke-ManualDeviceNoRebuildCheck $hwnd 1001 1002 1118 160
  Write-Host "GUI manual-device default-device running seen: $($manualDeviceNoRebuild.RunningSeen)"
  Write-Host "GUI manual-device default-device no-rebuild seen: $($manualDeviceNoRebuild.NoRebuildSeen)"
  Write-Host "GUI manual-device default-device summary seen: $($manualDeviceNoRebuild.SummarySeen)"
  Write-Host "GUI manual-device default-device running preserved: $($manualDeviceNoRebuild.RunningPreserved)"
  Write-Host "GUI manual-device default-device stopped cleanly: $($manualDeviceNoRebuild.StoppedCleanly)"
  Write-Host "GUI manual-device default-device checkbox restored: $($manualDeviceNoRebuild.FollowDefaultsRestored)"
  Write-Host "GUI manual-device default-device summary text: $($manualDeviceNoRebuild.SummaryText)"
  Write-Host "GUI manual-device default-device diagnostics text: $($manualDeviceNoRebuild.DiagnosticsText)"
  $manualDeviceDiagnosticsLabelsSeen = Test-GuiActiveConfigurationDiagnosticsText $manualDeviceNoRebuild.DiagnosticsText
  $manualDeviceRequestedIdsExplicit =
      $manualDeviceNoRebuild.DiagnosticsText.IndexOf("Active session requested capture id: default", [System.StringComparison]::Ordinal) -lt 0 -and
      $manualDeviceNoRebuild.DiagnosticsText.IndexOf("Active session requested render id: default", [System.StringComparison]::Ordinal) -lt 0
  Write-Host "GUI manual-device default-device diagnostics labels seen: $manualDeviceDiagnosticsLabelsSeen"
  Write-Host "GUI manual-device default-device requested ids explicit: $manualDeviceRequestedIdsExplicit"
  if (-not $manualDeviceNoRebuild.RunningSeen -or
      -not $manualDeviceNoRebuild.NoRebuildSeen -or
      -not $manualDeviceNoRebuild.SummarySeen -or
      -not $manualDeviceNoRebuild.RunningPreserved -or
      -not $manualDeviceNoRebuild.StoppedCleanly -or
      -not $manualDeviceNoRebuild.FollowDefaultsRestored -or
      -not $manualDeviceDiagnosticsLabelsSeen -or
      -not $manualDeviceRequestedIdsExplicit) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: manual-device default-device-change semantics did not behave correctly."
  }
  $followDefaultsRefreshResync = Invoke-FollowDefaultsRefreshResyncCheck $hwnd 1003 1001 1002 1118 1104 1105 120
  Write-Host "GUI follow-defaults refresh running baseline seen: $($followDefaultsRefreshResync.RunningBaselineSeen)"
  Write-Host "GUI follow-defaults refresh tracking preserved: $($followDefaultsRefreshResync.TrackingPreserved)"
  Write-Host "GUI follow-defaults refresh rebuild seen: $($followDefaultsRefreshResync.RefreshRebuildSeen)"
  Write-Host "GUI follow-defaults refresh summary seen: $($followDefaultsRefreshResync.SummarySeen)"
  Write-Host "GUI follow-defaults refresh running restored: $($followDefaultsRefreshResync.RunningRestored)"
  Write-Host "GUI follow-defaults refresh capture disabled: $($followDefaultsRefreshResync.CaptureStillDisabled)"
  Write-Host "GUI follow-defaults refresh render disabled: $($followDefaultsRefreshResync.RenderStillDisabled)"
  Write-Host "GUI follow-defaults refresh stopped cleanly: $($followDefaultsRefreshResync.StoppedCleanly)"
  Write-Host "GUI follow-defaults refresh checkbox restored: $($followDefaultsRefreshResync.FollowDefaultsRestored)"
  Write-Host "GUI follow-defaults refresh summary text: $($followDefaultsRefreshResync.SummaryText)"
  Write-Host "GUI follow-defaults refresh diagnostics text: $($followDefaultsRefreshResync.DiagnosticsText)"
  $followDefaultsRefreshDiagnosticsLabelsSeen = Test-GuiActiveConfigurationDiagnosticsText $followDefaultsRefreshResync.DiagnosticsText
  $followDefaultsRefreshRequestedIdsDefault =
      $followDefaultsRefreshResync.DiagnosticsText.IndexOf("Active session requested capture id: default", [System.StringComparison]::Ordinal) -ge 0 -and
      $followDefaultsRefreshResync.DiagnosticsText.IndexOf("Active session requested render id: default", [System.StringComparison]::Ordinal) -ge 0
  Write-Host "GUI follow-defaults refresh diagnostics labels seen: $followDefaultsRefreshDiagnosticsLabelsSeen"
  Write-Host "GUI follow-defaults refresh requested ids default: $followDefaultsRefreshRequestedIdsDefault"
  if (-not $followDefaultsRefreshResync.RunningBaselineSeen -or
      -not $followDefaultsRefreshResync.TrackingPreserved -or
      -not $followDefaultsRefreshResync.RefreshRebuildSeen -or
      -not $followDefaultsRefreshResync.SummarySeen -or
      -not $followDefaultsRefreshResync.RunningRestored -or
      -not $followDefaultsRefreshResync.CaptureStillDisabled -or
      -not $followDefaultsRefreshResync.RenderStillDisabled -or
      -not $followDefaultsRefreshResync.StoppedCleanly -or
      -not $followDefaultsRefreshResync.FollowDefaultsRestored -or
      -not $followDefaultsRefreshDiagnosticsLabelsSeen -or
      -not $followDefaultsRefreshRequestedIdsDefault) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults refresh did not preserve tracked-device semantics correctly."
  }
  $followDefaultsRebuild = Invoke-FollowDefaultsDefaultDeviceRebuildCheck $hwnd 1001 1002 1118 160
  Write-Host "GUI follow-defaults default-device running seen: $($followDefaultsRebuild.RunningSeen)"
  Write-Host "GUI follow-defaults default-device rebuild seen: $($followDefaultsRebuild.RebuildSeen)"
  Write-Host "GUI follow-defaults default-device running restored: $($followDefaultsRebuild.RunningRestored)"
  Write-Host "GUI follow-defaults default-device stopped cleanly: $($followDefaultsRebuild.StoppedCleanly)"
  Write-Host "GUI follow-defaults default-device checkbox restored: $($followDefaultsRebuild.FollowDefaultsRestored)"
  if (-not $followDefaultsRebuild.RunningSeen -or
      -not $followDefaultsRebuild.RebuildSeen -or
      -not $followDefaultsRebuild.RunningRestored -or
      -not $followDefaultsRebuild.StoppedCleanly -or
      -not $followDefaultsRebuild.FollowDefaultsRestored) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: follow-defaults default-device-change rebuild semantics did not behave correctly."
  }
  $quickBusySurface = Invoke-NormalizeBusyProbeSurface $hwnd 1118 1117 1122 1104 1105 @(1119, 1120, 1121) 60
  Write-Host "GUI quick busy surface normalized: $($quickBusySurface.NormalizedOk)"
  if (-not $quickBusySurface.NormalizedOk) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: quick busy-cycle surface could not be normalized."
  }
  $quick = Invoke-ProbeUiCheck $hwnd 1004 1005 1001 1002 "Quick Probe Running" "Quick Probe Running..." "Run Quick Probe" $watchedControls 240
  $quickBusySurfaceRestored = Invoke-RestoreBusyProbeSurface $hwnd $quickBusySurface.Baseline 1118 1117 1122 60
  Write-Host "GUI quick title busy seen: $($quick.TitleBusySeen)"
  Write-Host "GUI quick text busy seen: $($quick.ProbeTextBusySeen)"
  Write-Host "GUI quick disabled seen: $($quick.ProbeDisabledSeen)"
  Write-Host "GUI quick paired probe disabled seen: $($quick.PairedProbeDisabledSeen)"
  Write-Host "GUI quick start disabled seen: $($quick.StartDisabledSeen)"
  Write-Host "GUI quick stop stayed disabled: $($quick.StopStayedDisabled)"
  Write-Host "GUI quick busy surface restored: $quickBusySurfaceRestored"
  Write-Host "GUI quick watched availability: $(if($quick.AvailabilityIssues.Count -eq 0){'OK'}else{$quick.AvailabilityIssues -join ', '})"
  Write-Host "GUI quick watched disabled seen: $(Format-StateMap $quick.WatchedDisabledSeen)"
  Write-Host "GUI quick watched restored: $(Format-StateMap $quick.WatchedRestored)"
  Write-Host "GUI quick watched baseline enabled: $(Format-StateMap $quick.BaselineEnabled)"
  Write-Host "GUI quick watched final enabled: $(Format-StateMap $quick.FinalEnabled)"
  Write-Host "GUI quick watched baseline values: $(Format-SnapshotMap $quick.BaselineValue)"
  Write-Host "GUI quick watched final values: $(Format-SnapshotMap $quick.FinalValue)"
  Write-Host "GUI quick baseline restored: $($quick.BaselineRestored)"
  Write-Host "GUI quick restored: $($quick.Restored)"
  if ($quick.AvailabilityIssues.Count -ne 0 -or
      -not $quick.TitleBusySeen -or
      -not $quick.ProbeTextBusySeen -or
      -not $quick.ProbeDisabledSeen -or
      -not $quick.PairedProbeDisabledSeen -or
      -not $quick.StartDisabledSeen -or
      -not $quick.StopStayedDisabled -or
      -not $quickBusySurfaceRestored -or
      -not (Test-AllStatesTrue $quick.WatchedDisabledSeen) -or
      -not (Test-AllStatesTrue $quick.WatchedRestored) -or
      -not $quick.BaselineRestored -or
      -not $quick.Restored) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: quick probe busy cycle incomplete."
  }
  $quickWhileRunning = Invoke-RunningSessionProbeCycleCheck $hwnd 1004 1005 1001 1002 "Quick Probe Running" "Quick Probe Running..." "Run Quick Probe" $watchedControls 240
  Write-Host "GUI quick while-running baseline seen: $($quickWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI quick while-running title busy seen: $($quickWhileRunning.ProbeCycle.TitleBusySeen)"
  Write-Host "GUI quick while-running text busy seen: $($quickWhileRunning.ProbeCycle.ProbeTextBusySeen)"
  Write-Host "GUI quick while-running probe disabled seen: $($quickWhileRunning.ProbeCycle.ProbeDisabledSeen)"
  Write-Host "GUI quick while-running paired probe disabled seen: $($quickWhileRunning.ProbeCycle.PairedProbeDisabledSeen)"
  Write-Host "GUI quick while-running start disabled seen: $($quickWhileRunning.ProbeCycle.StartDisabledSeen)"
  Write-Host "GUI quick while-running stop available seen: $($quickWhileRunning.StopAvailableSeen)"
  Write-Host "GUI quick while-running restored: $($quickWhileRunning.RunningRestored)"
  Write-Host "GUI quick while-running stopped cleanly: $($quickWhileRunning.StoppedCleanly)"
  if (-not $quickWhileRunning.RunningBaselineSeen -or
      -not $quickWhileRunning.ProbeCycle.TitleBusySeen -or
      -not $quickWhileRunning.ProbeCycle.ProbeTextBusySeen -or
      -not $quickWhileRunning.ProbeCycle.ProbeDisabledSeen -or
      -not $quickWhileRunning.ProbeCycle.PairedProbeDisabledSeen -or
      -not $quickWhileRunning.ProbeCycle.StartDisabledSeen -or
      -not $quickWhileRunning.StopAvailableSeen -or
      -not $quickWhileRunning.RunningRestored -or
      -not $quickWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: quick probe did not restore the running session correctly."
  }
  $quickSemantics = Invoke-QuickProbeResultSemanticsCheck $hwnd 1004 1117 180
  Write-Host "GUI quick semantics monitor-on seen: $($quickSemantics.MonitorOnSeen)"
  Write-Host "GUI quick semantics monitor-off seen: $($quickSemantics.MonitorOffSeen)"
  Write-Host "GUI quick semantics monitor restored: $($quickSemantics.Restored)"
  if (-not $quickSemantics.MonitorOnSeen -or
      -not $quickSemantics.MonitorOffSeen -or
      -not $quickSemantics.Restored) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: quick probe result semantics did not reflect monitor on/off correctly."
  }
  $quickSourceModeFailure = Invoke-QuickProbeSourceModeFailureCheck $hwnd 1101 1103 1004 120
  Write-Host "GUI quick source-mode failure seen: $($quickSourceModeFailure.FailureSeen)"
  Write-Host "GUI quick source-mode failure restored: $($quickSourceModeFailure.Restored)"
  if (-not $quickSourceModeFailure.FailureSeen -or
      -not $quickSourceModeFailure.Restored) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: quick probe source-mode failure semantics did not surface correctly."
  }
  $appLoopbackGui = Invoke-ApplicationLoopbackGuiCheck $hwnd 1103 1104 1125 1004 120
  Write-Host "GUI app-loopback surface seen: $($appLoopbackGui.SurfaceSeen)"
  Write-Host "GUI app-loopback source seen: $($appLoopbackGui.SourceSeen)"
  Write-Host "GUI app-loopback label seen: $($appLoopbackGui.LabelSeen)"
  Write-Host "GUI app-loopback device-count seen: $($appLoopbackGui.DeviceCountSeen)"
  Write-Host "GUI app-loopback target seen: $($appLoopbackGui.TargetSeen)"
  Write-Host "GUI app-loopback requested-capture-id seen: $($appLoopbackGui.RequestedCaptureIdSeen)"
  Write-Host "GUI app-loopback summary seen: $($appLoopbackGui.SummarySeen)"
  Write-Host "GUI app-loopback quick failure seen: $($appLoopbackGui.QuickFailureSeen)"
  Write-Host "GUI app-loopback restored: $($appLoopbackGui.Restored)"
  Write-Host "GUI app-loopback last source text: $($appLoopbackGui.LastSourceText)"
  Write-Host "GUI app-loopback last capture label: $($appLoopbackGui.LastCaptureLabel)"
  Write-Host "GUI app-loopback last device-count line: $($appLoopbackGui.LastDeviceCountLine)"
  Write-Host "GUI app-loopback last target text: $($appLoopbackGui.LastAppTargetText)"
  Write-Host "GUI app-loopback summary text: $($appLoopbackGui.SummaryText)"
  Write-Host "GUI app-loopback probe text: $($appLoopbackGui.ProbeText)"
  if (-not $appLoopbackGui.SurfaceSeen -or
      -not $appLoopbackGui.RequestedCaptureIdSeen -or
      -not $appLoopbackGui.SummarySeen -or
      -not $appLoopbackGui.QuickFailureSeen -or
      -not $appLoopbackGui.Restored) {
    Stop-ProcessByIdIfRunning $p.Id
    throw "GUI smoke failed: application-loopback source-mode semantics did not surface correctly."
  }
  $matrixBusySurface = Invoke-NormalizeBusyProbeSurface $hwnd 1118 1117 1122 1104 1105 @(1119, 1120, 1121) 60
  Write-Host "GUI matrix busy surface normalized: $($matrixBusySurface.NormalizedOk)"
  if (-not $matrixBusySurface.NormalizedOk) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: matrix busy-cycle surface could not be normalized."
  }
  $matrix = Invoke-ProbeUiCheck $hwnd 1005 1004 1001 1002 "Probe Matrix Running" "Probe Matrix Running..." "Run Probe Matrix" $watchedControls 1800
  $matrixBusySurfaceRestored = Invoke-RestoreBusyProbeSurface $hwnd $matrixBusySurface.Baseline 1118 1117 1122 60
  Write-Host "GUI matrix title busy seen: $($matrix.TitleBusySeen)"
  Write-Host "GUI matrix text busy seen: $($matrix.ProbeTextBusySeen)"
  Write-Host "GUI matrix disabled seen: $($matrix.ProbeDisabledSeen)"
  Write-Host "GUI matrix paired probe disabled seen: $($matrix.PairedProbeDisabledSeen)"
  Write-Host "GUI matrix start disabled seen: $($matrix.StartDisabledSeen)"
  Write-Host "GUI matrix stop stayed disabled: $($matrix.StopStayedDisabled)"
  Write-Host "GUI matrix busy surface restored: $matrixBusySurfaceRestored"
  Write-Host "GUI matrix watched availability: $(if($matrix.AvailabilityIssues.Count -eq 0){'OK'}else{$matrix.AvailabilityIssues -join ', '})"
  Write-Host "GUI matrix watched disabled seen: $(Format-StateMap $matrix.WatchedDisabledSeen)"
  Write-Host "GUI matrix watched restored: $(Format-StateMap $matrix.WatchedRestored)"
  Write-Host "GUI matrix watched baseline enabled: $(Format-StateMap $matrix.BaselineEnabled)"
  Write-Host "GUI matrix watched final enabled: $(Format-StateMap $matrix.FinalEnabled)"
  Write-Host "GUI matrix watched baseline values: $(Format-SnapshotMap $matrix.BaselineValue)"
  Write-Host "GUI matrix watched final values: $(Format-SnapshotMap $matrix.FinalValue)"
  Write-Host "GUI matrix baseline restored: $($matrix.BaselineRestored)"
  Write-Host "GUI matrix restored: $($matrix.Restored)"
  if ($matrix.AvailabilityIssues.Count -ne 0 -or
      -not $matrix.TitleBusySeen -or
      -not $matrix.ProbeTextBusySeen -or
      -not $matrix.ProbeDisabledSeen -or
      -not $matrix.PairedProbeDisabledSeen -or
      -not $matrix.StartDisabledSeen -or
      -not $matrix.StopStayedDisabled -or
      -not $matrixBusySurfaceRestored -or
      -not (Test-AllStatesTrue $matrix.WatchedDisabledSeen) -or
      -not (Test-AllStatesTrue $matrix.WatchedRestored) -or
      -not $matrix.BaselineRestored -or
      -not $matrix.Restored) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: probe matrix busy cycle incomplete."
  }
  $matrixWhileRunning = Invoke-RunningSessionProbeCycleCheck $hwnd 1005 1004 1001 1002 "Probe Matrix Running" "Probe Matrix Running..." "Run Probe Matrix" $watchedControls 1800
  Write-Host "GUI matrix while-running baseline seen: $($matrixWhileRunning.RunningBaselineSeen)"
  Write-Host "GUI matrix while-running title busy seen: $($matrixWhileRunning.ProbeCycle.TitleBusySeen)"
  Write-Host "GUI matrix while-running text busy seen: $($matrixWhileRunning.ProbeCycle.ProbeTextBusySeen)"
  Write-Host "GUI matrix while-running probe disabled seen: $($matrixWhileRunning.ProbeCycle.ProbeDisabledSeen)"
  Write-Host "GUI matrix while-running paired probe disabled seen: $($matrixWhileRunning.ProbeCycle.PairedProbeDisabledSeen)"
  Write-Host "GUI matrix while-running start disabled seen: $($matrixWhileRunning.ProbeCycle.StartDisabledSeen)"
  Write-Host "GUI matrix while-running stop available seen: $($matrixWhileRunning.StopAvailableSeen)"
  Write-Host "GUI matrix while-running restored: $($matrixWhileRunning.RunningRestored)"
  Write-Host "GUI matrix while-running stopped cleanly: $($matrixWhileRunning.StoppedCleanly)"
  if (-not $matrixWhileRunning.RunningBaselineSeen -or
      -not $matrixWhileRunning.ProbeCycle.TitleBusySeen -or
      -not $matrixWhileRunning.ProbeCycle.ProbeTextBusySeen -or
      -not $matrixWhileRunning.ProbeCycle.ProbeDisabledSeen -or
      -not $matrixWhileRunning.ProbeCycle.PairedProbeDisabledSeen -or
      -not $matrixWhileRunning.ProbeCycle.StartDisabledSeen -or
      -not $matrixWhileRunning.StopAvailableSeen -or
      -not $matrixWhileRunning.RunningRestored -or
      -not $matrixWhileRunning.StoppedCleanly) {
    Stop-WindowProcessIfRunning $p
    throw "GUI smoke failed: probe matrix did not restore the running session correctly."
  }
  Stop-WindowProcessIfRunning $p

  $closeDuringQuickProbe = Invoke-CloseDuringBusyProbeCheck $guiExe 1004 "Quick Probe Running" "Quick Probe Running..." 300
  Write-Host "GUI close-during-quick-probe window ready: $($closeDuringQuickProbe.WindowReady)"
  Write-Host "GUI close-during-quick-probe busy seen: $($closeDuringQuickProbe.BusySeen)"
  Write-Host "GUI close-during-quick-probe exited cleanly: $($closeDuringQuickProbe.ExitedCleanly)"
  if (-not $closeDuringQuickProbe.WindowReady -or
      -not $closeDuringQuickProbe.BusySeen -or
      -not $closeDuringQuickProbe.ExitedCleanly) {
    throw "GUI smoke failed: close-during-quick-probe lifecycle check failed."
  }
  $closeDuringMatrixProbe = Invoke-CloseDuringBusyProbeCheck $guiExe 1005 "Probe Matrix Running" "Probe Matrix Running..." 300
  Write-Host "GUI close-during-matrix-probe window ready: $($closeDuringMatrixProbe.WindowReady)"
  Write-Host "GUI close-during-matrix-probe busy seen: $($closeDuringMatrixProbe.BusySeen)"
  Write-Host "GUI close-during-matrix-probe exited cleanly: $($closeDuringMatrixProbe.ExitedCleanly)"
  if (-not $closeDuringMatrixProbe.WindowReady -or
      -not $closeDuringMatrixProbe.BusySeen -or
      -not $closeDuringMatrixProbe.ExitedCleanly) {
    throw "GUI smoke failed: close-during-matrix-probe lifecycle check failed."
  }
  $closeDuringQuickProbeWhileRunning = Invoke-CloseDuringBusyProbeWhileRunningCheck $guiExe 1004 1001 1002 "Quick Probe Running" "Quick Probe Running..." 300
  Write-Host "GUI close-during-quick-probe-while-running window ready: $($closeDuringQuickProbeWhileRunning.WindowReady)"
  Write-Host "GUI close-during-quick-probe-while-running running seen: $($closeDuringQuickProbeWhileRunning.RunningSeen)"
  Write-Host "GUI close-during-quick-probe-while-running busy seen: $($closeDuringQuickProbeWhileRunning.BusySeen)"
  Write-Host "GUI close-during-quick-probe-while-running exited cleanly: $($closeDuringQuickProbeWhileRunning.ExitedCleanly)"
  if (-not $closeDuringQuickProbeWhileRunning.WindowReady -or
      -not $closeDuringQuickProbeWhileRunning.RunningSeen -or
      -not $closeDuringQuickProbeWhileRunning.BusySeen -or
      -not $closeDuringQuickProbeWhileRunning.ExitedCleanly) {
    throw "GUI smoke failed: close-during-quick-probe-while-running lifecycle check failed."
  }
  $closeDuringMatrixProbeWhileRunning = Invoke-CloseDuringBusyProbeWhileRunningCheck $guiExe 1005 1001 1002 "Probe Matrix Running" "Probe Matrix Running..." 300
  Write-Host "GUI close-during-matrix-probe-while-running window ready: $($closeDuringMatrixProbeWhileRunning.WindowReady)"
  Write-Host "GUI close-during-matrix-probe-while-running running seen: $($closeDuringMatrixProbeWhileRunning.RunningSeen)"
  Write-Host "GUI close-during-matrix-probe-while-running busy seen: $($closeDuringMatrixProbeWhileRunning.BusySeen)"
  Write-Host "GUI close-during-matrix-probe-while-running exited cleanly: $($closeDuringMatrixProbeWhileRunning.ExitedCleanly)"
  if (-not $closeDuringMatrixProbeWhileRunning.WindowReady -or
      -not $closeDuringMatrixProbeWhileRunning.RunningSeen -or
      -not $closeDuringMatrixProbeWhileRunning.BusySeen -or
      -not $closeDuringMatrixProbeWhileRunning.ExitedCleanly) {
    throw "GUI smoke failed: close-during-matrix-probe-while-running lifecycle check failed."
  }
  $closeWhileRunningSession = Invoke-CloseWhileRunningSessionCheck $guiExe 1001 1002 300
  Write-Host "GUI close-while-running-session window ready: $($closeWhileRunningSession.WindowReady)"
  Write-Host "GUI close-while-running-session running seen: $($closeWhileRunningSession.RunningSeen)"
  Write-Host "GUI close-while-running-session exited cleanly: $($closeWhileRunningSession.ExitedCleanly)"
  if (-not $closeWhileRunningSession.WindowReady -or
      -not $closeWhileRunningSession.RunningSeen -or
      -not $closeWhileRunningSession.ExitedCleanly) {
    throw "GUI smoke failed: close-while-running-session lifecycle check failed."
  }
}

if (-not $GuiSmokeOnly) {
  Write-Step "CTest"
  Invoke-ConvergenceCommand { ctest --test-dir $BuildDir -C $Config --output-on-failure } "CTest failed"
}

