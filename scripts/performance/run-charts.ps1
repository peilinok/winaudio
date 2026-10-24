param(
    [Parameter(Mandatory=$true)][string]$EndpointId,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$BuildDirectory = 'build/performance-vs18',
    [ValidateSet('baseline','overhead','smoke')][string]$Plan = 'baseline'
)

$ErrorActionPreference = 'Stop'
$perfOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $perfOutput | Out-Null
$perfExecutable = [IO.Path]::GetFullPath("$BuildDirectory/bin/Release/WinAudioPerformanceGui.exe")
$perfVersion = git rev-parse HEAD
$perfRuns = if ($Plan -eq 'baseline') {
    foreach ($perfRepeat in 1..3) {
        foreach ($perfTracks in 2,1) {
            [pscustomobject]@{name="charts-$perfTracks-$perfRepeat"; tracks=$perfTracks; mode='buffered'; warmup=30; duration=120}
        }
    }
} elseif ($Plan -eq 'overhead') {
    foreach ($perfMode in 'off','buffered','flush') {
        [pscustomobject]@{name="charts-overhead-$perfMode"; tracks=2; mode=$perfMode; warmup=30; duration=120}
    }
} else {
    [pscustomobject]@{name='charts-smoke'; tracks=2; mode='buffered'; warmup=3; duration=40}
}
foreach ($perfRun in $perfRuns) {
    $perfFolder = Join-Path $perfOutput $perfRun.name
    if (Test-Path -LiteralPath $perfFolder) { throw "Refusing to append to an existing run: $perfFolder" }
    New-Item -ItemType Directory -Path $perfFolder | Out-Null
    $env:WINAUDIO_FRAME_PROBE = if ($perfRun.mode -eq 'off') { '0' } else { '1' }
    $env:WINAUDIO_FRAME_PROBE_FLUSH = if ($perfRun.mode -eq 'flush') { '1' } else { '0' }
    $env:WINAUDIO_FRAME_PROBE_PATH = Join-Path $perfFolder 'frames.txt'
    $env:WINAUDIO_PERF_ENDPOINT = $EndpointId
    $env:WINAUDIO_PERF_TRACKS = [string]$perfRun.tracks
    $env:WINAUDIO_PERF_WARMUP = [string]$perfRun.warmup
    $env:WINAUDIO_PERF_DURATION = [string]$perfRun.duration
    [pscustomobject]@{source=$perfVersion; executable=$perfExecutable;
        executable_sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $perfExecutable).Hash;
        endpoint=$EndpointId; configuration=$perfRun; started=(Get-Date -Format o)} |
        ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 -LiteralPath "$perfFolder/config.json"
    Write-Output "Starting $($perfRun.name)"
    $perfProcess = Start-Process -FilePath $perfExecutable -WorkingDirectory $perfFolder -PassThru
    $perfSamples = @()
    $perfClock = [Diagnostics.Stopwatch]::StartNew()
    while (!$perfProcess.WaitForExit(1000)) {
        $perfProcess.Refresh()
        $perfSamples += [pscustomobject]@{elapsed_s=$perfClock.Elapsed.TotalSeconds;
            cpu_ms=$perfProcess.TotalProcessorTime.TotalMilliseconds;
            private_bytes=$perfProcess.PrivateMemorySize64; working_set=$perfProcess.WorkingSet64}
    }
    $perfProcess.WaitForExit()
    $perfSamples | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$perfFolder/process.csv"
    if ($perfProcess.ExitCode -ne 0) { throw "Performance GUI failed: $($perfRun.name), exit=$($perfProcess.ExitCode)" }
    if ($perfRun.mode -ne 'off') {
        py -3 -B scripts/performance/summarize.py "$perfFolder/frames.txt" --warmup-seconds 0 `
            --duration-seconds ($perfRun.warmup + $perfRun.duration + 5) --require-complete --output "$perfFolder/summary.json"
        if ($LASTEXITCODE -ne 0) { throw "Incomplete frame data: $($perfRun.name)" }
    }
    Write-Output "Completed $($perfRun.name)"
}
