function Select-ConvergenceSummaryLines {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string[]]$Patterns
  )

  Get-Content -LiteralPath $Path | Where-Object {
    $line = $_
    foreach ($pattern in $Patterns) {
      if ($line -match $pattern) {
        return $true
      }
    }
    return $false
  }
}

function Extract-PreferredIdFromFile {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$Prefix
  )

  $matches = Select-String -LiteralPath $Path -Pattern ("^" + [regex]::Escape($Prefix))
  if ($null -eq $matches -or $matches.Count -eq 0) {
    return $null
  }

  $defaultLine = $matches |
    Where-Object { $_.Line -match ' \| Default(\s|\||$)' } |
    Select-Object -First 1
  $line = if ($null -ne $defaultLine) {
    $defaultLine
  } else {
    $matches | Select-Object -First 1
  }

  $text = $line.Line
  $firstQuote = $text.IndexOf('"')
  if ($firstQuote -lt 0) {
    return $null
  }
  $secondQuote = $text.IndexOf('"', $firstQuote + 1)
  if ($secondQuote -lt 0) {
    return $null
  }
  return $text.Substring($firstQuote + 1, $secondQuote - $firstQuote - 1)
}

function Assert-QuickSourceModeFailureSemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  $requirements = @(
    @{ Pattern = '^QuickSummary: failed \| dump=none \| cap-fmt=not-negotiated \| ren-fmt=not-negotiated \| mode=not-started \| monitor=on \| cap-wave=not-started \| ren-wave=not-started \| ren-updates=0$'; Message = "$FailurePrefix did not surface the expected not-negotiated/not-started summary semantics." },
    @{ Pattern = '^NegotiatedCapture: not-negotiated$'; Message = "$FailurePrefix did not mark negotiated capture as not-negotiated." },
    @{ Pattern = '^NegotiatedRender: not-negotiated$'; Message = "$FailurePrefix did not mark negotiated render as not-negotiated." },
    @{ Pattern = '^CaptureFormatMatch: not-negotiated$'; Message = "$FailurePrefix did not mark capture format match as not-negotiated." },
    @{ Pattern = '^RenderFormatMatch: not-negotiated$'; Message = "$FailurePrefix did not mark render format match as not-negotiated." },
    @{ Pattern = '^Waveform: not-started$'; Message = "$FailurePrefix did not mark waveform state as not-started." },
    @{ Pattern = '^CaptureWave: not-started$'; Message = "$FailurePrefix did not mark capture waveform as not-started." },
    @{ Pattern = '^RenderWave: not-started$'; Message = "$FailurePrefix did not mark render waveform as not-started." },
    @{ Pattern = '^CaptureMode: not-started$'; Message = "$FailurePrefix did not mark capture mode as not-started." },
    @{ Pattern = '^RenderMode: not-started$'; Message = "$FailurePrefix did not mark render mode as not-started." },
    @{ Pattern = '^Resampler: not-started$'; Message = "$FailurePrefix did not mark the resampler as not-started." },
    @{ Pattern = '^CaptureRuntime: not-started$'; Message = "$FailurePrefix did not mark capture runtime as not-started." },
    @{ Pattern = '^RenderRuntime: not-started$'; Message = "$FailurePrefix did not mark render runtime as not-started." },
    @{ Pattern = '^FailureStage: source-mode$'; Message = "$FailurePrefix did not report the expected failure stage." }
  )

  foreach ($requirement in $requirements) {
    if (-not (Select-String -LiteralPath $Path -Pattern $requirement.Pattern -Quiet)) {
      throw $requirement.Message
    }
  }
}

function Assert-QuickInvalidRenderDeviceSemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  $requirements = @(
    @{ Pattern = '^FailureStage: render-device$'; Message = "$FailurePrefix did not report the expected failure stage." },
    @{ Pattern = '^RequestedRenderDeviceId: definitely-invalid-render-device$'; Message = "$FailurePrefix did not preserve the requested invalid render device id." },
    @{ Pattern = '^FailureReason: Selected render device is not available for this backend\. Choose a device from devices for the same render backend, or omit --render-device-id\.$'; Message = "$FailurePrefix did not surface the expected render-device recovery guidance." }
  )

  foreach ($requirement in $requirements) {
    if (-not (Select-String -LiteralPath $Path -Pattern $requirement.Pattern -Quiet)) {
      throw $requirement.Message
    }
  }
}

function Assert-QuickInvalidLoopbackCaptureDeviceSemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  $requirements = @(
    @{ Pattern = '^FailureStage: source-mode$'; Message = "$FailurePrefix did not report the expected failure stage." },
    @{ Pattern = '^RequestedCaptureDeviceId: not-a-loopback-render-endpoint$'; Message = "$FailurePrefix did not preserve the requested invalid loopback capture id." },
    @{ Pattern = '^FailureReason: Selected loopback capture device is not available for this source\. Use devices --source=loopback to choose a render-backed loopback endpoint\.$'; Message = "$FailurePrefix did not surface the expected loopback recovery guidance." }
  )

  foreach ($requirement in $requirements) {
    if (-not (Select-String -LiteralPath $Path -Pattern $requirement.Pattern -Quiet)) {
      throw $requirement.Message
    }
  }
}

function Assert-QuickMonitorOffInvalidRenderDeviceSemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  $requirements = @(
    @{ Pattern = '^QuickSummary: success'; Message = "$FailurePrefix did not succeed." },
    @{ Pattern = 'monitor=off'; Message = "$FailurePrefix did not report monitor=off in the summary." },
    @{ Pattern = 'ren-fmt=disabled'; Message = "$FailurePrefix did not mark render format as disabled in the summary." },
    @{ Pattern = '^RequestedRenderDeviceId: definitely-invalid-render-device$'; Message = "$FailurePrefix did not preserve the requested invalid render device id." },
    @{ Pattern = '^NegotiatedRender: disabled$'; Message = "$FailurePrefix did not mark negotiated render as disabled." },
    @{ Pattern = '^RenderFormatMatch: disabled$'; Message = "$FailurePrefix did not mark render format match as disabled." },
    @{ Pattern = '^RenderRuntime: disabled$'; Message = "$FailurePrefix did not mark render runtime as disabled." },
    @{ Pattern = '^RenderWaveNote: render monitoring is disabled because monitor playback is off$'; Message = "$FailurePrefix did not surface the disabled render monitoring explanation." },
    @{ Pattern = '^FailureStage: none$'; Message = "$FailurePrefix still reported a failure stage." }
  )

  foreach ($requirement in $requirements) {
    if (-not (Select-String -LiteralPath $Path -Pattern $requirement.Pattern -Quiet)) {
      throw $requirement.Message
    }
  }
}

function Assert-MatrixDelayScopeSemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedDelay,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  if (-not (Select-String -LiteralPath $Path -Pattern ("^-\sDelaySummary: " + [regex]::Escape($ExpectedDelay) + "(\s|$)") -Quiet)) {
    throw "$FailurePrefix did not narrow to delay=$ExpectedDelay."
  }

  $unexpected = @("0ms", "120ms") | Where-Object { $_ -ne $ExpectedDelay }
  foreach ($delay in $unexpected) {
    if (Select-String -LiteralPath $Path -Pattern ("DelaySummary: " + [regex]::Escape($delay)) -Quiet) {
      throw "$FailurePrefix still contains delay=$delay."
    }
  }
}

function Assert-MatrixBufferScopeSemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedBuffer,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  if (-not (Select-String -LiteralPath $Path -Pattern ("^-\sBufferSummary: " + [regex]::Escape($ExpectedBuffer) + "(\s|$)") -Quiet)) {
    throw "$FailurePrefix did not narrow to buffer=$ExpectedBuffer."
  }

  $unexpected = @("cap40-ren40", "cap80-ren120") | Where-Object { $_ -ne $ExpectedBuffer }
  foreach ($buffer in $unexpected) {
    if (Select-String -LiteralPath $Path -Pattern ("BufferSummary: " + [regex]::Escape($buffer)) -Quiet) {
      throw "$FailurePrefix still contains buffer=$buffer."
    }
  }
}

function Assert-MatrixAlignScopeSemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedAlign,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  if (-not (Select-String -LiteralPath $Path -Pattern ("^-\sAlignSummary: " + [regex]::Escape($ExpectedAlign) + "(\s|$)") -Quiet)) {
    throw "$FailurePrefix did not narrow to align=$ExpectedAlign."
  }

  $unexpected = @("on", "off") | Where-Object { $_ -ne $ExpectedAlign }
  foreach ($align in $unexpected) {
    if (Select-String -LiteralPath $Path -Pattern ("AlignSummary: " + [regex]::Escape($align)) -Quiet) {
      throw "$FailurePrefix still contains align=$align."
    }
  }
}

function Assert-MatrixSharedOnlySemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  if (-not (Select-String -LiteralPath $Path -Pattern 'UNSUPPORTED_MODE=0' -Quiet)) {
    throw "$FailurePrefix did not eliminate unsupported-mode noise."
  }

  if (Select-String -LiteralPath $Path -Pattern '\[unsupported-mode\]' -Quiet) {
    throw "$FailurePrefix still contains unsupported-mode rows."
  }
}

function Assert-ExplicitLoopbackMatrixSemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [string]$ExpectedCaptureId,

    [string]$ExpectedRenderId,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  if (Select-String -LiteralPath $Path -Pattern 'Microphone \|' -Quiet) {
    throw "$FailurePrefix still contains microphone rows."
  }

  if (-not (Select-String -LiteralPath $Path -Pattern 'System Loopback \|' -Quiet)) {
    throw "$FailurePrefix produced no loopback rows."
  }

  if (-not [string]::IsNullOrWhiteSpace($ExpectedCaptureId) -and
      -not (Select-String -LiteralPath $Path -Pattern ("cap-dev=" + [regex]::Escape($ExpectedCaptureId)) -Quiet)) {
    throw "$FailurePrefix did not report the requested loopback capture device id."
  }

  if (-not [string]::IsNullOrWhiteSpace($ExpectedRenderId) -and
      -not (Select-String -LiteralPath $Path -Pattern ("ren-dev=" + [regex]::Escape($ExpectedRenderId)) -Quiet)) {
    throw "$FailurePrefix did not report the requested render device id."
  }

  if (-not (Select-String -LiteralPath $Path -Pattern 'RENDER_DEVICE_FAIL=' -Quiet)) {
    throw "$FailurePrefix did not surface render-device failure accounting."
  }

  if (-not (Select-String -LiteralPath $Path -Pattern '^- MatrixHint: render-device failures are clustering\.' -Quiet)) {
    throw "$FailurePrefix did not surface the render-device clustering hint."
  }
}

function Assert-WaveRenderLoopbackMatrixSemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  if (-not (Select-String -LiteralPath $Path -Pattern '^- MatrixSummary:' -Quiet)) {
    throw "$FailurePrefix produced no matrix summary."
  }

  if (-not (Select-String -LiteralPath $Path -Pattern '^- BackendSummary: WASAPI -> WAVE API' -Quiet)) {
    throw "$FailurePrefix did not include the WASAPI -> WAVE API backend summary."
  }

  if (-not (Select-String -LiteralPath $Path -Pattern '^- BackendSummary: WAVE API -> WAVE API' -Quiet)) {
    throw "$FailurePrefix did not include the WAVE API -> WAVE API backend summary."
  }

  if (Select-String -LiteralPath $Path -Pattern 'BackendSummary: WAVE API -> WASAPI|BackendSummary: WASAPI -> WASAPI' -Quiet) {
    throw "$FailurePrefix still contains non-wave render backend summaries."
  }
}

function Assert-WasapiWaveLoopbackMatrixSemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  if (-not (Select-String -LiteralPath $Path -Pattern '^- MatrixSummary:' -Quiet)) {
    throw "$FailurePrefix produced no matrix summary."
  }

  if (-not (Select-String -LiteralPath $Path -Pattern '^- BackendSummary: WASAPI -> WAVE API' -Quiet)) {
    throw "$FailurePrefix did not narrow to the expected WASAPI -> WAVE API backend pair."
  }

  if (-not (Select-String -LiteralPath $Path -Pattern 'UNSUPPORTED_MODE=16' -Quiet)) {
    throw "$FailurePrefix did not surface the expected unsupported-mode concentration."
  }

  if (-not (Select-String -LiteralPath $Path -Pattern '\[unsupported-mode\] \{WASAPI loopback requires shared mode\.\}' -Quiet)) {
    throw "$FailurePrefix did not preserve the expected unsupported-mode rows."
  }
}

function Assert-Pcm16LoopbackMatrixSemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  if (-not (Select-String -LiteralPath $Path -Pattern '^- ProfileSummary: PCM16-48k-stereo' -Quiet)) {
    throw "$FailurePrefix did not narrow to the expected PCM16-48k-stereo profile."
  }

  if (Select-String -LiteralPath $Path -Pattern 'PCM24-44k-mono' -Quiet) {
    throw "$FailurePrefix still contains the PCM24-44k-mono profile."
  }
}

function Assert-LoopbackDevicesOutputSemantics {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$FailurePrefix
  )

  if (-not (Select-String -LiteralPath $Path -Pattern '^Loopback Capture Devices \(' -Quiet)) {
    throw "$FailurePrefix did not label loopback capture devices clearly."
  }

  if (-not (Select-String -LiteralPath $Path -Pattern '^Loopback capture uses render endpoints as capture sources\.$' -Quiet)) {
    throw "$FailurePrefix did not explain render-backed capture sources."
  }

  $guidanceCount = (Select-String -LiteralPath $Path -Pattern '^Loopback capture uses render endpoints as capture sources\.$').Count
  if ($guidanceCount -ne 1) {
    throw "$FailurePrefix repeated the render-backed capture guidance line."
  }
}

function Invoke-ConvergenceCommand {
  param(
    [Parameter(Mandatory = $true)]
    [scriptblock]$Command,

    [Parameter(Mandatory = $true)]
    [string]$FailureMessage
  )

  & $Command
  if ($LASTEXITCODE -ne 0) {
    throw "$FailureMessage (exit=$LASTEXITCODE)"
  }
}
