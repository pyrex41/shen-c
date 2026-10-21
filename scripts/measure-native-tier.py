#!/usr/bin/env python3
"""Alternating suite timing for the native tier.

Rounds run base, tier-off, warm tier and warm forced tier in rotating order,
each in a fresh process and fresh suite copy. Reported per run: child user
CPU, and the in-process CPU (clock()) the harness prints for the
L-interpreter load, which is dominated by typechecking. Warm caches are
filled first; cold compile cost is measured separately and reported as
compiler-child CPU, which clock() in the Shen process does not include.
"""
import argparse, json, os, re, statistics, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from release_manifest import load_release, prepare_suite, validate_suite  # noqa: E402


def l_load_seconds(text):
    m = re.search(r'L interpreter: \(load "interpreter.shen"\) = loaded.*?run time: (\S+) secs', text, re.S)
    return float(m.group(1)) if m else None


def run(binary, env, release, work):
    suite = prepare_suite(release, work)
    full = {k: v for k, v in os.environ.items() if not k.startswith('SHEN_RC_NATIVE')}
    full.update(env, SHEN_RC_NATIVE_STATS='1')
    before = os.times()
    proc = subprocess.run([binary, 'eval', '-l', release['suite_entrypoint']], cwd=suite, env=full,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=900)
    after = os.times()
    text = proc.stdout.decode(errors='replace')
    status = validate_suite(release, text, suite)['status'] if proc.returncode == 0 else 'FAIL'
    stats = next((l for l in text.splitlines() if l.startswith('shen-rc native-tier: mode=')), '')
    return {'status': status, 'user': round(after.children_user - before.children_user, 3),
            'sys': round(after.children_system - before.children_system, 3),
            'l_load': l_load_seconds(text), 'stats': stats}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base')
    ap.add_argument('candidate')
    ap.add_argument('--rounds', type=int, default=5)
    ap.add_argument('--out', type=Path)
    args = ap.parse_args()
    args.base, args.candidate = str(Path(args.base).resolve()), str(Path(args.candidate).resolve())
    release = load_release(ROOT / 'shen/releases/shenoskernel-43.1.json')
    tmp = Path(tempfile.mkdtemp(prefix='native-tier-measure-'))
    on = {'SHEN_RC_NATIVE': '1', 'SHEN_RC_NATIVE_CACHE': str(tmp / 'cache-on')}
    forced = {'SHEN_RC_NATIVE': '1', 'SHEN_RC_NATIVE_THRESHOLD': '1',
              'SHEN_RC_NATIVE_CACHE': str(tmp / 'cache-forced')}
    cases = {'base': (args.base, {}), 'off': (args.candidate, {}),
             'on-warm': (args.candidate, on), 'forced-warm': (args.candidate, forced)}
    record = {'uptime_start': subprocess.run(['uptime'], capture_output=True, text=True).stdout.strip(),
              'cold': {}, 'runs': {k: [] for k in cases}}
    n = 0
    for name, env in (('on-cold', on), ('forced-cold', forced)):
        n += 1
        record['cold'][name] = run(args.candidate, env, release, tmp / f'w{n}')
        print(name, record['cold'][name], flush=True)
    names = list(cases)
    for r in range(args.rounds):
        order = names[r % len(names):] + names[:r % len(names)]
        for name in order:
            n += 1
            binary, env = cases[name]
            result = run(binary, env, release, tmp / f'w{n}')
            record['runs'][name].append(result)
            print(r, name, result['status'], result['user'], result['l_load'], flush=True)
    record['uptime_end'] = subprocess.run(['uptime'], capture_output=True, text=True).stdout.strip()
    summary = {}
    for name, runs in record['runs'].items():
        summary[name] = {'median_user': statistics.median(x['user'] for x in runs),
                         'median_l_load': statistics.median(x['l_load'] for x in runs),
                         'all_pass': all(x['status'] == 'PASS' for x in runs)}
    for name in ('off', 'on-warm', 'forced-warm'):
        pairs = [c['user'] / b['user'] for b, c in zip(record['runs']['base'], record['runs'][name])]
        lpairs = [c['l_load'] / b['l_load'] for b, c in zip(record['runs']['base'], record['runs'][name])]
        summary[name]['median_paired_user_ratio_vs_base'] = round(statistics.median(pairs), 4)
        summary[name]['median_paired_l_load_ratio_vs_base'] = round(statistics.median(lpairs), 4)
    record['summary'] = summary
    print(json.dumps(summary, indent=1), record['uptime_start'], record['uptime_end'], sep='\n')
    if args.out:
        args.out.write_text(json.dumps(record, indent=1))


if __name__ == '__main__':
    main()
