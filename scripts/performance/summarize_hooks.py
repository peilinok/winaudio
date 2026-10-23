"""Pair the pinned WinAudio hook probes; never apply GUI frame budgets."""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path


def read_rows(path, header):
    data = path.read_bytes()
    lines = data.decode('utf-8-sig').splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith(header))
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith('result=')), len(lines))
    rows = list(csv.DictReader(io.StringIO('\n'.join(lines[start:end]))))
    return rows, hashlib.sha256(data).hexdigest()


def summarize(directory):
    artifacts = {}
    micro = []
    for architecture in ('x64', 'x86'):
        versions = []
        for version in ('before', 'after'):
            path = directory / 'hook-paired-{}-{}.csv'.format(version, architecture)
            rows, digest = read_rows(path, 'repeat,entry,state,')
            artifacts[path.name] = digest
            indexed = {(int(r['repeat']), int(r['entry']), r['state']): r for r in rows}
            if len(indexed) != 72 or len(rows) != 72:
                raise ValueError('expected 3 repeats x 4 entries x 6 states: ' + path.name)
            if any(int(r['calls']) != 330000 for r in rows):
                raise ValueError('unexpected original API call count')
            versions.append(indexed)
        before, after = versions
        if before.keys() != after.keys():
            raise ValueError('unpaired probe conditions')
        for entry in range(4):
            for state in ('direct', 'no-ring', 'quiet', 'capture-off', 'pump-off', 'enabled'):
                pairs = []
                for repeat in range(1, 4):
                    old, new = before[repeat, entry, state], after[repeat, entry, state]
                    old_ns, new_ns = float(old['batch_ns']), float(new['batch_ns'])
                    pairs.append(dict(repeat=repeat, before_batch_ns=old_ns, after_batch_ns=new_ns,
                        change_percent=100 * (new_ns / old_ns - 1),
                        before_single_ns={p: float(old[p + '_ns']) for p in ('p50', 'p95', 'p99')},
                        after_single_ns={p: float(new[p + '_ns']) for p in ('p50', 'p95', 'p99')}))
                micro.append(dict(architecture=architecture, entry=entry, state=state, pairs=pairs))
    audio = []
    for repeat in range(1, 4):
        versions = []
        for version in ('before', 'after'):
            path = directory / 'hook-audio-{}-{}.txt'.format(version, repeat)
            if 'result=PASS;' not in path.read_text(encoding='utf-8-sig'):
                raise ValueError('audio probe did not pass: ' + path.name)
            rows, digest = read_rows(path, 'phase,target,callbacks,')
            artifacts[path.name] = digest
            indexed = {(r['phase'], int(r['target'])): r for r in rows}
            if len(indexed) != 14:
                raise ValueError('expected seven phases for two fresh targets')
            versions.append(indexed)
        before, after = versions
        if before.keys() != after.keys():
            raise ValueError('unpaired audio phases')
        for key in sorted(before):
            old, new = before[key], after[key]
            audio.append(dict(repeat=repeat, phase=key[0], target=key[1],
                before={k: float(v) for k, v in old.items() if k not in ('phase', 'target')},
                after={k: float(v) for k, v in new.items() if k not in ('phase', 'target')}))
    return dict(artifacts=artifacts, micro=micro, audio=audio,
        interpretation='Batch means and QPC-quantized single-call percentiles are separate. '
                       'Audio percentiles cover the target audio-processing loop body, '
                       'excluding periodic GetService and Sleep. '
                       'No GUI 0.5 ms gate or invented nanosecond ceiling is applied.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.write_text(json.dumps(summarize(args.directory), indent=2) + '\n', encoding='utf-8')
