#!/bin/sh
# Differential ownership checks plus a structural bound on physical frames.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_FRAME_ROOT="$root" RC_FRAME_BUILDER="$builder"
exec python3 - <<'PY'
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(os.environ['RC_FRAME_ROOT'])
def tree(depth):
    return 'X' if not depth else '(+ %s %s)' % (tree(depth-1), tree(depth-1))

cases = [
    ('(compact 3)', '96'),
    ('(branch-compact 3 true)', '96'),
    ('(branch-compact 3 false)', '97'),
    ('(let X (cons 40 ()) (trap-error (cons 1 (do (tl X) (simple-error "outer"))) '
     '(lambda E (+ (hd X) (trap-error (simple-error "inner") (lambda F 2))))))', '42'),
    ('(let X 20 (let F (if true (lambda Y (+ X Y)) (lambda Y 0)) (F 22)))', '42'),
    ('(if (or false (and true true)) 42 0)', '42'),
]
text = 't[123] quote: " slash: \\'
cases.append((json.dumps(text), '"' + text + '"'))
with tempfile.TemporaryDirectory(prefix='shen-rc-compaction.') as tmp:
    work = Path(tmp)
    source = work / 'input'; source.mkdir()
    (source / 'kernel.kl').write_text('')
    (source / 'rc.manifest.txt').write_text('kernel=kernel.kl\nuser=user.kl\nsource-format=escaped-v1\n')
    (source / 'user.kl').write_text(
        '(defun compact (X) %s)\n' % tree(5) +
        '(defun branch-compact (X Flag) (if Flag %s (+ %s 1)))\n' % (tree(5), tree(5)) +
        '\n'.join(case[0] for case in cases) + '\n')
    env = dict(os.environ, SHEN_C_HOME=str(root))
    frames = {}
    for mode, flags in [('conservative', ['--conservative-ownership']), ('optimized', [])]:
        app = work / mode
        build = subprocess.run([os.environ['RC_FRAME_BUILDER'], *flags, str(source), str(app)],
                               env=env, capture_output=True, text=True, timeout=90)
        if build.returncode:
            raise SystemExit('%s build failed:\n%s\n%s' % (mode,build.stdout,build.stderr))
        code = (app / 'app.c').read_text()
        def single_function(name):
            functions = re.findall(r'static RcValue \*' + re.escape(name) +
                                   r'\([^\n]+\) \{(.*?)\n\}', code, re.S)
            assert len(functions) == 1, (mode, name, len(functions))
            return functions[0]
        callback = single_function('rc_native_1')
        guard = r'if\(argc!=1\) return rc_error\(ctx,"native arity mismatch"\);'
        if mode == 'optimized':
            # Public callbacks validate the runtime ABI before delegating to a
            # single internal body, which owns the compacted frame.
            assert re.fullmatch(r'\s*' + guard +
                                r'\s*return rc_native_body_1\(ctx,self,argv\);\s*', callback), callback
            function = single_function('rc_native_body_1')
        else:
            assert re.search(guard, callback), callback
            assert not re.search(r'static RcValue \*rc_native_body_1\(', code), mode
            function = callback
        count = re.search(r'(?:RcValue \*t\[|RcValue \*\*a=t\+)(\d+)', function)
        assert count, function
        frames[mode] = int(count[1])
        run = subprocess.run([str(app / 'app')], env=dict(env, SHEN_RC_PRINT='1'),
                             capture_output=True, text=True, timeout=30)
        if run.returncode or run.stdout.splitlines() != [case[1] for case in cases]:
            raise SystemExit('%s failed: exit=%d stdout=%r stderr=%s' %
                             (mode,run.returncode,run.stdout,run.stderr))
    assert frames['conservative'] > 64, frames
    assert frames['optimized'] < 32, frames
    print('RC frame compaction: %d differential cases PASS; slots %s' % (len(cases),frames))
PY
