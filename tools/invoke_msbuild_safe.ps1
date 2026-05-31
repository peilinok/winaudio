param(
  [switch]$PrintEnvironmentSummary,

  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$Command
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "build_environment_helpers.ps1")

if (-not $Command -or $Command.Length -eq 0) {
  throw "No command provided."
}

$summary = Get-BuildEnvironmentSummary
if ([string]::IsNullOrEmpty($summary.CanonicalValue)) {
  throw "Unable to resolve a PATH/Path value from the current process environment."
}

if ($PrintEnvironmentSummary) {
  Write-BuildEnvironmentSummary -Summary $summary -Prefix "SafeBuild:"
}

function Invoke-SafeBuildCommand([string[]]$CommandParts) {
  $escapedPath = $summary.CanonicalValue.Replace('"', '""')
  $commandLine = [string]::Join(' ', ($CommandParts | ForEach-Object {
        if ($_ -match '\s') { '"' + $_.Replace('"', '""') + '"' } else { $_ }
      }))

  $cmd = "set PATH=$escapedPath && $commandLine"
  $output = & cmd /c $cmd 2>&1
  $exitCode = $LASTEXITCODE
  [pscustomobject]@{
    Output = $output
    ExitCode = $exitCode
  }
}

function Stop-KnownLockedBinaryFromLnk1168([object[]]$outputLines) {
  foreach ($line in $outputLines) {
    $text = [string]$line
    if ($text -match 'LNK1168:.*\\([^\\]+)\.exe') {
      $processName = [System.IO.Path]::GetFileNameWithoutExtension($Matches[1])
      if ($processName -in @('winaudio', 'winaudio_probe', 'app_model_text_test', 'probe_ui_text_test', 'probe_cli_test', 'session_controller_test', 'core_pipeline_test', 'wave_format_utils_test')) {
        Get-Process -Name $processName -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        return $true
      }
    }
  }
  return $false
}

$result = Invoke-SafeBuildCommand $Command
foreach ($line in $result.Output) {
  Write-Output $line
}

if ($result.ExitCode -ne 0 -and (Stop-KnownLockedBinaryFromLnk1168 $result.Output)) {
  Write-Output "SafeBuild: detected LNK1168 on a known workspace binary; terminated stale process and retrying once."
  $result = Invoke-SafeBuildCommand $Command
  foreach ($line in $result.Output) {
    Write-Output $line
  }
}

exit $result.ExitCode
