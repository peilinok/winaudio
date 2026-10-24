param(
    [string]$SourceDirectory = 'build/performance-inspector-source',
    [string]$BuildDirectory = 'build/performance-inspector-before',
    [string]$Generator = 'Visual Studio 17 2022'
)
$ErrorActionPreference = 'Stop'
$perfRepository = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$perfSource = [IO.Path]::GetFullPath($SourceDirectory)
$perfBinary = [IO.Path]::GetFullPath($BuildDirectory)
if (Test-Path -LiteralPath $perfSource) { throw 'Refusing to replace a source archive' }
# Extract committed bytes, including the pinned submodule, without switching the
# implementation checkout or mixing in its uncommitted optimization work.
@'
import io
from pathlib import Path
import subprocess
import sys
import tarfile
root, destination = Path(sys.argv[1]), Path(sys.argv[2])
revision = '2f0b37ce0c63bd97cc265c2b436e2270154fce3e'
destination.mkdir(parents=True, exist_ok=False)
def extract(repository, target, output):
    data = subprocess.check_output(['git', '-C', str(repository), 'archive', target])
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        archive.extractall(str(output))
extract(root, revision, destination)
link = subprocess.check_output(['git', '-C', str(root), 'ls-tree', revision, 'third_party/spdlog'], text=True)
dependency = link.split()[2]
output = destination / 'third_party/spdlog'
output.mkdir(parents=True, exist_ok=True)
extract(root / 'third_party/spdlog', dependency, output)
(destination / 'baseline-source.txt').write_text(revision + '\nspdlog=' + dependency + '\n', encoding='utf-8')
'@ | py -3 -B - $perfRepository $perfSource
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract the pinned Inspector baseline' }
cmake -S $perfSource -B $perfBinary -G $Generator -A x64
if ($LASTEXITCODE -ne 0) { throw 'Baseline configure failed' }
cmake --build $perfBinary --config Release --target WinAudioPerformanceGui HookAudioProbe WinAudioHook -j 8
if ($LASTEXITCODE -ne 0) { throw 'Baseline build failed' }
Write-Output (Join-Path $perfBinary 'bin/Release/WinAudioPerformanceGui.exe')
