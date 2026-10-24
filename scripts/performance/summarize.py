"""Summarize schema-2 FrameProbe records without pooling runs or stages."""
import argparse
import hashlib
import json
import math
from collections import defaultdict
from pathlib import Path


SLICES = ['heatmap-submit', 'waveform-envelope', 'log-drain', 'present',
          'main-thread', 'ui-build', 'dx11-submit', 'probe-io', 'chart-refresh',
          'fft', 'heatmap-fill', 'heatmap-upload', 'call-receive', 'call-sort',
          'call-match', 'call-rows', 'graph-hint', 'export-start', 'spec-history']


def nearest_rank(values, percentile):
    if not values:
        return None
    return sorted(values)[max(0, math.ceil(percentile * len(values)) - 1)]


def distribution(values):
    return dict(n=len(values), p50=nearest_rank(values, .5),
                p95=nearest_rank(values, .95), p99=nearest_rank(values, .99),
                maximum=max(values) if values else None)


def summarize(path, warmup_seconds=0, duration_seconds=None, phases=None):
    data = Path(path).read_bytes()
    runs = [[]]
    completed_frame = None
    for line in data.decode('utf-8-sig').splitlines():
        if line.startswith('# WinAudio frame probe') and runs[-1]:
            runs.append([])
            completed_frame = None
        elif line.startswith('# End frame probe '):
            fields = dict(word.split('=', 1) for word in line.split() if '=' in word)
            completed_frame = int(fields['frame']) if fields.get('complete') == '1' else None
        elif line.strip() and not line.startswith('#'):
            fields = dict(word.split('=', 1) for word in line.split() if '=' in word)
            runs[-1].append(fields)
    records = runs[-1]
    frames = {}
    slices = defaultdict(dict)
    operations = defaultdict(list)
    work = defaultdict(list)
    for record in records:
        if 'frame' not in record or not record['frame'].isdigit() or int(record['frame']) <= 0:
            raise ValueError('unparseable or truncated record')
        frame = int(record['frame'])
        if record.get('kind') == 'frame':
            if frame in frames:
                raise ValueError('duplicate frame metadata: {}'.format(frame))
            frames[frame] = record
        elif 'slice' in record:
            if record['slice'] in slices[frame]:
                raise ValueError('duplicate slice in frame {}'.format(frame))
            slices[frame][record['slice']] = record
        elif record.get('kind') == 'operation':
            operations[frame].append(record)
        elif record.get('kind') == 'work':
            work[frame].append(record)
    if not frames or (set(slices) | set(operations) | set(work)) - set(frames):
        raise ValueError('missing frame metadata or truncated run')
    ids = sorted(frames)
    if ids != list(range(ids[0], ids[-1] + 1)):
        raise ValueError('frame sequence has gaps')
    first_tick = int(frames[ids[0]]['start'])
    groups = {}
    for frame in ids:
        meta = frames[frame]
        if meta.get('schema') != '2':
            raise ValueError('unsupported frame schema')
        for field in ['lost_slices', 'lost_operations', 'lost_work', 'lost_frames']:
            if int(meta[field]):
                raise ValueError('lost samples: frame={} {}'.format(frame, field))
        mask = 0
        for name, record in slices[frame].items():
            if name not in SLICES or int(record.get('lost', 0)):
                raise ValueError('unknown or lost slice')
            mask |= 1 << SLICES.index(name)
        if mask != int(meta['valid_mask']) or 'main-thread' not in slices[frame]:
            raise ValueError('slice mask mismatch or missing main-thread bracket')
        if int(slices[frame]['main-thread']['N']) != 1:
            raise ValueError('expected one main-thread bracket per frame')
        frequency = int(meta['qpc_freq'])
        if frequency <= 0:
            raise ValueError('missing clock frequency')
        for record in slices[frame].values():
            if int(record['qpc_freq']) != frequency or int(record['ticks']) < 0 or int(record['N']) <= 0:
                raise ValueError('invalid slice clock, ticks or hit count')
        for record in operations[frame]:
            start, end = int(record['start']), int(record['end'])
            submitted = int(meta['start']) + int(slices[frame]['main-thread']['ticks'])
            if (end < start or end != submitted or int(record['ticks']) != end - start or
                    int(record['qpc_freq']) != frequency):
                raise ValueError('operation response does not match the submitted frame')
        elapsed = (int(meta['start']) - first_tick) / frequency - warmup_seconds
        if elapsed < 0 or (duration_seconds is not None and elapsed >= duration_seconds):
            continue
        stage = meta['stage']
        if phases:
            phase = int(elapsed // 30)
            if phase >= len(phases):
                continue
            stage = phases[phase]
        key = (meta['scene'], stage)
        if key not in groups:
            groups[key] = dict(scene=key[0], stage=key[1], frames=[], intervals=[],
                               operations=[], operations_by_target=defaultdict(list),
                               slices=defaultdict(list), work=defaultdict(int), work_frames=[])
        group = groups[key]
        group['frames'].append(frame)
        interval = int(meta['interval_ticks'])
        if interval:
            group['intervals'].append(1000 * interval / frequency)
        for name in SLICES:
            record = slices[frame].get(name)
            group['slices'][name].append(1000 * int(record['ticks']) / frequency if record else 0)
        for record in operations[frame]:
            latency = 1000 * (int(record['end']) - int(record['start'])) / frequency
            group['operations'].append(latency)
            group['operations_by_target'][record['target']].append(latency)
        frame_work = defaultdict(int)
        for record in work[frame]:
            key = '{}:{}:{}:{}'.format(record['metric'], record['source'],
                                       record['direction'], record['channel'])
            group['work'][key] += int(record['value'])
            frame_work[key] += int(record['value'])
        group['work_frames'].append(frame_work)
    if not groups:
        raise ValueError('no samples in selected interval')
    output = []
    for group in groups.values():
        stats = {name: distribution(values) for name, values in group['slices'].items()}
        main = stats['main-thread']
        response = distribution(group['operations'])
        output.append(dict(scene=group['scene'], stage=group['stage'],
                           frame_count=len(group['frames']), first_frame=group['frames'][0],
                           last_frame=group['frames'][-1], main_thread_ms=main,
                           frame_interval_ms=distribution(group['intervals']),
                           operation_response_ms=response, slices_ms=stats, work=dict(group['work']),
                           operations_by_target_ms={target: distribution(values)
                                                    for target, values in group['operations_by_target'].items()},
                           work_per_frame={key: distribution([f.get(key, 0) for f in group['work_frames']])
                                           for key in group['work']},
                           frame_budget_met=main['p95'] <= 8 and main['p99'] <= 16,
                           operation_budget_met=response['p95'] <= 100 if response['n'] else None))
    return dict(source=str(Path(path).resolve()), sha256=hashlib.sha256(data).hexdigest(),
                run_count=len(runs), selected_run=len(runs), groups=output,
                run_complete=completed_frame == ids[-1],
                pending_operations_at_end=int(frames[ids[-1]]['pending_operations']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--warmup-seconds', type=float, default=30)
    parser.add_argument('--duration-seconds', type=float, default=120)
    parser.add_argument('--phases', help='comma-separated names for consecutive 30-second phases')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--require-budget', action='store_true')
    parser.add_argument('--require-complete', action='store_true')
    args = parser.parse_args()
    result = summarize(args.source, args.warmup_seconds, args.duration_seconds,
                       args.phases.split(',') if args.phases else None)
    text = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text + '\n', encoding='utf-8')
    else:
        print(text)
    if (args.require_complete or args.require_budget) and not result['run_complete']:
        raise SystemExit(2)
    if args.require_budget and any(not g['frame_budget_met'] for g in result['groups']):
        raise SystemExit(2)


if __name__ == '__main__':
    main()
