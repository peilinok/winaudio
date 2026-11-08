"""Summarize all recorded runs; retain CPU outliers and same-process controls."""
import argparse
import csv
import json
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('evidence', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)

def read(path):
    return json.loads(path.read_text(encoding='utf-8'))

def summary(folder):
    data = read(folder / 'summary.json')
    assert data['run_complete'] and data['pending_operations_at_end'] == 0, folder
    return data

rows = []
pattern = re.compile(r'^(baseline|optimized)-(main|original)-(0|5000|empty|full|search)-([12])$')
for folder in sorted(args.evidence.iterdir()):
    match = pattern.match(folder.name)
    if not match:
        continue
    variant, workspace, case, repeat = match.groups()
    result = read(folder / 'result.json')
    assert result['exit_code'] == 0, folder
    data = summary(folder)
    groups = [group for group in data['groups'] if group['stage'] == 'idle']
    assert len(groups) == 1, folder
    group = groups[0]
    row = dict(variant=variant, workspace=workspace, case=case, repeat=int(repeat),
               cpu_one_core_percent=result['cpu_one_core_percent'],
               private_peak_mib=result['private_peak_mib'], frames=group['frame_count'],
               main_mean_ms=group['main_thread_ms']['mean'], main_p95_ms=group['main_thread_ms']['p95'],
               main_p99_ms=group['main_thread_ms']['p99'], main_max_ms=group['main_thread_ms']['maximum'],
               exe_sha256=result['exe_sha256'], trace_sha256=data['sha256'])
    for key, value in group['work_per_frame'].items():
        if key.startswith('log-'):
            name = key.split(':')[0]
            for stat in ['mean', 'p95', 'maximum']:
                row[name + '_' + stat] = value[stat]
    rows.append(row)
assert len(rows) == 20, len(rows)

controls = []
for workspace in ['main', 'original']:
    folder = args.evidence / ('alternate-' + workspace)
    result = read(folder / 'result.json')
    samples = read(folder / 'process.json')
    data = summary(folder)
    stages = list(csv.reader((folder / 'log-stages.csv').read_text().splitlines()))
    assert [int(row[0]) for row in stages] == [0, 1, 2, 3]
    frequency = result['qpc_frequency']
    phases = []
    for index, (_, tick) in enumerate(stages):
        start = int(tick)
        end = int(stages[index + 1][1]) if index < 3 else start + 20 * frequency
        good = [sample for sample in samples if not sample['error'] and
                start + frequency <= sample['qpc'] < end - frequency]
        assert len(good) >= 20
        first, last = good[0], good[-1]
        seconds = (last['qpc'] - first['qpc']) / frequency
        name = 'log-{}-{}'.format('full' if index % 2 else 'empty', index // 2 + 1)
        group = next(group for group in data['groups'] if group['stage'] == name)
        phases.append(dict(stage=name, sampled_s=seconds,
                           cpu_one_core_percent=(last['cpu_100ns'] - first['cpu_100ns']) / 1e5 / seconds,
                           cycles_per_second=(last['cycles'] - first['cycles']) / seconds,
                           main_mean_ms=group['main_thread_ms']['mean'],
                           main_p95_ms=group['main_thread_ms']['p95'],
                           work=group['work_per_frame']))
    deltas = [phases[index + 1]['cpu_one_core_percent'] - phases[index]['cpu_one_core_percent']
              for index in [0, 2]]
    controls.append(dict(workspace=workspace, phases=phases, extra_cpu_points=deltas,
                         mean_extra_cpu_points=sum(deltas) / len(deltas),
                         extra_cpu_target_met=all(delta <= 5 for delta in deltas),
                         exe_sha256=result['exe_sha256'], trace_sha256=data['sha256']))

output = dict(independent_runs=rows, same_process_controls=controls)
(args.output / 'metrics.json').write_text(json.dumps(output, indent=2) + '\n', encoding='utf-8')
columns = list(dict.fromkeys(key for row in rows for key in row))
with (args.output / 'gui-summary.csv').open('w', newline='', encoding='utf-8') as stream:
    writer = csv.DictWriter(stream, fieldnames=columns)
    writer.writeheader()
    writer.writerows(rows)
for control in controls:
    print(control['workspace'], 'extra CPU points:', control['extra_cpu_points'],
          'target met:', control['extra_cpu_target_met'])
for row in rows:
    print(row['variant'], row['workspace'], row['case'], row['repeat'],
          'CPU={:.2f} p95={:.4f}'.format(row['cpu_one_core_percent'], row['main_p95_ms']))
