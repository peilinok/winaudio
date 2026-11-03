"""Run the #263 pinned Release probes sequentially; retain stdout, stderr and external process samples."""
import argparse
import csv
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


class ProcessEntry(C.Structure):
    _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('pid', W.DWORD),
                ('heap', C.c_size_t), ('module', W.DWORD), ('threads', W.DWORD),
                ('parent', W.DWORD), ('priority', W.LONG), ('flags', W.DWORD), ('exe', W.WCHAR * 260)]


class MemoryCounters(C.Structure):
    _fields_ = [('size', W.DWORD), ('faults', W.DWORD)] + [
        (name, C.c_size_t) for name in ('peak_ws', 'working_set', 'peak_pool', 'pool',
                                       'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile', 'private_bytes')]


kernel = C.WinDLL('kernel32', use_last_error=True)
psapi = C.WinDLL('psapi', use_last_error=True)
kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = [W.DWORD, W.BOOL, W.DWORD], W.HANDLE
kernel.CloseHandle.argtypes = [W.HANDLE]
kernel.CreateToolhelp32Snapshot.argtypes, kernel.CreateToolhelp32Snapshot.restype = [W.DWORD, W.DWORD], W.HANDLE
kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [W.HANDLE, C.POINTER(ProcessEntry)]
kernel.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
kernel.GetExitCodeProcess.argtypes = [W.HANDLE, C.POINTER(W.DWORD)]
kernel.QueryPerformanceCounter.argtypes = kernel.QueryPerformanceFrequency.argtypes = [C.POINTER(C.c_longlong)]
psapi.GetProcessMemoryInfo.argtypes = [W.HANDLE, C.POINTER(MemoryCounters), W.DWORD]


def filetime(value):
    return value.dwHighDateTime * 2**32 + value.dwLowDateTime


def processes(roots):
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == W.HANDLE(-1).value:
        raise OSError(C.get_last_error(), 'CreateToolhelp32Snapshot')
    entry = ProcessEntry()
    entry.size = C.sizeof(entry)
    rows = []
    try:
        more = kernel.Process32FirstW(snapshot, C.byref(entry))
        while more:
            rows.append((entry.pid, entry.parent, entry.exe))
            more = kernel.Process32NextW(snapshot, C.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    selected = set(roots)
    for _ in rows:
        added = {pid for pid, parent, _ in rows if parent in selected} - selected
        if not added:
            break
        selected.update(added)
    return [(pid, exe) for pid, _, exe in rows if pid in selected]


def memory_row(pid, executable):
    handle = kernel.OpenProcess(0x410, False, pid)
    if not handle:
        return dict(pid=pid, executable=executable, error=C.get_last_error())
    try:
        created, exited, system, user = (W.FILETIME() for _ in range(4))
        mem = MemoryCounters()
        mem.size = C.sizeof(mem)
        if not kernel.GetProcessTimes(handle, C.byref(created), C.byref(exited), C.byref(system), C.byref(user)):
            raise OSError(C.get_last_error(), 'GetProcessTimes')
        if not psapi.GetProcessMemoryInfo(handle, C.byref(mem), C.sizeof(mem)):
            raise OSError(C.get_last_error(), 'GetProcessMemoryInfo')
        return dict(pid=pid, executable=executable, created_filetime=filetime(created),
                    private_bytes=mem.private_bytes, working_set=mem.working_set,
                    cpu_100ns=filetime(system) + filetime(user), error=0, qpc=qpc())
    finally:
        kernel.CloseHandle(handle)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def qpc():
    value = C.c_longlong()
    if not kernel.QueryPerformanceCounter(C.byref(value)):
        raise OSError(C.get_last_error(), 'QueryPerformanceCounter')
    return value.value


def run(args):
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    bins = root / 'build/bin/Release'
    warmup = 3 if args.smoke else 30
    duration = 12 if args.smoke else (600 if args.kind == 'memory' else 30 if args.kind in ('overload', 'stall') else 120)
    if args.kind == 'manual':
        warmup, duration = 3, 600
    if args.smoke and args.kind == 'audio':
        warmup, duration = 1, 3
    environment = {key: value for key, value in os.environ.items() if not key.startswith('WINAUDIO_PERF_')}
    environment.update(WINAUDIO_FRAME_PROBE='1', WINAUDIO_FRAME_PROBE_FLUSH='0',
                       WINAUDIO_FRAME_PROBE_PATH=str(output / 'frames.txt'),
                       WINAUDIO_PERF_WARMUP=str(warmup), WINAUDIO_PERF_DURATION=str(duration), WINAUDIO_PERF_PUMP='1',
                       WINAUDIO_PERF_OWNER_CANCEL=str(output / 'owner-cancel.flag'))
    commands, owned, streams = [], [], []
    started = time.time()
    def launch(name, command):
        stdout, stderr = output / (name + '.stdout.txt'), output / (name + '.stderr.txt')
        out, err = stdout.open('wb'), stderr.open('wb')
        streams.extend([out, err])
        process = subprocess.Popen([str(item) for item in command], cwd=output, env=environment,
                                   stdout=out, stderr=err, creationflags=0x08000000)
        owned.append((name, process))
        commands.append(dict(name=name, argv=[str(item) for item in command], pid=process.pid,
                             created_filetime=memory_row(process.pid, Path(command[0]).name)['created_filetime'],
                             stdout=str(stdout), stderr=str(stderr)))
        return process
    result = dict(kind=args.kind, smoke=args.smoke, warmup=warmup, duration=duration,
                  started_unix=started, base=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(), commands=commands)
    frequency = C.c_longlong()
    kernel.QueryPerformanceFrequency(C.byref(frequency))
    result['qpc_frequency'] = frequency.value
    result['patch_sha256'] = hashlib.sha256(subprocess.check_output(['git', 'diff', '--binary', 'HEAD'], cwd=root)).hexdigest()
    result['binaries'] = {item.name: digest(item) for item in bins.iterdir() if item.name in
        ('WinAudioPerformanceGui.exe', 'SegmentWindowProbe.exe', 'HookAudioProbe.exe', 'HookBufferProbe.exe', 'WinAudioHook.dll')}
    source_paths = ('src/core/PipelineAttachController.cpp', 'src/core/PipelineAttachController.h',
        'src/gui/AppUi.cpp', 'src/gui/PerformanceFixture.cpp', 'src/gui/PerformanceFixture.h', 'src/gui/main.cpp',
        'src/probe/HookAudioProbe.cpp', 'src/probe/HookAudioTiming.h', 'src/probe/HookBufferProbe.cpp',
        'src/probe/PerformanceCalls.cpp', 'src/probe/SegmentWindowProbe.cpp', 'src/probe/SegmentWindowTiming.h',
        'src/tests/test_performance_calls.cpp', 'src/tests/test_pipeline_attach_ui_model.cpp',
        'scripts/performance/run_pipeline.py', 'scripts/performance/verify_pipeline.py',
        'scripts/performance/summarize.py', 'scripts/performance/test_summarize.py', 'scripts/performance/test_verify_pipeline.py')
    result['source_files'] = {path: digest(root / path) for path in source_paths}
    observed = {}
    try:
        gui = args.kind in ('gui', 'manual', 'overload', 'memory')
        if gui:
            if args.kind in ('gui', 'manual'):
                owner = launch('source', [bins / 'HookAudioProbe.exe', '--continuous', warmup + duration + 20])
                environment['WINAUDIO_PERF_MODE'] = 'pipeline'
            else:
                mode = 'gui-memory' if args.kind == 'memory' else 'gui-overload'
                owner = launch('source', [bins / 'SegmentWindowProbe.exe', '--timed', output, mode, warmup, duration, 0])
                environment.update(WINAUDIO_PERF_MODE='steady' if args.kind == 'memory' else 'pressure',
                                   WINAUDIO_PERF_SOURCE_DIRECTORY=str(output))
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                lines = (output / 'source.stdout.txt').read_text(encoding='utf-8', errors='replace')
                pids = re.findall(r'^ready target=\d+ pid=(\d+) ', lines, re.M)
                if len(pids) == 2:
                    break
                if owner.poll() is not None:
                    raise RuntimeError('source exited before readiness: ' + str(owner.returncode))
                time.sleep(.1)
            else:
                raise RuntimeError('two explicit targets not ready')
            environment['WINAUDIO_PERF_LIVE_PIDS'] = ' '.join(pids)
            if args.kind == 'manual':
                environment['WINAUDIO_PERF_MANUAL'] = '1'
            launch('gui', [bins / 'WinAudioPerformanceGui.exe'])
        elif args.kind in ('pressure', 'stall'):
            launch('probe', [bins / 'SegmentWindowProbe.exe', '--timed', output,
                             'overload' if args.kind == 'stall' else 'regular', warmup,
                             duration, 1 if args.kind == 'stall' else 0])
        elif args.kind == 'micro':
            launch('probe', [bins / 'HookBufferProbe.exe', '--pipeline', output / 'micro-samples.csv'])
        elif args.kind == 'audio':
            launch('probe', [bins / 'HookAudioProbe.exe', '--performance', output, warmup, duration])
        roots = [process.pid for _, process in owned]
        with (output / 'process.jsonl').open('w', encoding='utf-8') as external:
            next_sample = time.monotonic()
            while any(process.poll() is None for _, process in owned):
                if args.kind == 'manual' and any(name == 'gui' and process.poll() is not None for name, process in owned):
                    (output / 'owner-cancel.flag').touch()
                for pid, executable in processes(roots):
                    before = qpc()
                    row = memory_row(pid, executable)
                    row.update(qpc=row.get('qpc', qpc()), sample_start_qpc=before, unix=time.time())
                    if not row['error'] and (pid, row['created_filetime']) not in observed:
                        handle = kernel.OpenProcess(0x1000, False, pid)
                        if handle:
                            observed[pid, row['created_filetime']] = handle
                    external.write(json.dumps(row) + '\n')
                external.flush()
                next_sample += .8
                time.sleep(max(0, next_sample - time.monotonic()))
        result['exits'] = {name: process.wait() for name, process in owned}
        result['success'] = all(code == 0 for code in result['exits'].values())
    except Exception as error:
        result.update(success=False, error=repr(error))
        # Ask the controlled probes to exit normally, so their C++ owners reap
        # the explicit targets. Keep any forced fallback distinct in the receipt.
        (output / 'owner-cancel.flag').touch()
        deadline = time.monotonic() + 20
        while any(process.poll() is None for _, process in owned) and time.monotonic() < deadline:
            time.sleep(.1)
        result['forced_roots'] = []
        for _, process in owned:
            if process.poll() is None:
                result['forced_roots'].append(process.pid)
                process.terminate()
                process.wait(timeout=20)
        result['exits'] = {name: process.poll() for name, process in owned}
    finally:
        for stream in streams:
            stream.close()
        result['ended_unix'] = time.time()
        result['observed_process_exits'] = []
        for (pid, created), handle in observed.items():
            code = W.DWORD(259)
            ok = kernel.GetExitCodeProcess(handle, C.byref(code))
            result['observed_process_exits'].append(dict(pid=pid, created_filetime=created,
                actual_exit=code.value if ok else None, error=0 if ok else C.get_last_error()))
            kernel.CloseHandle(handle)
            if not ok or code.value != 0:
                result['success'] = False
        ready = output / 'gui-ready.json'
        if ready.is_file():
            result['ready'] = json.loads(ready.read_text(encoding='utf-8'))
            result['loaded_hooks'] = {item['hook_path']: digest(Path(item['hook_path'])) for item in result['ready']['targets']}
        elif args.kind == 'audio':
            stdout = (output / 'probe.stdout.txt').read_text(encoding='utf-8', errors='strict')
            result['loaded_hooks'] = {path: digest(Path(path)) for path in
                re.findall(r'^loaded pid=\d+ path=(.*?) layout=', stdout, re.M)}
        for item in commands:
            item['stdout_sha256'], item['stderr_sha256'] = digest(Path(item['stdout'])), digest(Path(item['stderr']))
        (output / 'command-result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2), flush=True)
    return 0 if result['success'] else 2


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=['pressure', 'overload', 'stall', 'memory', 'gui', 'manual', 'micro', 'audio'])
    parser.add_argument('output', type=Path)
    parser.add_argument('--smoke', action='store_true')
    raise SystemExit(run(parser.parse_args()))
