"""Measure the production GUI with fixed ordinary-log input, one run at a time."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.dont_write_bytecode = True

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--exe', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--lines', type=int, default=5000)
parser.add_argument('--search', default='')
parser.add_argument('--warmup', type=int, default=10)
parser.add_argument('--duration', type=int, default=20)
parser.add_argument('--screenshot', action='store_true')
parser.add_argument('--alternate', action='store_true', help='Four 20-second empty/full phases in one process')
args = parser.parse_args()
if args.alternate:
    args.duration = 80
sys.path.insert(0, str(args.root / 'scripts/performance'))
from run_pipeline import memory_row
from summarize import summarize

out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
exe = args.exe.resolve()
env = {key: value for key, value in os.environ.items() if not key.startswith('WINAUDIO_')}
env.update(WINAUDIO_FRAME_PROBE='1', WINAUDIO_FRAME_PROBE_FLUSH='0',
           WINAUDIO_FRAME_PROBE_PATH=str(out / 'frames.txt'), WINAUDIO_PERF_MODE='idle',
           WINAUDIO_PERF_RECORDS='1000', WINAUDIO_PERF_PIDS='2',
           WINAUDIO_PERF_WARMUP=str(args.warmup), WINAUDIO_PERF_DURATION=str(args.duration),
           WINAUDIO_LOG_PANEL_LINES=str(args.lines), WINAUDIO_LOG_PANEL_SEARCH=args.search)
if args.alternate:
    env['WINAUDIO_LOG_PANEL_ALTERNATE'] = '1'
kernel = C.WinDLL('kernel32', use_last_error=True)
kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
kernel.OpenProcess.restype = W.HANDLE
kernel.QueryProcessCycleTime.argtypes = [W.HANDLE, C.POINTER(C.c_ulonglong)]
kernel.QueryProcessCycleTime.restype = W.BOOL
kernel.CloseHandle.argtypes = [W.HANDLE]
kernel.CloseHandle.restype = W.BOOL
kernel.QueryPerformanceFrequency.argtypes = [C.POINTER(C.c_longlong)]
kernel.QueryPerformanceFrequency.restype = W.BOOL
frequency = C.c_longlong()
if not kernel.QueryPerformanceFrequency(C.byref(frequency)):
    raise OSError(C.get_last_error(), 'QueryPerformanceFrequency')
samples = []
captured = False
started = time.perf_counter()
with (out / 'stdout.txt').open('wb') as stdout, (out / 'stderr.txt').open('wb') as stderr:
    process = subprocess.Popen([str(exe)], cwd=out, env=env, stdout=stdout, stderr=stderr,
                               creationflags=0x08000000)
    process_handle = kernel.OpenProcess(0x1000, False, process.pid)
    if not process_handle:
        process.terminate()
        raise OSError(C.get_last_error(), 'OpenProcess')
    while process.poll() is None:
        row = memory_row(process.pid, exe.name)
        cycles = C.c_ulonglong()
        if not kernel.QueryProcessCycleTime(process_handle, C.byref(cycles)):
            raise OSError(C.get_last_error(), 'QueryProcessCycleTime')
        row['cycles'] = cycles.value
        row['elapsed_s'] = time.perf_counter() - started
        samples.append(row)
        if args.screenshot and not captured and row['elapsed_s'] >= 2:
            subprocess.run(['powershell', '-NoProfile', '-File', str(Path(__file__).with_name('capture.ps1')),
                            '-CapturePid', str(process.pid), '-OutputPath', str(out / 'log-panel.png')], check=True)
            captured = True
        if row['elapsed_s'] > args.warmup + args.duration + 40:
            process.terminate()
            raise RuntimeError('GUI measurement timed out')
        time.sleep(.25)
    exit_code = process.wait(timeout=10)
    kernel.CloseHandle(process_handle)
result = dict(exit_code=exit_code, wall_s=time.perf_counter() - started,
              qpc_frequency=frequency.value,
              lines=args.lines, search=args.search,
              environment={key: value for key, value in env.items() if key.startswith('WINAUDIO_')},
              exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
good = [row for row in samples if not row['error'] and row['elapsed_s'] >= args.warmup + 1]
if len(good) < 2:
    raise RuntimeError('Insufficient process samples')
a, b = good[0], good[-1]
result.update(cpu_one_core_percent=(b['cpu_100ns'] - a['cpu_100ns']) / 1e5 /
              (b['elapsed_s'] - a['elapsed_s']),
              cycles_per_second=(b['cycles'] - a['cycles']) / (b['elapsed_s'] - a['elapsed_s']),
              private_peak_mib=max(row['private_bytes'] for row in good) / 2**20)
(out / 'process.json').write_text(json.dumps(samples, indent=2), encoding='utf-8')
(out / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
if exit_code != 0:
    raise RuntimeError('GUI failed: {}'.format(exit_code))
summary = summarize(out / 'frames.txt')
(out / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
print(json.dumps(dict(name=out.name, cpu=result['cpu_one_core_percent'],
                     private_peak_mib=result['private_peak_mib'], exit_code=exit_code)))
