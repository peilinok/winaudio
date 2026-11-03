"""Verify the #263 measurement contract on retained raw data, one run at a time."""
import argparse
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path
import re

from summarize import distribution, summarize

KINDS = ('switch', 'configure', 'filter', 'scroll', 'details', 'clear', 'export-start')
REQUIRED = {
    'pressure': ('result.json', 'samples.csv', 'windows.jsonl'),
    'stall': ('result.json', 'samples.csv', 'windows.jsonl'),
    'overload': ('result.json', 'samples.csv', 'windows.jsonl', 'frames.txt', 'fixture-events.csv', 'gui-ready.json',
                 'fixture-final-windows.jsonl', 'fixture-final-summary.jsonl'),
    'memory': ('result.json', 'samples.csv', 'windows.jsonl', 'frames.txt', 'fixture-events.csv', 'gui-ready.json',
               'fixture-final-windows.jsonl', 'fixture-final-summary.jsonl'),
    'gui': ('frames.txt', 'fixture-events.csv', 'fixture-input.jsonl', 'gui-ready.json',
            'fixture-final-windows.jsonl', 'fixture-final-summary.jsonl'),
    'manual': ('frames.txt', 'fixture-events.csv', 'fixture-input.jsonl'),
    'micro': ('micro-samples.csv',),
    'audio': ('audio-samples.csv', 'audio-summary.csv', 'audio-boundaries.csv'),
}


def stable_differences(groups, states, fields, baseline):
    """Report state costs separately from a version-to-version regression."""
    report = []
    for target in sorted({key[0] for key in groups}):
        for state in states:
            for field in fields:
                before = [groups[target, baseline, repeat][field] for repeat in (1, 2, 3)]
                after = [groups[target, state, repeat][field] for repeat in (1, 2, 3)]
                paired = all(a > b for a, b in zip(after, before))
                if paired and min(after) > max(before):
                    report.append(dict(target=target, state=state, metric=field,
                        baseline=before, observed=after, paired_worse=True, ranges_overlap=False))
    return report


def verify_micro(directory, command):
    stdout = (directory / 'probe.stdout.txt').read_text(encoding='utf-8-sig')
    frequency = int(re.search(r'QPC frequency=(\d+)', stdout).group(1))
    if not re.search(r'batch=300000; samples=20000', stdout) or 'warmup=10000 repeats=3 observer=compiled-out' not in stdout:
        return dict(success=False, errors=['microbenchmark fixed sample parameters or timing build missing'])
    metadata = [dict(word.split('=', 1) for word in line.split()[2:]) for line in stdout.splitlines() if line.startswith('# state_metadata ')]
    expected_masks = {'direct': 33554431, 'installed-mask0': 0, 'default': 9487231, 'high-frequency': 33554431}
    if len(metadata) != 48 or any(int(row['mask']) != expected_masks.get(row['state']) or
        int(row['direct_original']) != (row['state'] == 'direct') or row['capture'] != '1' or row['mapped'] != '1' for row in metadata):
        return dict(success=False, errors=['microbenchmark actual four-state mapping/mask metadata incomplete'])
    header = 'repeat,entry,state,batch_ns,p50_ns,p95_ns,p99_ns,calls,mean_ns,max_ns,minimum_ns'
    summaries = list(csv.DictReader([header] + [line for line in stdout.splitlines() if re.match(r'^\d,\d,', line)]))
    groups = {(int(row['entry']), row['state'], int(row['repeat'])): row for row in summaries}
    states = ('direct', 'installed-mask0', 'default', 'high-frequency')
    expected = {(entry, state, repeat) for entry in range(4) for state in states for repeat in (1, 2, 3)}
    errors = []
    if set(groups) != expected or len(summaries) != len(expected):
        return dict(success=False, errors=['microbenchmark summary groups are incomplete or duplicated'])
    raw = defaultdict(list)
    for row in csv.DictReader((directory / 'micro-samples.csv').open(encoding='utf-8-sig')):
        key = int(row['entry']), row['state'], int(row['repeat'])
        if key not in expected or int(row['sample']) != len(raw[key]) or int(row['ticks']) < 0:
            errors.append('invalid microbenchmark raw identity, sequence or ticks')
            break
        raw[key].append(int(row['ticks']))
    numeric = {}
    for key, summary in groups.items():
        values = [tick * 1e9 / frequency for tick in raw[key]]
        measured = distribution(values)
        numeric[key] = {name: float(summary[name]) for name in ('batch_ns', 'mean_ns', 'p50_ns', 'p95_ns', 'p99_ns')}
        if len(values) != 20000 or int(summary['calls']) != 330000:
            errors.append('microbenchmark raw sample or original-call count mismatch: ' + str(key))
        for output, calculated in (('mean_ns', 'mean'), ('p50_ns', 'p50'), ('p95_ns', 'p95'),
                                    ('p99_ns', 'p99'), ('max_ns', 'maximum'), ('minimum_ns', 'minimum')):
            if measured[calculated] is None or abs(float(summary[output]) - measured[calculated]) > .00051:
                errors.append('microbenchmark raw distribution differs from summary: ' + str(key) + ' ' + output)
    return dict(success=not errors, errors=errors, groups=len(groups), raw_samples=sum(map(len, raw.values())),
        qpc_frequency=frequency, qpc_resolution_ns=1e9 / frequency,
        stable_state_differences=stable_differences(numeric, states[1:], ('batch_ns', 'mean_ns', 'p95_ns'), 'direct'),
        interpretation='Four states quantify inherent observation costs in the same current production Hook. '
        'State differences are not a version regression; no Hook/audio production hot-path source changed from the fixed base.')


def verify_audio(directory, command):
    states = ('unattached', 'installed-mask0', 'default', 'high-frequency')
    summaries = list(csv.DictReader((directory / 'audio-summary.csv').open(encoding='utf-8-sig')))
    boundaries = list(csv.DictReader((directory / 'audio-boundaries.csv').open(encoding='utf-8-sig')))
    expected = {(target, state, repeat) for target in range(2) for state in states for repeat in (1, 2, 3)}
    groups = {(int(row['target']), row['state'], int(row['repeat'])): row for row in summaries}
    times = {(row['state'], int(row['repeat'])): row for row in boundaries}
    errors = []
    if len(groups) != 24 or set(groups) != expected or len(summaries) != 24 or len(times) != 12 or len(boundaries) != 12:
        return dict(success=False, errors=['audio 24 target/state/repeat groups or 12 boundaries missing or duplicated'])
    raw, crossings = defaultdict(list), Counter()
    for row in csv.DictReader((directory / 'audio-samples.csv').open(encoding='utf-8-sig')):
        key = int(row['target']), row['state'], int(row['repeat'])
        if key not in groups:
            errors.append('unexpected audio raw identity')
            break
        boundary = times[key[1], key[2]]
        start, end, tick = int(boundary['formal_start']), int(boundary['formal_end']), int(row['start_qpc'])
        ticks = int(row['ticks'])
        if int(row['sample']) != len(raw[key]) or int(row['pid']) != int(groups[key]['pid']) or not start <= tick < end or ticks < 0:
            errors.append('audio raw sample identity, sequence or boundary mismatch: ' + str(key))
            break
        if raw[key] and tick <= raw[key][-1][0]:
            errors.append('nonmonotonic audio sample starts')
        raw[key].append((tick, ticks))
        crossings[key] += tick + ticks > end
    numeric = {}
    for key, row in groups.items():
        boundary = times[key[1], key[2]]
        frequency = int(boundary['qpc_frequency'])
        if frequency != command['qpc_frequency'] or int(boundary['formal_start']) - int(boundary['warmup_start']) < command['warmup'] * frequency or int(boundary['formal_end']) - int(boundary['formal_start']) != command['duration'] * frequency:
            errors.append('audio fixed time boundaries mismatch: ' + str(key))
        values = [ticks * 1e6 / frequency for _, ticks in raw[key]]
        measured = distribution(values)
        numeric[key] = {name: float(row[name]) for name in ('mean_us', 'p50_us', 'p95_us', 'p99_us')}
        if len(values) != int(row['samples']) or int(row['callbacks']) != len(values) or not values or any(int(row[name]) for name in ('failures', 'overflow', 'exit')) or int(row['original_calls']) < 2 * len(values) or not int(row['captured_frames']):
            errors.append('audio samples, original calls, frames, failures, overflow or target exit mismatch: ' + str(key))
        if raw[key] and (raw[key][0][0] - int(boundary['formal_start']) > frequency or
            int(boundary['formal_end']) - sum(raw[key][-1]) > frequency):
            errors.append('audio raw samples do not reach both formal boundaries: ' + str(key))
        # Each loop owns the bucket containing its start; in-flight tails remain
        # included. The unmeasured sleep/control work is stated in the probe.
        for output, calculated in (('mean_us', 'mean'), ('p50_us', 'p50'), ('p95_us', 'p95'),
                                    ('p99_us', 'p99'), ('max_us', 'maximum'), ('min_us', 'minimum')):
            if measured[calculated] is None or not math.isclose(float(row[output]), measured[calculated], rel_tol=1e-5, abs_tol=.000051):
                errors.append('audio raw distribution differs from summary: ' + str(key) + ' ' + output)
    discontinuities, new_discontinuities = [], []
    for target in range(2):
        for state in states[1:]:
            before = [int(groups[target, 'unattached', repeat]['discontinuities']) for repeat in (1, 2, 3)]
            after = [int(groups[target, state, repeat]['discontinuities']) for repeat in (1, 2, 3)]
            increased = [repeat for repeat, (a, b) in enumerate(zip(after, before), 1) if a > b]
            if increased:
                new_discontinuities.append(dict(target=target, state=state, baseline=before,
                    observed=after, increased_repeats=increased))
            if len(increased) >= 2:
                discontinuities.append(dict(target=target, state=state, baseline=before, observed=after))
    if discontinuities:
        errors.append('repeated new formal audio discontinuities require investigation and rerun')
    stdout = (directory / 'probe.stdout.txt').read_text(encoding='utf-8-sig')
    target_lines = re.findall(r'^audio_target (.*)$', stdout, re.M)
    if len(target_lines) != 6:
        errors.append('missing audio device/format/identity metadata')
    else:
        formats = [re.sub(r'repeat=\d+ target=\d+ pid=\d+ created=\d+ ', '', line) for line in target_lines]
        if len(set(formats)) != 1:
            errors.append('audio device or full format changed between paired targets/repeats')
        for line in target_lines:
            info = dict(word.split('=', 1) for word in line.split())
            identity = int(info['pid']), int(info['created'])
            if any((int(groups[int(info['target']), state, int(info['repeat'])]['pid']),
                    int(groups[int(info['target']), state, int(info['repeat'])]['created_filetime'])) != identity for state in states):
                errors.append('audio paired target identity changed across states or metadata')
    return dict(success=not errors, errors=errors, groups=len(groups), raw_samples=sum(map(len, raw.values())),
        boundary_crossing_samples={str(key): value for key, value in crossings.items()},
        new_formal_discontinuities=new_discontinuities,
        repeated_new_discontinuities=discontinuities,
        stable_state_differences=stable_differences(numeric, states[1:], ('mean_us', 'p95_us', 'p99_us'), 'unattached'),
        interpretation='Loop start QPC owns the formal sample; crossing-end samples are retained. '
        'Loop timing excludes periodic controls and Sleep. Silent render does not prove audible content quality.')


def check_source_window(rows, target, index, capacity, allow_loss):
    errors, source_rows = [], []
    total = target['originals']
    for row in rows:
        if (row['pid'], row['processCreated'], row['observationGeneration']) != (target['pid'], target['created_filetime'], 1):
            errors.append('source window process identity or generation mismatch')
        pattern = r'hz=(\d+)' if index == 0 else r'frames=(\d+) flags=0x2'
        matched = re.fullmatch(pattern, row['args'])
        source_row = int(matched.group(1)) - (100000 if index == 0 else 0) if matched else 0
        if (row['iface'], row['method'], row['streamId']) != (('IAudioClock', 'GetFrequency', 4096) if index == 0 else ('IAudioRenderClient', 'ReleaseBuffer', 8192)):
            errors.append('source window method, interface or original stream identity mismatch')
        if not 1 <= source_row <= total or row['hresult'] != (1 if source_row % 7 == 0 else 0):
            errors.append('source window original result or independently encoded row mismatch')
        source_rows.append(source_row)
    if len(rows) != 1024 or len(set(source_rows)) != len(source_rows):
        errors.append('source window size or unique source rows mismatch')
    if not source_rows or max(source_rows) <= total - capacity:
        errors.append('window does not reach the final independently completed source calls')
    if not allow_loss and source_rows:
        # The production reader scans 64 physical slots; receipt order is not
        # global source order. Bound the edge by that real capacity and require
        # the complete final scan, rather than silently sorting the host window.
        if min(source_rows) < total - 1024 - capacity + 1 or not set(range(total - capacity + 1, total + 1)).issubset(source_rows):
            errors.append('loss-free window misses latest independent source scan')
    return dict(success=not errors, errors=errors, minimum_source_row=min(source_rows) if source_rows else None,
        maximum_source_row=max(source_rows) if source_rows else None, source_originals=total,
        source_order_edge_difference=len(set(range(max(1, total - 1023), total + 1)) - set(source_rows)))


def verify_source(directory, command, result):
    errors, windows = [], []
    targets = result['targets']
    if len(targets) != 2 or result['warmup_seconds'] != command['warmup'] or result['formal_seconds'] != command['duration'] or result['qpc_frequency'] != command['qpc_frequency']:
        errors.append('source fixed parameters or two target identities mismatch')
    rows = list(csv.DictReader((directory / 'samples.csv').open(encoding='utf-8-sig')))
    raw_windows = [json.loads(line) for line in (directory / 'windows.jsonl').open(encoding='utf-8')]
    if command['kind'] in ('memory', 'overload'):
        raw_windows = [json.loads(line) for line in (directory / 'fixture-final-windows.jsonl').open(encoding='utf-8')]
    for i, target in enumerate(targets):
        actual_exits = {(item['pid'], item['created_filetime']) for item in command['observed_process_exits'] if not item['error'] and item['actual_exit'] == 0}
        if (target['pid'], target['created_filetime']) not in actual_exits:
            errors.append('source target has no retained successful actual process exit')
        minimum = (100 if i else 20000) if command['kind'] in ('overload', 'stall') else 2000
        if target['actual_rate'] < minimum or target['formal_elapsed_s'] < command['duration'] or target['formal_controls'] != (0 if command['kind'] == 'memory' else command['duration']) or target['exit'] or target['invalid'] or target['source_completed'] != target['originals'] or target['source_failed'] or not target['source_known']:
            errors.append('source measured rates, original counts, controls, identity or exit failed: ' + str(target['pid']))
        samples = [row for row in rows if (int(row['pid']), int(row['created_filetime'])) == (target['pid'], target['created_filetime'])]
        if not samples or any(int(b['originals']) < int(a['originals']) or int(b['controls']) < int(a['controls']) for a, b in zip(samples, samples[1:])):
            errors.append('source samples missing or nonmonotonic')
        formal = [row for row in samples if command['warmup'] <= float(row['elapsed_s']) < command['warmup'] + command['duration']]
        bins = {int(float(row['elapsed_s']) - command['warmup']) for row in formal}
        if bins != set(range(command['duration'])) or max(((int(b['qpc']) - int(a['qpc'])) / result['qpc_frequency'] for a, b in zip(formal, formal[1:])), default=0) > .5:
            errors.append('source samples do not continuously cover formal load')
        if command['kind'] != 'memory':
            changes = [int(b['qpc']) for a, b in zip(formal, formal[1:]) if int(b['controls']) > int(a['controls'])]
            if not changes or max(((b - a) / result['qpc_frequency'] for a, b in zip(changes, changes[1:])), default=0) > 1.4:
                errors.append('source measured control cadence is not one per second')
        if command['kind'] == 'memory' and any(int(row['controls']) for row in samples):
            errors.append('steady source added control calls')
        windows.append(check_source_window([row for row in raw_windows if row['pid'] == target['pid']], target, i,
            result['pump_transport_capacity'], command['kind'] in ('overload', 'stall')))
        errors.extend(windows[-1]['errors'])
    if command['kind'] == 'stall' and (result['stall_end_qpc'] <= result['stall_start_qpc'] or result['stall_source_after'] - result['stall_source_before'] <= result['pump_transport_capacity'] or result['other_received_after'] <= result['other_received_before']):
        errors.append('stall did not cross actual source capacity while the other receiver advanced')
    return dict(success=not errors, errors=errors, windows=windows,
        stall_scope='Independent synthetic receiver stall; concurrent GUI continuity is established by separate GUI overload runs.' if command['kind'] == 'stall' else None)


def check_gui_contract(summary, events, warmup, duration, require_operations=True, smoke=False, expected_frequency=None):
    """The generic frame summary is necessary, but insufficient, for acceptance."""
    errors = []
    indexed = defaultdict(list)
    for row in events:
        indexed[row['event']].append(row)
    for name in ('warmup-start', 'formal-start', 'formal-planned-end', 'formal-end'):
        if len(indexed[name]) != 1:
            errors.append('missing or duplicate ' + name)
    if errors:
        return dict(success=False, errors=errors)
    origin, start, end = (int(indexed[name][0]['value']) for name in
                          ('warmup-start', 'formal-start', 'formal-planned-end'))
    frequency = (start - origin) / warmup if warmup else 0
    if expected_frequency is not None and frequency != expected_frequency:
        errors.append('GUI boundary frequency differs from independent Win32 QPC frequency')
    if not frequency or (end - start) / frequency < duration or int(indexed['formal-end'][0]['qpc']) < end:
        errors.append('independent QPC boundaries do not cover the required warmup and formal duration')
    visible = True
    for row in events:
        tick = int(row['qpc'])
        if row['event'] in ('occluded', 'visible'):
            if tick < start:
                visible = row['event'] == 'visible' and row['id'] == '0'
            elif tick < end and (row['event'] == 'occluded' or row['id'] != '0'):
                errors.append('confirmed occlusion or minimization inside formal interval')
    if not visible:
        errors.append('formal interval began while occluded or minimized')
    if not summary['run_complete'] or summary['run_count'] != 1 or summary['pending_operations_at_end']:
        errors.append('incomplete frame stream or unfinished operations')
    groups = [group for group in summary['groups'] if group['stage'] != 'teardown']
    operations = {}
    if not groups:
        errors.append('no formal GUI frames')
    for group in groups:
        if group['scene'] != 'Pipeline' or not group['frame_budget_met']:
            errors.append('formal scene or main-thread budget failed')
        for kind, values in group['operations_by_target_ms'].items():
            if kind in operations:
                errors.append('operation class split across scenes/stages: ' + kind)
            operations[kind] = values
        if require_operations:
            controls = [values['minimum'] for key, values in group['work_per_frame'].items()
                        if key.startswith('fixture-controls:')]
            if len(controls) != 2 or sum(controls) < 10000:
                errors.append('10000 structured controls were not retained throughout formal frames')
        windows = [values for key, values in group['work_per_frame'].items() if key.startswith('fixture-window:')]
        if len(windows) != 2 or any(values['maximum'] > 1024 for values in windows):
            errors.append('missing independent windows or window capacity exceeded')
    issued = {int(row['id']): row for row in indexed['command-issued']}
    visible_ops = {int(row['id']): row for row in indexed['effect-visible']}
    if len(issued) != len(indexed['command-issued']) or len(visible_ops) != len(indexed['effect-visible']) or issued.keys() != visible_ops.keys():
        errors.append('issued, completed and unique operation identities do not reconcile')
    if sum(values['n'] for values in operations.values()) != len(issued):
        errors.append('frame operations do not reconcile with source command identities')
    if require_operations:
        for kind in KINDS:
            values = operations.get(kind, {})
            if values.get('n', 0) < (1 if smoke else 20) or values.get('p95', 101) > 100:
                errors.append('operation count or budget failed: ' + kind)
    return dict(success=not errors, errors=errors, formal_start=start, formal_end=end,
                qpc_frequency=frequency, formal_seconds=(end - start) / frequency if frequency else None,
                operations=operations, interpretation='Fixture production-command latency includes scheduling, '
                'effect acknowledgement and first submitted frame. It is not mouse-input latency.')


def check_refresh_cadence(steady_milliseconds):
    return bool(steady_milliseconds) and all(b - a >= 100 for a, b in zip(steady_milliseconds, steady_milliseconds[1:]))


def check_receiver_progress(series, start, frequency, seconds):
    bins = defaultdict(list)
    for tick, value in series:
        second = int((tick - start) / frequency)
        if start <= tick < start + seconds * frequency:
            bins[second].append(value)
    return all(second in bins and max(bins[second]) > min(bins[second]) for second in range(seconds))


def verify_gui(directory, command):
    events = list(csv.DictReader((directory / 'fixture-events.csv').open(encoding='utf-8-sig')))
    start = next(int(row['value']) for row in events if row['event'] == 'formal-start')
    end = next(int(row['value']) for row in events if row['event'] == 'formal-planned-end')
    summary = summarize(directory / 'frames.txt', start_tick=start, end_tick=end)
    contract = check_gui_contract(summary, events, command['warmup'], command['duration'],
                                  command['kind'] == 'gui', command['smoke'], command['qpc_frequency'])
    # Raw per-frame gaps remain visible; no invented complete-frame timing gate.
    frame_meta, refreshes, work, operations = [], [], [], []
    for line in (directory / 'frames.txt').open(encoding='utf-8-sig'):
        if line.startswith('kind=frame '):
            row = dict(word.split('=', 1) for word in line.split())
            frame_meta.append(row)
        elif line.startswith('kind=work '):
            row = dict(word.split('=', 1) for word in line.split())
            work.append(row)
            if row['metric'] == 'presentation-refresh':
                refreshes.append(row)
        elif line.startswith('kind=operation '):
            operations.append(dict(word.split('=', 1) for word in line.split()))
    by_id = {row['frame']: row for row in frame_meta}
    frequency = contract.get('qpc_frequency', 1)
    if any(int(row['qpc_freq']) != command['qpc_frequency'] for row in frame_meta):
        contract['errors'].append('raw frame frequency differs from independent QPC frequency')
    indexed_events = {(row['event'], int(row['id'])): row for row in events if row['event'] in
                      ('command-issued', 'command-enqueued', 'effect-visible')}
    for operation in operations:
        if not start <= int(operation['start']) <= int(operation['end']) < end:
            continue
        identity = int(operation['id'])
        related = [indexed_events.get((name, identity)) for name in ('command-issued', 'command-enqueued', 'effect-visible')]
        if any(row is None for row in related):
            contract['errors'].append('raw operation has no complete command lifecycle: ' + str(identity))
            continue
        issued, enqueued, visible = related
        action = int(issued['value'])
        if action >= len(KINDS) or operation['target'] != KINDS[action] or len({row['segment'] for row in related}) != 1 or len({row['value'] for row in related}) != 1 or not int(operation['start']) <= int(issued['qpc']) <= int(enqueued['qpc']) <= int(visible['qpc']) <= int(operation['end']):
            contract['errors'].append('raw operation target, segment or command timestamps differ: ' + str(identity))
    ticks = [int(by_id[row['frame']]['start']) for row in refreshes]
    formal_refresh = [tick for tick in ticks if start <= tick < end]
    refresh_intervals = [(b - a) * 1000 / frequency for a, b in zip(formal_refresh, formal_refresh[1:])]
    cadence = [int(row['value']) for row in work if row['metric'] == 'presentation-refresh-steady-ms' and
               start <= int(by_id[row['frame']]['start']) < end]
    if len(cadence) != len(formal_refresh) or not check_refresh_cadence(cadence):
        contract['errors'].append('periodic presentation cadence exceeded 10 Hz')
    # Preserve actual intervals beside the full-interval refresh rate. Explicit
    # user-command export snapshot handoffs are reported as separate work.
    contract.update(periodic_refresh_count=len(formal_refresh),
                    periodic_refresh_interval_ms=distribution(refresh_intervals),
                    periodic_refresh_gate_interval_ms=distribution([b - a for a, b in zip(cadence, cadence[1:])]),
                    refresh_quantization='Production gate uses integer steady milliseconds; adjacent gate values must differ by at least 100. Frame-start QPC intervals can differ by scheduling and 1 ms quantization.',
                    largest_frame_intervals=[dict(frame=row['frame'], start=row['start'],
                         interval_ms=1000 * int(row['interval_ticks']) / int(row['qpc_freq']))
                         for row in sorted(frame_meta, key=lambda row: int(row['interval_ticks']), reverse=True)[:20]])
    final_rows = [json.loads(line) for line in (directory / 'fixture-final-windows.jsonl').open(encoding='utf-8')]
    by_segment = defaultdict(list)
    for row in final_rows:
        by_segment[row['attachId']].append(row)
    contract['final_windows'] = {str(key): len(rows) for key, rows in by_segment.items()}
    if len(by_segment) != 2 or any(len(rows) != 1024 for rows in by_segment.values()):
        contract['errors'].append('final latest window validation failed')
    ready = json.loads((directory / 'gui-ready.json').read_text(encoding='utf-8'))
    observed = {(item['pid'], item['created_filetime']) for item in command['observed_process_exits'] if not item['error'] and item['actual_exit'] == 0}
    if any((target['pid'], target['created_filetime']) not in observed for target in ready['targets']):
        contract['errors'].append('explicit GUI targets have no retained successful actual process exit')
    if command['kind'] == 'gui':
        inputs = [json.loads(line) for line in (directory / 'fixture-input.jsonl').open(encoding='utf-8')]
        counts = Counter(row['pid'] for row in inputs)
        control_methods = {'Initialize', 'SetEventHandle', 'SetMute', 'SetChannelVolume', 'SetClientProperties',
                           'InitializeSharedAudioStream', 'GetService', 'Start', 'Stop', 'Reset'}
        if len(inputs) != 10000 or counts != Counter({target['pid']: 5000 for target in ready['targets']}) or any(
            row['pump'] or row['method'] not in control_methods or max((arg['depth'] for arg in row.get('parameters', [])), default=0) < 2 for row in inputs):
            contract['errors'].append('initial 10000 controls are missing, not control methods or lack nested structured parameters')
    progress = []
    for target in ready['targets']:
        segment, token = target['segment'], target['token']
        metrics = {}
        for metric, source in (('fixture-window-received', segment), ('fixture-source-completed', segment),
                               ('receiver-pump-upper-sequence', token)):
            series = [(int(by_id[row['frame']]['start']), int(row['value'])) for row in work
                      if row['metric'] == metric and int(row['source']) == source]
            metrics[metric] = check_receiver_progress(series, start, frequency, command['duration'])
        received_bins = {int((int(by_id[row['frame']]['start']) - start) / frequency) for row in work
            if row['metric'] == 'pump-received' and int(row['source']) == token and int(row['value']) > 0
            and start <= int(by_id[row['frame']]['start']) < end}
        metrics['handoff_every_formal_second'] = received_bins == set(range(command['duration']))
        if not all(metrics.values()):
            contract['errors'].append('source/worker/handoff/window did not advance throughout the formal interval: ' + str(target['pid']))
        progress.append(dict(pid=target['pid'], created_filetime=target['created_filetime'], metrics=metrics))
    contract['receiver_progress'] = progress
    summaries = [json.loads(line) for line in (directory / 'fixture-final-summary.jsonl').open(encoding='utf-8')]
    if command['kind'] in ('memory', 'overload'):
        source = json.loads((directory / 'result.json').read_text(encoding='utf-8'))
        for target in source['targets']:
            final = next((row for row in summaries if row['pid'] == target['pid'] and row['processCreated'] == target['created_filetime']), None)
            completed = sum(value for key, value in final.items() if key.startswith('statistics.') and key.endswith('.completed')) if final else -1
            if completed != target['originals'] or not final['statistics.known']:
                contract['errors'].append('final GUI statistics do not reconcile with the independently completed source')
    contract['success'] = not contract['errors']
    return dict(contract=contract, frames=summary)


def verify_memory(directory, command, result):
    rows = [json.loads(line) for line in (directory / 'process.jsonl').open(encoding='utf-8')]
    identities = defaultdict(list)
    expected = {(item['pid'], item['created_filetime']) for item in command['commands']}
    expected.update((item['pid'], item['created_filetime']) for item in result['targets'])
    for row in rows:
        if not row['error'] and (row['pid'], row['created_filetime']) in expected:
            identities[row['pid'], row['created_filetime'], row['executable']].append(row)
    start = result['start_qpc'] + command['warmup'] * result['qpc_frequency']
    duration = command['duration']
    report, errors = [], []
    if len(expected) != 4 or {(pid, created) for pid, created, _ in identities} != expected:
        errors.append('missing exact GUI/owner/two source target PID and creation-time identities')
    if any(row['error'] and row['pid'] in {pid for pid, _ in expected} and start <= row['qpc'] < start + duration * result['qpc_frequency'] for row in rows):
        errors.append('external sampling errors inside the formal interval')
    for (pid, created, executable), samples in identities.items():
        formal = [row for row in samples if 0 <= (row['qpc'] - start) / result['qpc_frequency'] < duration]
        bins = {int((row['qpc'] - start) / result['qpc_frequency']) for row in formal}
        if bins != set(range(duration)):
            errors.append('external memory samples miss actual formal seconds: ' + str(pid))
        if not formal:
            continue
        trends = []
        for first in range(0, duration, 60):
            section = [row for row in formal if first <= (row['qpc'] - start) / result['qpc_frequency'] < first + 60]
            if len(section) < 2:
                continue
            xs = [(row['qpc'] - section[0]['qpc']) / result['qpc_frequency'] for row in section]
            ys = [row['private_bytes'] for row in section]
            mean_x, mean_y = sum(xs) / len(xs), sum(ys) / len(ys)
            slope = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / sum((x - mean_x)**2 for x in xs)
            trends.append(dict(first_formal_second=first, samples=len(section), private_bytes_per_second=slope,
                private_first=ys[0], private_last=ys[-1], private_range=max(ys) - min(ys)))
        report.append(dict(pid=pid, created_filetime=created, executable=executable,
            sampled_seconds=len(bins), sample_count=len(formal),
            max_interval_s=max((b['qpc'] - a['qpc']) / result['qpc_frequency'] for a, b in zip(formal, formal[1:])),
            private_bytes=distribution([row['private_bytes'] for row in formal]),
            working_set=distribution([row['working_set'] for row in formal]),
            private_first=formal[0]['private_bytes'], private_last=formal[-1]['private_bytes'], minute_trends=trends))
    return dict(success=not errors, errors=errors, processes=report,
                interpretation='Private Bytes and Working Set are external process totals. '
                'Window/queue capacities and live records are reported separately; these totals alone do not prove a leak.')


def verify(directory):
    command = json.loads((directory / 'command-result.json').read_text(encoding='utf-8'))
    kind = command['kind']
    errors = []
    output = dict(command_success=command['success'], kind=kind, smoke=command['smoke'], errors=errors)
    if kind not in REQUIRED:
        return dict(output, success=False, errors=['unknown measurement kind'])
    expected = (3, 12) if command['smoke'] else (30, 600 if kind == 'memory' else
        30 if kind in ('overload', 'stall') else 120)
    if kind == 'audio' and command['smoke']:
        expected = (1, 3)
    if kind == 'manual':
        expected = (3, 600)
    if (command['warmup'], command['duration']) != expected:
        errors.append('fixed measurement parameters do not match kind and smoke status')
    required = REQUIRED[kind] + ('process.jsonl',)
    for name in required:
        if not (directory / name).is_file():
            errors.append('missing required artifact: ' + name)
    for item in command['commands']:
        for stream in ('stdout', 'stderr'):
            if not Path(item[stream]).is_file():
                errors.append('missing command output: ' + item[stream])
    if not command.get('exits') or any(command['exits'].values()) or command.get('forced_roots'):
        errors.append('missing successful normal process exits')
    if not command.get('observed_process_exits') or any(item['error'] or item['actual_exit'] != 0 for item in command['observed_process_exits']):
        errors.append('missing successful actual exits for observed owned process instances')
    if kind in ('gui', 'memory', 'overload', 'audio') and len(command.get('loaded_hooks', {})) != 1:
        errors.append('missing unique actual loaded Hook file identity')
    if errors:
        return dict(output, success=False)
    success = command['success']
    if kind == 'manual':
        output['interpretation'] = 'Manual functional evidence; automatic operation counts and performance budgets do not apply.'
    elif kind in ('gui', 'overload', 'memory'):
        output['gui'] = verify_gui(directory, command)
        success = success and output['gui']['contract']['success']
    if kind in ('pressure', 'stall', 'overload', 'memory'):
        output['source'] = json.loads((directory / 'result.json').read_text(encoding='utf-8'))
        success = success and output['source']['success']
        output['source_contract'] = verify_source(directory, command, output['source'])
        success = success and output['source_contract']['success']
        if command['kind'] == 'memory':
            output['memory'] = verify_memory(directory, command, output['source'])
            success = success and output['memory']['success']
    if kind in ('micro', 'audio'):
        output[kind] = verify_micro(directory, command) if kind == 'micro' else verify_audio(directory, command)
        success = success and output[kind]['success']
    output['success'] = success
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    try:
        result = verify(args.directory.resolve())
    except Exception as error:
        result = dict(success=False, parse_error=repr(error))
    (args.directory / 'verification.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['success'] else 2)
