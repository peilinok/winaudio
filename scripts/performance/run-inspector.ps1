param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$BuildDirectory = 'build/performance-vs18',
    [string]$SourceRevision = '',
    [ValidateSet('baseline','memory','overhead','live','live-smoke','smoke')][string]$Plan = 'baseline',
    [string[]]$RunNames = @()
)

$ErrorActionPreference = 'Stop'
$perfOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $perfOutput | Out-Null
$perfExecutable = [IO.Path]::GetFullPath("$BuildDirectory/bin/Release/WinAudioPerformanceGui.exe")
$perfVersion = if ($SourceRevision) { git rev-parse $SourceRevision } else { git rev-parse HEAD }
$perfRuns = if ($Plan -eq 'baseline') {
    foreach ($perfRepeat in 1..3) {
        [pscustomobject]@{name="inspector-1000-$perfRepeat"; records=1000; pids=2; mode='browse'; warmup=30; duration=120}
        [pscustomobject]@{name="inspector-10000-$perfRepeat"; records=10000; pids=2; mode='browse'; warmup=30; duration=120}
        [pscustomobject]@{name="inspector-single-$perfRepeat"; records=10000; pids=1; mode='browse'; warmup=30; duration=120}
        [pscustomobject]@{name="inspector-append-$perfRepeat"; records=10000; pids=2; mode='append'; warmup=30; duration=120}
        [pscustomobject]@{name="inspector-save-$perfRepeat"; records=10000; pids=2; mode='save'; warmup=30; duration=120}
    }
} elseif ($Plan -eq 'memory') {
    [pscustomobject]@{name='inspector-memory'; records=10000; pids=2; mode='memory'; warmup=30; duration=600}
    [pscustomobject]@{name='inspector-idle'; records=10000; pids=2; mode='idle'; warmup=30; duration=600}
} elseif ($Plan -eq 'overhead') {
    foreach ($perfProbeMode in 'off','buffered','flush') {
        [pscustomobject]@{name="inspector-overhead-$perfProbeMode"; records=10000; pids=2; mode='browse'; probe=$perfProbeMode; warmup=30; duration=120}
    }
} elseif ($Plan -eq 'live') {
    [pscustomobject]@{name='inspector-live'; records=0; pids=2; mode='live'; warmup=30; duration=620}
} elseif ($Plan -eq 'live-smoke') {
    [pscustomobject]@{name='inspector-live-smoke'; records=0; pids=2; mode='live'; warmup=3; duration=25}
} else {
    [pscustomobject]@{name='inspector-smoke'; records=10000; pids=2; mode='browse'; warmup=3; duration=20}
}
if ($RunNames.Count) { $perfRuns = @($perfRuns | Where-Object { $RunNames -contains $_.name }) }
if (!$perfRuns) { throw 'No matching measurement run' }
foreach ($perfRun in $perfRuns) {
    $perfFolder = Join-Path $perfOutput $perfRun.name
    if (Test-Path -LiteralPath $perfFolder) { throw "Refusing to append to an existing run: $perfFolder" }
    New-Item -ItemType Directory -Path $perfFolder | Out-Null
    $perfProbeMode = if ($perfRun.PSObject.Properties['probe']) { $perfRun.probe } else { 'buffered' }
    $env:WINAUDIO_FRAME_PROBE = if ($perfProbeMode -eq 'off') { '0' } else { '1' }
    $env:WINAUDIO_FRAME_PROBE_FLUSH = if ($perfProbeMode -eq 'flush') { '1' } else { '0' }
    $env:WINAUDIO_FRAME_PROBE_PATH = Join-Path $perfFolder 'frames.txt'
    $env:WINAUDIO_PERF_MODE = $perfRun.mode
    $env:WINAUDIO_PERF_RECORDS = [string]$perfRun.records
    $env:WINAUDIO_PERF_PIDS = [string]$perfRun.pids
    $env:WINAUDIO_PERF_WARMUP = [string]$perfRun.warmup
    $env:WINAUDIO_PERF_DURATION = [string]$perfRun.duration
    $env:WINAUDIO_PERF_PUMP = if ($Plan -eq 'live-smoke') { '1' } else { '0' }
    $perfVictimOwner = $null
    if ($perfRun.mode -eq 'live') {
        $perfVictimExe = [IO.Path]::GetFullPath("$BuildDirectory/bin/Release/HookAudioProbe.exe")
        $perfReadyFile = Join-Path $perfFolder 'targets.txt'
        $perfTargetDuration = if ($Plan -eq 'live-smoke') { '40' } else { '680' }
        $perfVictimOwner = Start-Process -FilePath $perfVictimExe -ArgumentList '--continuous',$perfTargetDuration `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput $perfReadyFile `
            -RedirectStandardError (Join-Path $perfFolder 'targets-error.txt')
        # Bind the native process handle before readiness polling caches HasExited.
        $null = $perfVictimOwner.Handle
        $perfTargetIds = @()
        for ($perfAttempt = 0; $perfAttempt -lt 150; ++$perfAttempt) {
            if ($perfVictimOwner.HasExited) { throw 'Controlled audio targets exited during setup' }
            if (Test-Path -LiteralPath $perfReadyFile) {
                $perfTargetIds = @(Get-Content -LiteralPath $perfReadyFile | ForEach-Object {
                    if ($_ -match '^ready target=\d+ pid=(\d+) ') { $Matches[1] }
                })
            }
            if ($perfTargetIds.Count -eq 2) { break }
            Start-Sleep -Milliseconds 100
        }
        if ($perfTargetIds.Count -ne 2) { throw 'Two fresh targets were not ready' }
        $env:WINAUDIO_PERF_LIVE_PIDS = $perfTargetIds -join ' '
    }
    $perfConfigPath = Join-Path $perfFolder 'config.json'
    $perfConfig = [pscustomobject]@{source=$perfVersion; executable=$perfExecutable;
        executable_sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $perfExecutable).Hash;
        configuration=$perfRun; started=(Get-Date -Format o);
        workspace_status=@(git status --short); live_pids=$env:WINAUDIO_PERF_LIVE_PIDS}
    $perfConfig | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 -LiteralPath $perfConfigPath
    Write-Output "Starting $($perfRun.name)"
    $perfProcess = Start-Process -FilePath $perfExecutable -WorkingDirectory $perfFolder -PassThru
    $perfSamples = @()
    $perfClock = [Diagnostics.Stopwatch]::StartNew()
    while (!$perfProcess.WaitForExit(1000)) {
        $perfProcess.Refresh()
        $perfSamples += [pscustomobject]@{elapsed_s=$perfClock.Elapsed.TotalSeconds;
            qpc_tick=[Diagnostics.Stopwatch]::GetTimestamp();
            cpu_ms=$perfProcess.TotalProcessorTime.TotalMilliseconds;
            private_bytes=$perfProcess.PrivateMemorySize64; working_set=$perfProcess.WorkingSet64}
    }
    $perfProcess.WaitForExit()
    $perfSamples | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath "$perfFolder/process.csv"
    $perfInputPath = Join-Path $perfFolder 'fixture-input.jsonl'
    if (Test-Path -LiteralPath $perfInputPath) {
        $perfConfig | Add-Member -NotePropertyName fixture_input_sha256 -NotePropertyValue (Get-FileHash -Algorithm SHA256 -LiteralPath $perfInputPath).Hash
    }
    if ($perfRun.mode -eq 'live') {
        $perfHookPaths = @(Get-Content -LiteralPath (Join-Path $perfFolder 'winaudio.log') | ForEach-Object {
            if ($_ -match 'PerformanceFixture::loaded-hook.*ret: (.+)$') { $Matches[1].Trim() }
        } | Sort-Object -Unique)
        if (!$perfHookPaths) { throw 'No loaded hook module path was observed' }
        $perfConfig | Add-Member -NotePropertyName loaded_hook_hashes -NotePropertyValue @($perfHookPaths | ForEach-Object {
            Get-FileHash -Algorithm SHA256 -LiteralPath $_ | Select-Object Path,Hash
        })
    }
    $perfConfig | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 -LiteralPath $perfConfigPath
    if ($perfVictimOwner) {
        $perfVictimOwner.WaitForExit()
        $perfVictimOwner.Refresh()
        if ($perfVictimOwner.ExitCode -ne 0) { throw "Controlled audio failed: exit=$($perfVictimOwner.ExitCode)" }
    }
    if ($perfProcess.ExitCode -ne 0) { throw "Performance GUI failed: $($perfRun.name), exit=$($perfProcess.ExitCode)" }
    if ($perfProbeMode -ne 'off') {
        py -3 -B scripts/performance/summarize.py "$perfFolder/frames.txt" --warmup-seconds 0 `
            --duration-seconds ($perfRun.warmup + $perfRun.duration + 5) --require-complete --output "$perfFolder/summary.json"
        if ($LASTEXITCODE -ne 0) { throw "Incomplete frame data: $($perfRun.name)" }
    }
    Write-Output "Completed $($perfRun.name)"
}
