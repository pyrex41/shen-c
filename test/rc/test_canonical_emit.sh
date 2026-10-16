#!/bin/sh
# Small native KL programs; no full kernel generation required.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_CANONICAL_ROOT="$root" RC_CANONICAL_BUILDER="$builder"
exec python3 - <<'PY'
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(os.environ['RC_CANONICAL_ROOT'])
source='''
(defun project-head (X) (hd X))
(defun project-tail (X) (tl X))
(defun pair-test (X) (cons? X))
(defun number-test (X) (number? X))
(defun string-test (X) (string? X))
(defun equal-test (X Y) (= X Y))
(defun partial-head () (hd))
(project-head (cons 42 ()))
(equal-test (project-tail (cons 1 (cons 42 ()))) (cons 42 ()))
(pair-test (cons 1 ()))
(number-test 42)
(string-test "text")
(equal-test (cons "same" ()) (cons "same" ()))
(let hd (lambda X (+ X 1)) (hd 41))
(hd (do (defun hd (X) 7) (cons 42 ())))
(hd ())
(string? (do (defun string? (X) false) "text"))
(string? "text")
'''
expected=['42','true','true','true','true','true','42','42','7','true','false']
with tempfile.TemporaryDirectory(prefix='rc-canonical-emit.') as tmp:
    work=Path(tmp);inputs=work/'input';inputs.mkdir()
    (inputs/'kernel.kl').write_text('')
    (inputs/'user.kl').write_text(source)
    env=dict(os.environ,SHEN_C_HOME=str(root))
    for mode,flags in [('optimized',[]),('conservative',['--conservative-ownership'])]:
        dest=work/mode
        run=subprocess.run([os.environ['RC_CANONICAL_BUILDER'],*flags,str(inputs),str(dest)],
                           env=env,capture_output=True,text=True,timeout=90)
        assert run.returncode==0,(mode,run.stdout,run.stderr)
        code=(dest/'app.c').read_text()
        for api in ['hd','tl','consp','numberp','stringp','eq']:
            assert (f'rc_primitive_{api}_owned(' in code)==(mode=='optimized'),(mode,api)
        run=subprocess.run([str(dest/'app')],env=dict(env,SHEN_RC_PRINT='1'),
                           capture_output=True,text=True,timeout=30)
        assert run.returncode==0 and run.stdout.splitlines()==expected,(mode,run.stdout,run.stderr)
print('canonical primitive emission: six APIs, 11 differential results, snapshots and shadows PASS')
PY
