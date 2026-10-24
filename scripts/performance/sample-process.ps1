param(
    [Parameter(Mandatory=$true)][int]$ProcessId,
    [Parameter(Mandatory=$true)][string]$Mode,
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [int]$WarmupSeconds = 10,
    [int]$DurationSeconds = 60
)

$ErrorActionPreference = 'Stop'
$perfRows = @()
$perfClock = [Diagnostics.Stopwatch]::StartNew()
$perfPrevCpu = $null
$perfPrevWall = $null
for ($perfIndex = 0; $perfIndex -le $WarmupSeconds + $DurationSeconds; $perfIndex++) {
    $perfProcess = Get-Process -Id $ProcessId
    $perfWall = $perfClock.Elapsed.TotalSeconds
    $perfCpu = $perfProcess.TotalProcessorTime.TotalMilliseconds
    $perfRate = if ($null -eq $perfPrevCpu) { 0 } else {
        ($perfCpu - $perfPrevCpu) / ($perfWall - $perfPrevWall)
    }
    $perfRows += [pscustomobject]@{
        mode = $Mode; sample = $perfIndex; elapsed_s = $perfWall
        cpu_ms = $perfCpu; cpu_ms_per_s = $perfRate
        private_bytes = $perfProcess.PrivateMemorySize64
        working_set = $perfProcess.WorkingSet64
        warmup = ($perfIndex -le $WarmupSeconds)
    }
    $perfPrevCpu = $perfCpu
    $perfPrevWall = $perfWall
    if ($perfIndex -lt $WarmupSeconds + $DurationSeconds) { [Threading.Thread]::Sleep(1000) }
}
$perfRows | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath $OutputPath
$perfRows | Where-Object { !$_.warmup } | Measure-Object cpu_ms_per_s -Average -Minimum -Maximum
