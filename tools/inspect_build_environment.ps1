$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "build_environment_helpers.ps1")

$summary = Get-BuildEnvironmentSummary
Write-BuildEnvironmentSummary -Summary $summary
