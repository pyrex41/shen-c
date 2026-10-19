#!/usr/bin/env python3
"""Native-tier controls: pinned suite and focused CLI checks.

Every suite run uses a fresh writable copy and the release validator. Output
is compared across configurations after masking only the harness's elapsed
seconds; typechecking inference counts must match exactly. Tier statistics
are asserted so that a configuration cannot pass without doing its work.
"""
import argparse, json, os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from release_manifest import load_release, prepare_suite, validate_suite  # noqa: E402

STATS = 'shen-rc native-tier:'


def clean_env(extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith('SHEN_RC_NATIVE')}
    env.update(extra)
    return env


def stats_of(text):
    for line in text.splitlines():
        if line.startswith(STATS + ' mode='):
            return {k: (float(v) if '.' in v else int(v) if v.isdigit() else v)
                    for k, v in re.findall(r'(\w+)=(\S+)', line)}
    return {}


def run_suite(binary, release, env_extra, workdir, timeout):
    suite = prepare_suite(release, workdir / 'suite')
    start, usage0 = time.monotonic(), os.times()
    try:
        proc = subprocess.run([str(binary), 'eval', '-l', release['suite_entrypoint']], cwd=suite,
                              env=clean_env(env_extra), stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout)
        text, code = proc.stdout.decode(errors='replace'), proc.returncode
    except subprocess.TimeoutExpired as exc:
        text, code = (exc.stdout or b'').decode(errors='replace') + '\n[TIMEOUT]\n', None
    usage1 = os.times()
    result = validate_suite(release, text, suite)
    result.update(exit=code, wall=round(time.monotonic() - start, 3),
                  user=round(usage1.children_user - usage0.children_user, 3),
                  sys=round(usage1.children_system - usage0.children_system, 3),
                  stats=stats_of(text))
    return result, text


def normalize(text):
    text = re.sub(r'run time: \S+ secs', 'run time: X secs', text)
    return '\n'.join(l for l in text.splitlines() if not l.startswith(STATS))


# Focused programs, each run in a fresh process with the tier off and forced.
PROGRAMS = {
    'redefine-after-promotion': ['(define f X -> (+ X 1))', '(f (f 3))', '(f 10)',
                                 '(define f X -> (* X 100))', '(f 2)'],
    'redefine-while-pending': ['(define g X -> [g X])', '(g 1)', '(define g X -> [new X])', '(g 2)', '(g 3)'],
    'tail-loop': ['(define loop N Acc -> (if (= N 0) Acc (loop (- N 1) (+ Acc 1))))',
                  '(loop 10 0)', '(loop 1000000 0)'],
    'partial-and-closures': ['(define add X Y -> (+ X Y))', '(add 1 2)', '(map (add 1) [1 2 3])',
                             '(define mk X -> (/. Y (+ X Y)))', '((mk 3) 4)', '((mk 5) 6)'],
    'errors': ['(define bad X -> (hd X))', '(bad [1])',
               '(trap-error (bad 5) (/. E (error-to-string E)))',
               '(define ok? X -> (if (number? X) true false))', '(ok? 1)', '(ok? a)'],
    'mutual': ['(define even? 0 -> true N -> (odd? (- N 1)))',
               '(define odd? 0 -> false N -> (even? (- N 1)))', '(even? 10)', '(even? 100001)'],
}


def run_program(binary, exprs, env, timeout=120):
    argv = [str(binary), 'eval']
    for e in exprs:
        argv += ['-e', e]
    proc = subprocess.run(argv, env=clean_env(env), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout)
    return proc.returncode, proc.stdout.decode(errors='replace')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('binary')
    ap.add_argument('--timeout', type=float, default=900)
    ap.add_argument('--evidence', type=Path)
    ap.add_argument('--configs', default='off,count,on,on,force-cold,force-warm,corrupt-cache,'
                                          'failing-cc,no-compiler,no-path')
    ap.add_argument('--no-programs', action='store_true')
    ap.add_argument('--cflags', help='SHEN_RC_NATIVE_CFLAGS for every configuration that sets none')
    args = ap.parse_args()
    release = load_release(ROOT / 'shen/releases/s43.1.json')
    binary = Path(args.binary).resolve()
    base = Path(tempfile.mkdtemp(prefix='native-tier-'))
    force = {'SHEN_RC_NATIVE': '1', 'SHEN_RC_NATIVE_THRESHOLD': '1'}
    if args.cflags:
        force['SHEN_RC_NATIVE_CFLAGS'] = args.cflags
    configs = {
        'off': {},
        'count': {'SHEN_RC_NATIVE': 'count'},
        'on': {'SHEN_RC_NATIVE': '1', 'SHEN_RC_NATIVE_CACHE': str(base / 'cache-on')},
        'on-O0': {'SHEN_RC_NATIVE': '1', 'SHEN_RC_NATIVE_CFLAGS': '-O0',
                  'SHEN_RC_NATIVE_CACHE': str(base / 'cache-on-O0')},
        'force-cold': dict(force, SHEN_RC_NATIVE_CACHE=str(base / 'cache-force')),
        'force-warm': dict(force, SHEN_RC_NATIVE_CACHE=str(base / 'cache-force')),
        'force-nocache': dict(force, SHEN_RC_NATIVE_CACHE='off'),
        'corrupt-cache': dict(force, SHEN_RC_NATIVE_CACHE=str(base / 'cache-corrupt')),
        'failing-cc': dict(force, SHEN_RC_NATIVE_CC='false', SHEN_RC_NATIVE_CACHE='off'),
        'no-compiler': dict(force, SHEN_RC_NATIVE_CC=str(base / 'missing-cc'), SHEN_RC_NATIVE_CACHE='off'),
        'no-path': dict(force, SHEN_RC_NATIVE_CC='cc', PATH=str(base / 'empty-path'),
                        SHEN_RC_NATIVE_CACHE='off'),
    }
    records, outputs, ok, seen = {}, {}, True, {}
    for index, name in enumerate(args.configs.split(',')):
        env = dict(configs[name], SHEN_RC_NATIVE_STATS='1')
        if args.cflags and name != 'off':
            env.setdefault('SHEN_RC_NATIVE_CFLAGS', args.cflags)
        if name == 'corrupt-cache':
            # A warm cache whose objects are truncated must recompile, not fail.
            source = base / 'cache-force'
            if not source.exists():
                print('corrupt-cache: requires an earlier force-cold run'); ok = False; continue
            shutil.copytree(source, base / 'cache-corrupt')
            for obj in (base / 'cache-corrupt').glob('*.so'):
                obj.write_bytes(obj.read_bytes()[:64])
        work = base / f'{index}-{name}'
        work.mkdir()
        result, text = run_suite(binary, release, env, work, args.timeout)
        label = f'{index}-{name}'
        s = result['stats']
        checks = []
        if name != 'off':
            checks.append(('stats reported', bool(s)))
        if s:
            if name.startswith('force') or name == 'corrupt-cache':
                checks += [('promoted most defuns', s['promoted'] >= 250), ('no failures', s['failed'] == 0)]
            if name.startswith('on'):
                checks += [('promoted hot defuns', s['promoted'] > 0), ('no failures', s['failed'] == 0)]
            if name.startswith('on') and seen.get(name) or name == 'force-warm':
                checks += [('warm: no compiles', s['compiles'] == 0),
                           ('warm: every batch hit', s['cache_hits'] == s['batches'])]
            if name == 'corrupt-cache':
                checks.append(('corrupt objects recompiled', s['compiles'] > 0))
            if name == 'failing-cc':
                checks += [('compiler failure falls back', s['promoted'] == 0 and s['failed'] > 0),
                           ('tier stays enabled', s['disabled'] == 0)]
            if name in ('no-compiler', 'no-path'):
                checks.append(('missing compiler disables tier', s['disabled'] == 1 and s['promoted'] == 0))
            if name == 'count':
                checks.append(('count mode compiles nothing', s['compiles'] == 0 and s['promoted'] == 0))
        seen[name] = True
        result['checks'] = dict(checks)
        records[label] = result
        outputs[label] = normalize(text)
        if args.evidence:
            args.evidence.mkdir(parents=True, exist_ok=True)
            (args.evidence / (label + '.log')).write_text(text)
        passed = result['status'] == 'PASS' and result['exit'] == 0 and all(v for _, v in checks)
        ok &= passed
        brief = ' '.join(f'{k}={s[k]}' for k in ('promoted', 'failed', 'batches', 'compiles', 'cache_hits',
                                                    'disabled', 'emit_s', 'compile_s') if k in s)
        print(f"{label}: {'PASS' if passed else 'FAIL'} exit={result['exit']} wall={result['wall']}s "
              f"user={result['user']}s {brief} {result['errors']} "
              f"{[k for k, v in checks if not v]}", flush=True)
    if outputs:
        first = next(iter(outputs))
        for label, text in outputs.items():
            same = text == outputs[first]
            records[label]['identical_output'] = same
            ok &= same
            if not same:
                print(f'{label}: normalized output differs from {first}')
    if not args.no_programs:
        cache = base / 'cache-programs'
        mask = lambda t: re.sub(r'\{0x[0-9a-f]+\}', '{ADDR}', normalize(t))  # function addresses
        for name, exprs in PROGRAMS.items():
            forced = dict(force, SHEN_RC_NATIVE_CACHE=str(cache), SHEN_RC_NATIVE_STATS='1')
            code0, plain = run_program(binary, exprs, {})
            code1, cold = run_program(binary, exprs, forced)
            code2, warm = run_program(binary, exprs, forced)
            s1, s2 = stats_of(cold), stats_of(warm)
            good = (code0 == code1 == code2 == 0 and mask(plain) == mask(cold) == mask(warm)
                    and s1.get('promoted', 0) > 0 and s1.get('failed') == 0 and s2.get('compiles') == 0)
            records['program-' + name] = {'pass': good, 'cold': s1, 'warm': s2, 'output': mask(plain)}
            ok &= good
            print(f"program {name}: {'PASS' if good else 'FAIL'} promoted={s1.get('promoted')} "
                  f"redefined={s1.get('redefined')} warm_hits={s2.get('cache_hits')}", flush=True)
            if not good:
                print(plain, cold, warm, sep='\n---\n')
    if args.evidence:
        (args.evidence / 'summary.json').write_text(json.dumps(records, indent=1))
    shutil.rmtree(base, ignore_errors=True)
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
