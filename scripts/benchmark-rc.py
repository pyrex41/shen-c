#!/usr/bin/env python3
"""Serial, bounded manifest-bound release comparisons from an explicit JSON configuration.

Configuration: {"cases":[{"label":"rc","argv":["/absolute/shen-rc",
"script","runme.shen"],"env":{}}],"repetitions":3}.
Each repetition reverses order, preserving every raw output and time -l metric.
Environment overrides are passed through env AFTER time, preserving DYLD on macOS.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import statistics
import subprocess
import time
import platform
from release_manifest import load_release, prepare_suite, validate_suite, inventory


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def stages(text):
    """Shen timings are nested; retain events individually, never sum them."""
    result, context = [], []
    test_label = None
    for line in text.splitlines():
        stripped = line.strip()
        if ': (' in stripped and ' = ' in stripped:
            test_label = stripped[:2048]
        match = re.fullmatch(r'run time:\s+([0-9.eE+\-]+) secs', stripped)
        if match:
            result.append({'shen_cpu_seconds': float(match[1]), 'test_label': test_label,
                           'preceding_output': context[-3:]})
        elif stripped:
            context.append(stripped[:2048])
    return result


def acceptance(runs, config):
    """Explicit two-batch gate, independent of successful benchmark execution."""
    rc, boehm = config.get('rc_label', 'rc'), config.get('boehm_label', 'boehm')
    baseline = config.get('rc_baseline_rss_bytes')
    if len(runs) != 20 or config.get('repetitions') != 10 or not isinstance(baseline, (int,float)) or baseline <= 0:
        return {'status':'OPEN', 'reason':'requires ten complete pairs and frozen pre-optimization RC RSS'}
    pairs=[]
    for i in range(1,11):
        group={r['label']:r for r in runs if r['repetition']==i}
        if set(group)!={rc,boehm} or any(r['status']!='PASS' for r in group.values()):
            return {'status':'OPEN','reason':'missing complete successful pair'}
        try:
            pairs.append({k:group[rc][k]/group[boehm][k] for k in ('user_seconds','wall_seconds','max_rss_bytes')})
        except (KeyError,ZeroDivisionError):
            return {'status':'OPEN','reason':'missing time/RSS metric'}
    batches=[]
    for start in (0,5):
        med={k:statistics.median(p[k] for p in pairs[start:start+5]) for k in pairs[0]}
        batches.append(med)
    wins=sum(p['user_seconds']<1 for p in pairs)
    rc_rss=max(r['max_rss_bytes'] for r in runs if r['label']==rc)
    passed=(all(b['user_seconds']<=.9 and b['wall_seconds']<=1.03 and b['max_rss_bytes']<1 for b in batches)
            and wins>=8 and rc_rss<=1.05*baseline and all(p['max_rss_bytes']<1 for p in pairs))
    return {'status':'PASS' if passed else 'OPEN','batches':batches,'cpu_wins':wins,
            'paired_ratios':pairs,'rc_max_rss_vs_baseline':rc_rss/baseline}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, default=Path(__file__).resolve().parents[1] / 'shen/releases/shenoskernel-43.1.2.json')
    parser.add_argument('--suite', type=Path, help='Must match the selected manifest suite')
    parser.add_argument('--timeout', type=float, default=400)
    parser.add_argument('--output-mib', type=int, default=64)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    cases = config['cases']
    repetitions = config.get('repetitions', 3)
    assert cases and isinstance(repetitions, int) and 1 <= repetitions <= 20
    assert 0 < args.timeout <= 1800 and 1 <= args.output_mib <= 256
    labels = set()
    for case in cases:
        label = case['label']
        assert re.fullmatch(r'[A-Za-z0-9_-]+', label) and label not in labels
        labels.add(label)
        assert isinstance(case['argv'], list) and all(isinstance(x, str) for x in case['argv'])
        binary = Path(case['argv'][0])
        assert binary.is_absolute() and binary.is_file()
        assert all(isinstance(k, str) and isinstance(v, str) for k, v in case.get('env', {}).items())
        case['binary_sha256'] = digest(binary)
        for path, expected in case.get('dependency_sha256', {}).items():
            assert digest(path) == expected, path
    release = load_release(args.manifest)
    suite = Path(__file__).resolve().parents[1] / release['suite_directory']
    if args.suite and args.suite.resolve() != suite.resolve():
        raise ValueError('--suite must match selected release manifest')
    suite_hashes = {str(p.relative_to(suite)): digest(p) for p in sorted(suite.rglob('*')) if p.is_file()}
    args.out.mkdir(parents=True, exist_ok=False)
    out = args.out.resolve()
    stdin = out / 'stdin.txt'
    stdin.write_bytes(b'y\n' * 100000)
    record = {'scope': 'Serial same-suite/input CLI benchmark; no profiling or sampling',
              'release_manifest_sha256': digest(args.manifest), 'kernel_version': release['kernel_version'],
              'host': platform.uname()._asdict(),
              'source_identity': config.get('source_identity', {}),
              'suite': str(suite), 'suite_sha256': suite_hashes,
              'stdin_sha256': digest(stdin), 'config_sha256': digest(args.config),
              'cases': cases, 'repetitions': repetitions, 'runs': [],
              'harness_sha256': {str(p): digest(p) for p in (Path(__file__).resolve(), Path(__file__).resolve().with_name('release_manifest.py'))},
              'timing_note': 'Shen run-time events are nested CPU intervals; do not sum them.',
              'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
    def save():
        temporary = out / 'result.json.tmp'
        temporary.write_text(json.dumps(record, indent=2) + '\n')
        temporary.replace(out / 'result.json')
    base_env = dict(os.environ)
    for key in list(base_env):
        if key.startswith(('SHEN_', 'RC_', 'DYLD_', 'GC_')) or key in ('ASAN_OPTIONS', 'UBSAN_OPTIONS', 'CPUPROFILE', 'CPUPROFILESIGNAL'):
            base_env.pop(key, None)
    record['removed_environment_prefixes'] = ['SHEN_', 'RC_', 'DYLD_', 'GC_', 'ASAN_OPTIONS', 'UBSAN_OPTIONS', 'CPUPROFILE', 'CPUPROFILESIGNAL']
    save()
    for repetition in range(repetitions):
        order = cases if repetition % 2 == 0 else list(reversed(cases))
        for case in order:
            assert digest(case['argv'][0]) == case['binary_sha256'], 'binary changed before run'
            assert digest(args.manifest) == record['release_manifest_sha256'], 'release manifest changed'
            assert digest(args.config) == record['config_sha256'], 'configuration changed'
            assert digest(stdin) == record['stdin_sha256'], 'stdin changed'
            assert all(digest(p) == h for p,h in record['harness_sha256'].items()), 'harness changed'
            assert all(digest(path) == h for path, h in case.get('dependency_sha256', {}).items()), 'dependency changed before run'
            assert all(digest(suite / p) == h for p, h in suite_hashes.items()), 'suite changed before run'
            prefix = '%02d-%s' % (repetition + 1, case['label'])
            working = prepare_suite(release, out / (prefix + '-suite'))
            command = ['/usr/bin/time'] + (['-l'] if platform.system() == 'Darwin' else ['-v']) + ['/usr/bin/env']
            command += [key + '=' + value for key, value in sorted(case.get('env', {}).items())]
            command += case['argv']
            run = {'label': case['label'], 'repetition': repetition + 1,
                   'command': command, 'status': 'RUNNING', 'binary_sha256': case['binary_sha256']}
            record['runs'].append(run)
            save()
            started = time.monotonic()
            reason = None
            with stdin.open('rb') as inp, (out / (prefix + '.out')).open('wb') as stdout, (out / (prefix + '.err')).open('wb') as stderr:
                child = subprocess.Popen(command, cwd=working, env=base_env, stdin=inp,
                                         stdout=stdout, stderr=stderr, start_new_session=True)
                try:
                    while child.poll() is None:
                        if time.monotonic() - started > args.timeout:
                            reason = 'timeout'
                        if stdout.tell() + stderr.tell() > args.output_mib * 1024 * 1024:
                            reason = 'output budget'
                        if reason:
                            os.killpg(child.pid, signal.SIGKILL)
                            break
                        time.sleep(.2)
                finally:
                    if child.poll() is None:
                        os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
            stdout_path, stderr_path = out / (prefix + '.out'), out / (prefix + '.err')
            text, errors = stdout_path.read_text(errors='replace'), stderr_path.read_text(errors='replace')
            run.update(exit=child.returncode, wrapper_wall_seconds=round(time.monotonic() - started, 3),
                       stdout=stdout_path.name, stderr=stderr_path.name,
                       stdout_sha256=digest(stdout_path), stderr_sha256=digest(stderr_path),
                       binary_unchanged=digest(case['argv'][0]) == case['binary_sha256'],
                       dependencies_unchanged=all(digest(path) == h for path, h in case.get('dependency_sha256', {}).items()),
                       suite_unchanged=inventory(suite) == suite_hashes,
                       inputs_unchanged=(digest(args.manifest)==record['release_manifest_sha256'] and digest(args.config)==record['config_sha256'] and digest(stdin)==record['stdin_sha256'] and all(digest(p)==h for p,h in record['harness_sha256'].items())))
            run['suite_validation'] = validate_suite(release, text + '\n' + errors, working)
            run['passed'] = re.findall(r'^passed \.\.\. (\d+)\s*$', text, re.M)[-1:]
            run['failed'] = re.findall(r'^failed \.\.\. (\d+)\s*$', text, re.M)[-1:]
            match = re.search(r'([\d.]+) real\s+([\d.]+) user\s+([\d.]+) sys', errors)
            if match:
                run.update(wall_seconds=float(match[1]), user_seconds=float(match[2]), sys_seconds=float(match[3]))
            for key, pattern in [('max_rss_bytes', r'(\d+)\s+maximum resident set size'),
                                 ('instructions', r'(\d+)\s+instructions retired'),
                                 ('cycles', r'(\d+)\s+cycles elapsed'),
                                 ('involuntary_context_switches', r'(\d+)\s+involuntary context switches')]:
                match = re.search(pattern, errors)
                if match:
                    run[key] = int(match[1])
            events = stages(text)
            stage_path = out / (prefix + '-stages.json')
            stage_path.write_text(json.dumps(events, indent=2) + '\n')
            run['stage_events'] = stage_path.name
            run['stage_events_sha256'] = digest(stage_path)
            run['largest_shen_intervals'] = sorted(events, key=lambda event: event['shen_cpu_seconds'], reverse=True)[:10]
            run['status'] = 'PASS' if (not reason and child.returncode == 0 and run['suite_validation']['status'] == 'PASS' and run['binary_unchanged'] and run['dependencies_unchanged'] and run['suite_unchanged'] and run['inputs_unchanged'] and 'wall_seconds' in run) else 'FAIL'
            if reason:
                run['reason'] = reason
            save()
            print(json.dumps({key: value for key, value in run.items() if key != 'largest_shen_intervals'}), flush=True)
            if run['status'] != 'PASS':
                raise SystemExit(1)
    record['summary'] = {}
    for case in cases:
        selected = [run for run in record['runs'] if run['label'] == case['label']]
        record['summary'][case['label']] = {
            metric: {'min': min(values), 'median': statistics.median(values), 'max': max(values)}
            for metric in ('wall_seconds', 'user_seconds', 'sys_seconds', 'max_rss_bytes', 'instructions', 'cycles')
            if (values := [run[metric] for run in selected if metric in run])}
    record['execution_status'] = 'PASS'
    record['performance_acceptance'] = acceptance(record['runs'], config)
    record['status'] = 'PASS'  # execution validity; performance has its own gate
    record['ended_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save()


if __name__ == '__main__':
    main()
