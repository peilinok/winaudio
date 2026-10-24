"""Replay the baseline sink at the same public seam without editing the checkout."""
from pathlib import Path
import argparse
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--core-build', default='build-233')
parser.add_argument('--generator', default='Visual Studio 18 2026')
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
replay = root / '.scratch' / 'wav-dump-legacy-replay'
replay.mkdir(parents=True, exist_ok=True)
base = '3c433575f0bfc77808eaa09113ba1eb2052ffb4e'

def baseline(name):
    return subprocess.check_output(['git', 'show', f'{base}:src/core/{name}'], cwd=root).decode('utf-8')

header = baseline('WavSink.h')
header = header.replace('#include "Result.h"', '#include "Result.h"\n#include "WavSinkOutput.h"')
header = header.replace('    WavSink();', '    explicit WavSink(WavSinkOutputFactory outputFactory = {});')
header = header.replace('    mutable std::mutex          mtx_;', '    mutable std::mutex          mtx_;\n    WavSinkOutputFactory outputFactory_;')
source = baseline('WavSink.cpp')
adapter = '''class FileOutput : public WavSinkOutput {
public:
    Result open(const std::wstring& p, const AudioFormat& f) override { return w.open(p, f); }
    Result setBuffer(size_t b) override { return w.setStdioBuffer(b); }
    Result write(const void* p, size_t b) override { return w.write(p, b); }
    Result close() override { return w.close(); }
private:
    WavWriter w;
};
'''
source = source.replace('#include <vector>', '#include <vector>\n#include "WavSinkTestHooks.h"')
source = source.replace('namespace {\n', 'namespace {\n' + adapter, 1)
source = source.replace('WavSink::WavSink() = default;', 'WavSink::WavSink(WavSinkOutputFactory f) : outputFactory_(std::move(f)) {}')
source = source.replace('    if (state_.load(std::memory_order_acquire) != WavSinkState::Running) return 0;\n    if (!running_.load(std::memory_order_acquire) || !ring_) return 0;', '''    const auto state = state_.load(std::memory_order_acquire);
    const bool running = running_.load(std::memory_order_acquire);
    wavSinkTest::checkpoint(wavSinkTest::Point::AfterSnapshot, this);
    if (state != WavSinkState::Running || !running || !ring_) return 0;
    wavSinkTest::checkpoint(wavSinkTest::Point::AfterAdmission, this);''')
source = source.replace('    WavWriter writer;', '    auto writer = outputFactory_ ? outputFactory_() : std::make_unique<FileOutput>();')
for method in ('open', 'setStdioBuffer', 'write', 'close'):
    source = source.replace('writer.' + method + '(', 'writer->' + ('setBuffer' if method == 'setStdioBuffer' else method) + '(')
source = source.replace('        const bool drain = stop || overflow;', '        const bool drain = stop || overflow;\n        if (drain) wavSinkTest::drainObserved(this, false);')
(replay / 'WavSink.h').write_text(header, encoding='utf-8')
(replay / 'WavSink.cpp').write_text(source, encoding='utf-8')
# The first tests use the original two-argument sink API. Later source-token
# and preparation tests require the fixed interface and are excluded here.
tests = (root / 'src/tests/test_wav_sink_lifecycle.cpp').read_text(encoding='utf-8')
tests = tests[:tests.index('TEST(WavSinkLifecycle, DifferentSourceCannotWriteIntoCurrentFile)')]
(replay / 'test_legacy.cpp').write_text(tests, encoding='utf-8')
core_build = (root / args.core_build).resolve().as_posix()
root_cmake = root.as_posix()
(replay / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.21)
project(WavLegacyReplay LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
include("{root_cmake}/cmake/CompilerWarnings.cmake")
add_executable(WavLegacyReplay WavSink.cpp test_legacy.cpp "{root_cmake}/src/core/RingBuffer.cpp")
target_compile_definitions(WavLegacyReplay PRIVATE UNICODE _UNICODE WA_WAV_SINK_TESTING)
wa_set_project_warnings(WavLegacyReplay)
target_include_directories(WavLegacyReplay PRIVATE "{root_cmake}/src/core" "{root_cmake}/third_party/googletest/googletest/include")
target_link_libraries(WavLegacyReplay PRIVATE "{core_build}/lib/Debug/WinAudioCore.lib" "{core_build}/lib/Debug/gtest_main.lib" "{core_build}/lib/Debug/gtest.lib" ole32 oleaut32 mmdevapi tdh advapi32 shell32)
''', encoding='utf-8')
subprocess.run(['cmake', '-S', str(replay), '-B', str(replay / 'build'), '-G', args.generator, '-A', 'x64'], check=True)
subprocess.run(['cmake', '--build', str(replay / 'build'), '--config', 'Debug', '-j', '4'], check=True)
result = subprocess.run([str(replay / 'build/Debug/WavLegacyReplay.exe'), '--gtest_filter=WavSinkLifecycle.StopWaitsForAdmittedPushAndKeepsItsBytes:WavSinkLifecycle.EmptyRingDoesNotFinalizeBeforeLastPublication:WavSinkLifecycle.OldSnapshotCannotWriteIntoRestartedFile'])
if result.returncode != 1:
    raise SystemExit(f'Expected the baseline regression tests to fail, got {result.returncode}')
print('Confirmed: the original sink fails the deterministic lifecycle regressions.')
