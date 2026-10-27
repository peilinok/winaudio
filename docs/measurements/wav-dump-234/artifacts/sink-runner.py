import csv
import hashlib
import json
import struct
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path.cwd()
BASE = ROOT / '.scratch/wav-dump-234'
OUT = BASE / 'sink-formal'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def verify_wav(path):
    raw = path.read_bytes()
    assert raw[:4] == b'RIFF' and raw[8:12] == b'WAVE', path
    assert struct.unpack_from('<I', raw, 4)[0] + 8 == len(raw), path
    chunks = {}
    pos = 12
    while pos + 8 <= len(raw):
        tag, size = struct.unpack_from('<4sI', raw, pos)
        pos += 8
        assert pos + size <= len(raw), (path, tag)
        chunks[tag] = raw[pos:pos + size]
        pos += size + size % 2
    fmt = chunks[b'fmt ']
    tag, channels, rate, byte_rate, align, bits = struct.unpack_from('<HHIIHH', fmt)
    assert channels == 2 and rate == 48000 and align == 8 and bits == 32, path
    assert byte_rate == 384000, path
    assert tag in (3, 65534), (path, tag)
    if tag == 65534:
        assert len(fmt) >= 40 and struct.unpack_from('<H', fmt, 18)[0] == 32, path
        assert struct.unpack_from('<I', fmt, 24)[0] == 3, path
    block = b''.join(struct.pack('<f', ((i % 31) - 15) / 32.0) for i in range(960))
    expected = block * 1000
    assert chunks[b'data'] == expected, path
    return {'file': path.name, 'bytes': len(raw), 'data_bytes': len(expected),
            'sha256': sha(path), 'readback': 'all 1000 blocks equal expected Float32 PCM'}

if OUT.exists():
    raise RuntimeError('Refusing to replace formal measurement directory')
OUT.mkdir()
provenance = {'workspace': str(ROOT), 'method': 'before1, after1, before2, after2, before3, after3; separate normal and closed phases',
              'formal': True, 'complete': False, 'started_utc': datetime.now(timezone.utc).isoformat(), 'snapshots': {}, 'runs': []}
for name in ('before', 'after'):
    snapshot = BASE / ('performance-' + name)
    exe = snapshot / 'build/bin/Release/WavSinkProbe.exe'
    provenance['snapshots'][name] = {'source_pin': (snapshot/'source.txt').read_text(encoding='utf-8'),
        'executable': str(exe), 'executable_sha256': sha(exe),
        'probe_source_sha256': sha(snapshot/'source/wav-sink-probe.cpp')}
try:
    for round_number in (1, 2, 3):
        for name in ('before', 'after'):
            label = f'{name}-{round_number}'
            exe = Path(provenance['snapshots'][name]['executable'])
            wav = OUT / f'sink-{label}.wav'
            print('START ' + label, flush=True)
            started = datetime.now(timezone.utc).isoformat()
            result = subprocess.run([str(exe), str(wav)], capture_output=True, timeout=60)
            output = OUT / f'sink-{label}.csv'
            output.write_bytes(result.stdout)
            (OUT/f'sink-{label}.stderr.txt').write_bytes(result.stderr)
            assert result.returncode == 0, (label, result.returncode, result.stderr)
            lines = result.stdout.decode('utf-8-sig').splitlines()
            assert 'readback=verified' in result.stdout.decode('utf-8-sig'), label
            rows = list(csv.DictReader(line for line in lines if not line.startswith('#')))
            assert len(rows) == 2 and rows[0]['phase'] == 'normal' and rows[1]['phase'] == 'closed', label
            assert rows[0]['accepted_bytes'] == '3840000' and rows[1]['accepted_bytes'] == '0', label
            check = verify_wav(wav)
            run = {'label': label, 'started_utc': started, 'ended_utc': datetime.now(timezone.utc).isoformat(),
                   'exit_code': result.returncode, 'csv_sha256': sha(output), 'wav': check, 'rows': rows}
            provenance['runs'].append(run)
            (OUT/'sink-provenance.json').write_bytes(json.dumps(provenance, indent=2).encode('utf-8'))
            print('PASS ' + label + ' p50/p95/p99_ns=' + '/'.join(rows[0][key] for key in ('p50_ns','p95_ns','p99_ns')), flush=True)
    provenance['complete'] = len(provenance['runs']) == 6
finally:
    provenance['ended_utc'] = datetime.now(timezone.utc).isoformat()
    (OUT/'sink-provenance.json').write_bytes(json.dumps(provenance, indent=2).encode('utf-8'))
print('PASS six alternating runs; every WAV verified byte-for-byte', flush=True)
