#!/bin/sh
# Differential native execution plus measured RC/capture/allocation reductions.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_OWN_ROOT="$root" RC_OWN_BUILDER="$builder"
exec python3 - <<'PY'
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(os.environ['RC_OWN_ROOT'])
source = '''
(defun make (N A) (if (= N 0) A (make (- N 1) (cons N A))))
(defun rebuild (Xs) (if (cons? Xs) (cons (+ 1 (hd Xs)) (rebuild (tl Xs))) Xs))
(defun total (Xs A) (if (cons? Xs) (total (tl Xs) (+ A (hd Xs))) A))
(defun keep (Wanted Waste) (freeze Wanted))
'''
cases = [
    ('unique-traversal', '(total (rebuild (make 100 ())) 0)', '5150'),
    ('aliased-traversal', '(let Old (make 100 ()) (let New (rebuild Old) (+ (total Old 0) (total New 0))))', '10200'),
    ('free-capture', '(do (set *owned-closure* (keep 42 (make 200 ()))) ((value *owned-closure*)))', '42'),
    ('nested-free-capture', '(let X 20 (let Waste (make 10 ()) (let F (lambda Y (lambda Z (+ X (+ Y Z)))) ((F 10) 12))))', '42'),
    ('capture-param-shadow', '(let X (make 10 ()) ((lambda X X) 42))', '42'),
    ('special-shadow', '(let if (lambda X (+ X 1)) (if 41))', '42'),
    ('projection-shadow', '(let tl (lambda X 42) (tl (cons 1 ())))', '42'),
    ('constructor-shadow', '(let cons (lambda X (lambda Y X)) ((cons 42) ()))', '42'),
    ('if-left', '(let X (cons 42 ()) (if true (hd X) 0))', '42'),
    ('if-right', '(let X (cons 42 ()) (if false 0 (hd X)))', '42'),
    ('cond-rejected', '(let X (make 10 ()) (cond (false (hd X)) (true 42)))', '42'),
    ('handler-retains-read', '(let X (cons 42 ()) (trap-error (do (hd X) (simple-error "fault")) (lambda E (hd X))))', '42'),
    ('handler-blocks-detach', '(let X (cons 42 ()) (trap-error (cons 0 (do (tl X) (simple-error "fault"))) (lambda E (hd X))))', '42'),
    ('short-circuit', '(let X 42 (if (and false (simple-error "bad")) 0 X))', '42'),
    ('boolean-error', '(trap-error (and true 42) (lambda E 42))', '42'),
    ('redefined-projection', '(= (let X (cons 1 ()) (cons 2 (do (defun tl (Y) 42) (tl X)))) (cons 2 42))', 'true'),
    ('callee-before-args', '(let X (cons 1 ()) (hd (cons (do (defun cons (H T) H) 42) (tl X))))', '42'),
    ('redefined-cons', '(cons 42 ())', '42'),
    ('ordinary-do-redefined', '(do (defun do (X Y) X) 42)', '42'),
    ('ordinary-do-after-redefinition', '(do 42 0)', '42'),
]
with tempfile.TemporaryDirectory(prefix='shen-rc-ownership.') as tmp:
    work = Path(tmp)
    inp = work / 'input'; inp.mkdir()
    (inp / 'kernel.kl').write_text('')
    (inp / 'user.kl').write_text(source + '\n'.join(x[1] for x in cases) + '\n')
    env = dict(os.environ, SHEN_C_HOME=str(root))
    metrics = {}
    for mode, flags in [('conservative', ['--conservative-ownership']), ('optimized', [])]:
        dest = work / mode
        build = subprocess.run([os.environ['RC_OWN_BUILDER'], *flags, str(inp), str(dest)], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=90)
        if build.returncode:
            raise SystemExit('%s build failed:\n%s\n%s' % (mode, build.stdout, build.stderr))
        p = subprocess.run([str(dest / 'app')], env=dict(env, SHEN_RC_PRINT='1', SHEN_RC_STATS='1'),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30)
        actual = p.stdout.splitlines()
        expected = [x[2] for x in cases]
        if p.returncode or actual != expected:
            errors = ['%s expected=%r actual=%r' % (case[0], case[2], actual[i] if i < len(actual) else '<missing>')
                      for i, case in enumerate(cases) if i >= len(actual) or actual[i] != case[2]]
            raise SystemExit('%s semantic failure exit=%s\n%s\n%s' % (mode,p.returncode,'\n'.join(errors),p.stderr))
        lines = [x[len('RC_STATS '):] for x in p.stderr.splitlines() if x.startswith('RC_STATS ')]
        if len(lines) != 1:
            raise SystemExit('%s missing unique metrics line: %s' % (mode,p.stderr))
        metrics[mode] = json.loads(lines[0])
    base, opt = metrics['conservative'], metrics['optimized']
    assert opt['pair_reuses'] >= 100, metrics
    assert base['pair_allocations'] - opt['pair_allocations'] >= 100, metrics
    assert opt['capture_edges_created'] < base['capture_edges_created'], metrics
    assert opt['live_objects'] + 300 < base['live_objects'], metrics
    assert opt['retain_calls'] + opt['release_calls'] < base['retain_calls'] + base['release_calls'], metrics
    print('RC ownership: %d differential cases PASS; unique traversal reused cells; aliases/handlers/redefinitions preserved' % len(cases))
    print('RC ownership metrics ' + json.dumps(metrics, sort_keys=True))
PY
