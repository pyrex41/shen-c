"""Audit both retained attempts against the unchanged frozen configuration."""
from pathlib import Path
import hashlib
import json
import statistics
import subprocess

root = Path(__file__).resolve().parents[3]
config_path = root / 'evidence/rc/opt-20261017/final-freeze/final-config.json'
config = json.loads(config_path.read_text())
sha = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
for case in config['cases']:
    for path, expected in case['dependency_sha256'].items():
        assert sha(path) == expected, path
assert not subprocess.check_output([
    'git', 'diff', '8e01579', 'HEAD', '--', 'src/rc', 'tools/rc-build.c',
    'Makefile.rc', 'CMakeLists.txt', '.github/workflows/ci.yml'], cwd=root)

for name in ('accepted-window-20261017', 'accepted-window-20261017-settled'):
    out = root / 'evidence/rc' / name
    result = json.loads((out / 'benchmark/result.json').read_text())
    assert result['config_sha256'] == sha(config_path)
    assert result['execution_status'] == result['status'] == 'PASS'
    assert result['performance_acceptance']['status'] == 'PASS'
    runs = result['runs']
    assert len(runs) == 20
    for run in runs:
        assert run['status'] == 'PASS'
        assert run['passed'] == ['136'] and run['failed'] == ['0']
        validation = run['suite_validation']
        assert len(validation['passed_reports']) == 36
        assert set(validation['failed_reports']) == {0}
        assert validation['status'] == 'PASS' and not validation['errors']
        assert validation['output_sha256']['test.txt'] == hashlib.sha256(b'hello').hexdigest()
        for key in ('binary_unchanged', 'dependencies_unchanged', 'suite_unchanged', 'inputs_unchanged'):
            assert run[key]
        for kind in ('stdout', 'stderr'):
            assert sha(out / 'benchmark' / run[kind]) == run[kind + '_sha256']
        assert sha(run['command'][3]) == run['binary_sha256']
    pairs = []
    for index in range(1, 11):
        group = [r for r in runs if r['repetition'] == index]
        assert [r['label'] for r in group] == (['rc', 'boehm'] if index % 2 else ['boehm', 'rc'])
        by_label = {r['label']: r for r in group}
        pairs.append({key: by_label['rc'][key] / by_label['boehm'][key]
                      for key in ('user_seconds', 'sys_seconds', 'wall_seconds',
                                  'instructions', 'cycles', 'max_rss_bytes')})
    batches = [{key: statistics.median(p[key] for p in pairs[start:start + 5])
                for key in pairs[0]} for start in (0, 5)]
    assert all(b['user_seconds'] <= .9 and b['wall_seconds'] <= 1.03 for b in batches)
    wins = sum(p['user_seconds'] < 1 for p in pairs)
    assert wins >= 8 and all(p['max_rss_bytes'] < 1 for p in pairs)
    max_rss = max(r['max_rss_bytes'] for r in runs if r['label'] == 'rc')
    assert max_rss <= config['rc_baseline_rss_bytes'] * 1.05
    samples = [json.loads(line) for line in (out / 'process-samples.jsonl').read_text().splitlines()]
    activity = {needle: sum(needle in s['processes'] for s in samples) for needle in
                ('beam.smp', '/bin/java', '/host.test', '(compile)', '/clang',
                 'shen-go/shen', 'mediaanalysisd', 'wdavdaemon')}
    settled = name.endswith('-settled')
    if settled:
        assert all(activity[k] == 0 for k in ('beam.smp', '/bin/java', '/host.test',
                                             '(compile)', '/clang', 'shen-go/shen'))
    receipt = {
        'numerical_acceptance': 'PASS', 'execution_audit': 'PASS',
        'runtime_checkpoint': '8e01579c238ad6bf2aef18492bb47cf389cbbeed',
        'source_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
        'config_sha256': sha(config_path), 'result_sha256': sha(out / 'benchmark/result.json'),
        'host_samples_sha256': sha(out / 'process-samples.jsonl'),
        'suite_results': '20 x 136/0, all 36 reports and exact redirect output in every execution',
        'batches': batches, 'cpu_wins': wins, 'max_rc_rss_bytes': max_rss,
        'rc_baseline_rss_bytes': config['rc_baseline_rss_bytes'],
        'rss_ratio': max_rss / config['rc_baseline_rss_bytes'],
        'host_window': {
            'status': 'PASS_USER_ACCEPTED_WINDOW' if settled else 'COMPETING_JOBS_OBSERVED',
            'user_instruction': 'quiet enough now',
            'snapshot_count': len(samples), 'snapshot_presence_counts': activity,
            'scope': 'User-accepted ordinary service load; no claim of zero background CPU. '
                     'Process presence and ps estimated CPU are sampled, not exclusive accounting.',
            'session_work': 'No builds, profiles or diagnostic counters launched during timing.'},
        'retention': 'Both attempts retained in full. Earlier publications unchanged.'}
    (out / 'assessment.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(name, receipt['numerical_acceptance'], receipt['host_window']['status'], batches)
