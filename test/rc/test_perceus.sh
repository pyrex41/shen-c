#!/bin/sh
# Borrowed signatures, borrowed captures, fusion and reuse must be unobservable:
# redefinition, raises through borrowed frames, deep recursion and aliasing.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_PERCEUS_ROOT="$root" RC_PERCEUS_BUILDER="$builder"
exec python3 - <<'PY'
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(os.environ['RC_PERCEUS_ROOT'])
cases=[
 # A borrowed signature is a property of the closure, so redefinition replaces it.
 ('(defun blen (X) (if (cons? X) 1 0))',None),
 ('(defun caller (L) (let A (blen L) (let B (blen L) (cons A (cons B L)))))',None),
 ('(caller (cons 1 ()))','(1 1 1)'),
 ('(defun blen (X) X)',None),
 ('(caller (cons 1 ()))','((1) (1) 1)'),
 ('(defun blen (X) (cons X X))',None),
 ('(caller 5)','((5 | 5) (5 | 5) | 5)'),
 # The callee resolves before its arguments run.
 ('(defun target (X Y) (+ X Y))',None),
 ('(target 1 (do (defun target (X Y) (* X Y)) 5))','6'),
 ('(target 1 5)','5'),
 # A body may redefine itself and collect, then read its inputs.
 ('(defun self (X) (do (defun self (Y) 0) (do (shen.gc) (hd X))))',None),
 ('(self (cons 42 ()))','42'),
 ('(self (cons 42 ()))','0'),
 # Raises through borrowed frames, parameters and captures.
 ('(defun thrower (X) (if (cons? X) (simple-error "boom") X))',None),
 ('(defun mid (L) (let R (thrower L) (cons R L)))',None),
 ('(trap-error (mid (cons 1 ())) (lambda E (error-to-string E)))','"boom"'),
 ('(mid 3)','(3 | 3)'),
 ('(let L (cons 1 ()) (trap-error ((lambda Z (do (simple-error "c") L)) 0) (lambda E (cons (error-to-string E) L))))','("c" 1)'),
 ('(let L (cons 1 ()) (trap-error (cons (mid L) (simple-error "late")) (lambda E (cons (error-to-string E) L))))','("boom" 1)'),
 # Deep recursion, non-tail and tail, with a parameter that never changes.
 ('(defun deep (N L) (if (= N 0) (hd L) (+ 1 (deep (- N 1) L))))',None),
 ('(deep 20000 (cons 0 ()))','20000'),
 ('(defun sum (N L A) (if (= N 0) A (sum (- N 1) L (+ A (hd L)))))',None),
 ('(sum 1000000 (cons 1 ()) 0)','1000000'),
 ('(defun ev (N L) (if (= N 0) (hd L) (od (- N 1) L)))',None),
 ('(defun od (N L) (if (= N 0) (tl L) (ev (- N 1) L)))',None),
 ('(ev 1000001 (cons 1 (cons 2 ())))','(2)'),
 # A running closure outlives its last other owner.
 ('(defun mk (L) (lambda Z (do (set held 0) (do (shen.gc) (hd L)))))',None),
 ('(set held (mk (cons 7 ())))',None),
 ('((value held) 0)','7'),
 # Aliases never see reuse; unique inputs still produce the right values.
 ('(defun inc (L) (if (cons? L) (cons (+ 1 (hd L)) (inc (tl L))) ()))',None),
 ('(let L (cons 1 (cons 2 ())) (let R (inc L) (cons L R)))','((1 2) 2 3)'),
 ('(inc (inc (cons 1 ())))','(3)'),
 ('(inc 5)','()'),
 ('(defun app (X Y) (if (cons? X) (cons (hd X) (app (tl X) Y)) Y))',None),
 ('(let A (cons 1 ()) (let B (app A A) (cons A B)))','((1) 1 1)'),
 ('(app (app (cons 1 ()) (cons 2 ())) (cons 3 ()))','(1 2 3)'),
 ('(defun swap (L) (cons (tl L) (hd L)))',None),
 ('(swap (cons 1 2))','(2 | 1)'),
 ('(let L (cons 1 2) (cons (swap L) L))','((2 | 1) 1 | 2)'),
 ('(defun both (L) (cons (hd L) (cons (hd L) (tl L))))',None),
 ('(both (cons 1 ()))','(1 1)'),
 ('(defun keephd (L) (cons (hd L) ()))',None),
 ('(let P (cons (cons 1 ()) 2) (let R (keephd P) (cons R P)))','(((1)) (1) | 2)'),
 ('(defun twice (L) (cons L L))',None),
 ('(twice (cons 1 ()))','((1) 1)'),
 ('(defun trapping (L) (cons (hd L) (trap-error (simple-error "x") (lambda E (tl L)))))',None),
 ('(let L (cons 1 (cons 2 ())) (cons (trapping L) L))','((1 2) 1 2)'),
 ('(defun raises (L) (cons (hd L) (simple-error "after hd")))',None),
 ('(let L (cons 1 ()) (trap-error (raises L) (lambda E L)))','(1)'),
 ('(trap-error (raises (cons 1 ())) (lambda E (error-to-string E)))','"after hd"'),
 ('(trap-error (hd 5) (lambda E 0))','0'),
 # Partial, over and higher-order application of inferred signatures.
 ('(defun add3 (A B C) (+ A (+ B C)))',None),
 ('((add3 1) 2 3)','6'),
 ('(((add3 1) 2) 3)','6'),
 ('(defun const2 (A) (lambda B A))',None),
 ('(const2 1 2)','1'),
 ('(defun apply1 (F X) (F X))',None),
 ('(let L (cons 1 ()) (cons (apply1 (lambda Y (cons? Y)) L) L))','(true 1)'),
 ('(apply1 (lambda Y Y) (cons 1 ()))','(1)'),
 # Primitives replaced after callers compiled, with borrowed inputs.
 ('(defun second (L) (hd (tl L)))',None),
 ('(defun tl (X) (cons 9 X))',None),
 ('(second 5)','9'),
 ('(defun cons (A B) A)',None),
 ('(app 1 2)','2'),
 ('(+ 1 1)','2'),
]
source='\n'.join(c[0] for c in cases)+'\n'
with tempfile.TemporaryDirectory(prefix='rc-perceus.') as temp:
    work=Path(temp);inputs=work/'input';inputs.mkdir();(inputs/'kernel.kl').write_text('');(inputs/'user.kl').write_text(source)
    env=dict(os.environ,SHEN_C_HOME=str(root))
    for mode,flags in [('optimized',[])]:
        dest=work/mode
        built=subprocess.run([os.environ['RC_PERCEUS_BUILDER'],*flags,str(inputs),str(dest)],env=env,capture_output=True,text=True,timeout=300)
        assert built.returncode==0,(mode,built.stdout,built.stderr)
        ran=subprocess.run([str(dest/'app')],env=dict(env,SHEN_RC_PRINT='1'),capture_output=True,text=True,timeout=120)
        assert ran.returncode==0,(mode,ran.stdout,ran.stderr)
        lines=ran.stdout.splitlines();tops=[c for c in cases if not c[0].startswith('(defun')]
        assert len(lines)==len(tops),(mode,len(lines),len(tops),ran.stderr)
        for c,got in zip(tops,lines):
            assert c[1] is None or got==c[1],(mode,c[0],'want',c[1],'got',got)
print('RC perceus semantics: redefinition, raises through borrowed frames, deep recursion, aliases PASS')
PY
