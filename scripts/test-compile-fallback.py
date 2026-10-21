#!/usr/bin/env python3
"""A failed or timed-out load-time C compile must not change a program's result.

Runs one loaded Shen program through the CLI with a working compiler, with a
compiler that fails every second invocation (native and interpreted code
mixed), with a compiler that always exceeds SHEN_RC_COMPILE_TIMEOUT, and in the
explicit interpreter. Each run must exit 0 with identical stdout. Under
SHEN_RC_VERIFY_NATIVE=1 the same failing compiler must remain a hard error.
Finally a run on the fallback cache with a working compiler must compile again.
"""
import argparse, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

PROGRAM = r'''
(define square X -> (* X X))
(define sum-list [] -> 0 [X | Y] -> (+ X (sum-list Y)))
(define apply-twice F X -> (F (F X)))
(define count-down 0 Acc -> Acc N Acc -> (count-down (- N 1) (+ Acc 1)))
(output "~A~%" (sum-list (map (function square) [1 2 3 4])))
(output "~A~%" (apply-twice (/. X (+ X 1)) 5))
(output "~A~%" (count-down 100000 0))
(output "~A~%" (trap-error (simple-error "boom") (/. E (error-to-string E))))
(set *counter* 0)
(define bump -> (set *counter* (+ (value *counter*) 1)))
(bump)
(bump)
(output "~A~%" (value *counter*))
(output "~A~%" (let F (freeze (square 9)) (thaw F)))
'''
EXPECTED = '30\n7\n100000\nboom\n2\n81\n'

WRAPPER = '''#!/bin/sh
# Test compiler: FALLBACK_MODE=every-second fails even invocations,
# FALLBACK_MODE=slow exceeds any short timeout, otherwise compiles normally.
n=$(cat "$FALLBACK_COUNTER" 2>/dev/null || echo 0); n=$((n+1)); echo "$n" > "$FALLBACK_COUNTER"
case "$FALLBACK_MODE" in
  every-second) [ $((n % 2)) -eq 0 ] && { echo "injected failure $n" >&2; exit 1; } ;;
  slow) exec sleep 30 ;;
esac
exec "$FALLBACK_REAL_CC" "$@"
'''

def stats(stderr):
    lines = [dict(re.findall(r'(\w+)=([0-9.]+)', x)) for x in stderr.splitlines() if x.startswith('RC_NATIVE ')]
    if not lines: return {}
    return {k: float(v) if '.' in v else int(v) for k, v in lines[-1].items()}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('binary', type=Path)
    ap.add_argument('--cc', default=os.environ.get('SHEN_RC_NATIVE_CC') or shutil.which('cc') or 'cc')
    ap.add_argument('--timeout', type=float, default=600)
    args = ap.parse_args(); binary = args.binary.resolve()
    real_cc = shutil.which(args.cc) or args.cc
    with tempfile.TemporaryDirectory(prefix='shen-compile-fallback-') as directory:
        root = Path(directory); program = root / 'program.shen'; program.write_text(PROGRAM)
        wrapper = root / 'cc'; wrapper.write_text(WRAPPER); wrapper.chmod(0o700)
        base = {k: v for k, v in os.environ.items() if not k.startswith(('SHEN_RC_', 'FALLBACK_'))}
        base.update(SHEN_RC_NATIVE_STATS='1', SHEN_RC_NATIVE_CC=str(wrapper), FALLBACK_REAL_CC=real_cc,
                    FALLBACK_COUNTER=str(root / 'counter'))
        def run(name, success=True, **extra):
            env = dict(base, SHEN_RC_NATIVE_CACHE=str(root / ('cache-' + extra.pop('cache', name))), **extra)
            p = subprocess.run([str(binary), 'script', str(program)], cwd=root, env=env, capture_output=True,
                               text=True, timeout=args.timeout)
            assert (p.returncode == 0) == success, (name, p.returncode, p.stdout, p.stderr)
            if success: assert p.stdout == EXPECTED, (name, p.stdout, p.stderr)
            return stats(p.stderr), p
        s, p = run('native', SHEN_RC_VERIFY_NATIVE='1')
        assert s and s['compiles'] >= 2 and s['interpreter_entries'] == 0 and s['fallbacks'] == 0, s
        compiles = s['compiles']
        s, p = run('mixed', FALLBACK_MODE='every-second')
        assert p.stderr.count('interpreting instead') == 1 and 0 < s['fallbacks'] < s['compiles'], (s, p.stderr)
        assert s['failures'] == 0 and s['interpreter_entries'] > 0, s
        s, p = run('slow', FALLBACK_MODE='slow', SHEN_RC_COMPILE_TIMEOUT='0.2')
        assert 'status 124' in p.stderr and s['fallbacks'] == s['compiles'] > 0 and s['failures'] == 0, (s, p.stderr)
        assert not list((root / 'cache-slow').glob('*.so')), 'a timed-out compile was published'
        s, p = run('interpret', SHEN_RC_EXECUTION='interpret')
        s, p = run('strict', False, FALLBACK_MODE='every-second', SHEN_RC_VERIFY_NATIVE='1')
        assert 'native compilation failed (status 1)' in p.stderr and 'interpreting instead' not in p.stderr, p.stderr
        s, p = run('retry', cache='slow', SHEN_RC_VERIFY_NATIVE='1')
        assert s['compiles'] == compiles and s['disk_hits'] == 0 and s['interpreter_entries'] == 0, (s, compiles)
    print('RC compile fallback: native, mixed failures, timeouts and interpreter agree; strict mode errors; timed-out compiles retry PASS')

if __name__ == '__main__':
    main()
