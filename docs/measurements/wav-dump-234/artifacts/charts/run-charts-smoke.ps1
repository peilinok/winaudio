param([ValidateSet('before','after')][string]$Version)
$ErrorActionPreference = 'Stop'
$chartsRoot = 'E:\WORK\vibecoding\winaudio'
Set-Location -LiteralPath $chartsRoot
$chartsOutput = Join-Path $chartsRoot ".scratch/wav-dump-234/charts-$Version"
try {
    & scripts/performance/run-charts.ps1 `
        -EndpointId '{0.0.0.00000000}.{09793ccf-e246-4dbf-800b-a7cf44856994}' `
        -OutputDirectory $chartsOutput `
        -BuildDirectory ".scratch/wav-dump-234/performance-$Version/build" `
        -Plan smoke
    [pscustomobject]@{exit=0; completed=(Get-Date -Format o)} |
        ConvertTo-Json | Set-Content -Encoding UTF8 "$chartsOutput-exit.json"
} catch {
    [pscustomobject]@{exit=1; error=$_.Exception.Message; completed=(Get-Date -Format o)} |
        ConvertTo-Json | Set-Content -Encoding UTF8 "$chartsOutput-exit.json"
    throw
}
