"""Summarize the external process samples; no GUI frame inference in off mode."""
import argparse
import csv
import hashlib
import json
from pathlib import Path

from summarize import distribution


def summarize(root):
    result = []
    for mode in ('off', 'buffered', 'flush'):
        path = root / ('meter-' + mode + '-process.csv')
        data = path.read_bytes()
        rows = list(csv.DictReader(data.decode('utf-8-sig').splitlines()))
        samples = [r for r in rows if r['warmup'].lower() == 'false']
        first = rows[rows.index(samples[0]) - 1]
        last = samples[-1]
        seconds = float(last['elapsed_s']) - float(first['elapsed_s'])
        item = dict(mode=mode, process_sha256=hashlib.sha256(data).hexdigest(),
                    sample_count=len(samples), seconds=seconds,
                    process_cpu_ms_per_second=(float(last['cpu_ms']) - float(first['cpu_ms'])) / seconds,
                    one_second_cpu_ms=distribution([float(r['cpu_ms_per_s']) for r in samples]),
                    private_bytes_min=min(int(r['private_bytes']) for r in samples),
                    private_bytes_max=max(int(r['private_bytes']) for r in samples))
        if mode != 'off':
            probe = json.loads((root / ('meter-' + mode + '.json')).read_text(encoding='utf-8'))
            item['probe'] = probe
        result.append(item)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.write_text(json.dumps(summarize(args.root), ensure_ascii=False, indent=2) + '\n',
                           encoding='utf-8')
