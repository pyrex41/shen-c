#!/usr/bin/env python3
"""Bounded alternating phase-CPU probes; separate from whole-suite acceptance."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline', type=Path, required=True)
p.add_argument('--candidate', type=Path, required=True)
p.add_argument('--forms', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--repetitions', type=int, default=5)
a = p.parse_args()
assert 1 <= a.repetitions <= 10
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
binary = {'baseline': a.baseline.resolve(), 'candidate': a.candidate.resolve()}
forms = a.forms.resolve()
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
frozen = {str(path): sha(path) for path in [*binary.values(), forms]}
cases = [('concat', 16, 500000), ('concat', 256, 131072),
         ('concat', 4096, 32768), ('string-scan', 2048, 128),
         ('string-rebuild', 256, 256), ('string-build', 256, 256),
         ('vector-scan', 256, 16), ('vector-build', 256, 16)]
record = {'scope': 'Exploratory phase CPU (user plus system), including oracle and result release; '
                   'setup, input allocation, source loading and four warmup calls excluded. '
                   'Not a whole-suite or quiet-host performance claim.',
          'frozen_sha256': frozen, 'runs': [], 'repetitions': a.repetitions}
save = lambda: (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
for mode, size, iterations in cases:
    for repetition in range(1, a.repetitions + 1):
        for label in (['baseline', 'candidate'] if repetition % 2 else ['candidate', 'baseline']):
            for path, digest in frozen.items():
                assert sha(Path(path)) == digest, 'changed input: ' + path
            command = [str(binary[label]), str(forms), mode, str(size), str(iterations)]
            start = time.monotonic()
            run = subprocess.run(command, capture_output=True, timeout=45)
            name = '%s-%d-%02d-%s' % (mode, size, repetition, label)
            (out / (name + '.out')).write_bytes(run.stdout)
            (out / (name + '.err')).write_bytes(run.stderr)
            item = {'case': mode, 'size': size, 'repetition': repetition, 'label': label,
                    'command': command, 'exit': run.returncode,
                    'process_wall_seconds': time.monotonic() - start,
                    'stdout_sha256': hashlib.sha256(run.stdout).hexdigest(),
                    'stderr_sha256': hashlib.sha256(run.stderr).hexdigest()}
            record['runs'].append(item)
            save()
            assert run.returncode == 0, item
            assert len(run.stdout) + len(run.stderr) <= 1024 * 1024
            result = json.loads(run.stdout)
            assert result['mode'] == mode and result['size'] == size and result['iterations'] == iterations
            assert result['cpu_seconds'] > 0
            assert all(value == 0 for value in result['counters'].values()), 'diagnostic binary used for timing'
            item['result'] = result
            item['status'] = 'PASS'
            save()
    selected = [r for r in record['runs'] if r['case'] == mode and r['size'] == size]
    ratios = []
    for i in range(1, a.repetitions + 1):
        pair = {r['label']: r['result']['cpu_seconds'] for r in selected if r['repetition'] == i}
        ratios.append(pair['candidate'] / pair['baseline'])
    summary = {'case': mode, 'size': size, 'paired_cpu_ratios': ratios,
               'median_paired_cpu_ratio': statistics.median(ratios)}
    record.setdefault('summary', []).append(summary)
    print(json.dumps(summary), flush=True)
record['status'] = 'PASS'
for path, digest in frozen.items():
    assert sha(Path(path)) == digest
save()
