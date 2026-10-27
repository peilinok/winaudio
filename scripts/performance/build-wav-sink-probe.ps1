param(
    [Parameter(Mandatory=$true)][string]$Revision,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('x64','Win32')][string]$Architecture = 'x64',
    [string]$Generator = 'Visual Studio 18 2026'
)
$ErrorActionPreference = 'Stop'
$wavRepository = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$wavOutput = [IO.Path]::GetFullPath($OutputDirectory)
$wavSource = Join-Path $wavOutput 'source'
$wavBuild = Join-Path $wavOutput 'build'
if (Test-Path -LiteralPath $wavOutput) { throw 'Refusing to replace a measurement directory' }
New-Item -ItemType Directory -Path $wavOutput | Out-Null
@'
import io
from pathlib import Path
import subprocess
import sys
import tarfile
root, output, revision = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
sha = subprocess.check_output(['git', '-C', str(root), 'rev-parse', revision + '^{commit}'], text=True).strip()
source = output / 'source'
source.mkdir()
def extract(repository, ref, destination):
    data = subprocess.check_output(['git', '-C', str(repository), 'archive', ref])
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        archive.extractall(str(destination))
extract(root, sha, source)
dependency = subprocess.check_output(['git', '-C', str(root), 'ls-tree', sha, 'third_party/spdlog'], text=True).split()[2]
extract(root / 'third_party/spdlog', dependency, source / 'third_party/spdlog')
(output / 'source.txt').write_text(sha + '\nspdlog=' + dependency + '\n', encoding='utf-8')
with (source / 'CMakeLists.txt').open('a', encoding='utf-8') as cmake:
    cmake.write('\nadd_executable(WavSinkProbe wav-sink-probe.cpp)\ntarget_link_libraries(WavSinkProbe PRIVATE WinAudioCore)\nwa_set_project_warnings(WavSinkProbe)\n')
    cmake.write('add_executable(WavDumpDeviceSmoke wav-dump-device-smoke.cpp)\ntarget_link_libraries(WavDumpDeviceSmoke PRIVATE WinAudioCore)\nwa_set_project_warnings(WavDumpDeviceSmoke)\n')
'@ | py -3 -B - $wavRepository $wavOutput $Revision
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract the pinned WAV sink source' }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'wav-sink-probe.cpp') -Destination $wavSource
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'wav-dump-device-smoke.cpp') -Destination $wavSource
cmake -S $wavSource -B $wavBuild -G $Generator -A $Architecture
if ($LASTEXITCODE -ne 0) { throw 'WAV sink probe configure failed' }
cmake --build $wavBuild --config Release --target WavSinkProbe WavDumpDeviceSmoke -j 6
if ($LASTEXITCODE -ne 0) { throw 'WAV sink probe build failed' }
$wavExecutable = Join-Path $wavBuild 'bin/Release/WavSinkProbe.exe'
Get-FileHash -Algorithm SHA256 -LiteralPath $wavExecutable | Format-List | Out-File -LiteralPath (Join-Path $wavOutput 'executable.txt') -Encoding utf8
& $wavExecutable (Join-Path $wavOutput 'readback.wav') | Out-File -LiteralPath (Join-Path $wavOutput 'result.csv') -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw 'WAV sink probe or readback failed' }
Get-Content -LiteralPath (Join-Path $wavOutput 'result.csv')
