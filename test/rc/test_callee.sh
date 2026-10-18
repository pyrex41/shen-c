#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_CALLEE_ROOT="$root" RC_CALLEE_BUILDER="$builder"
exec python3 - <<'PY'
import os
import re
import json
from pathlib import Path
import subprocess
import tempfile
root=Path(os.environ['RC_CALLEE_ROOT'])
cases=[
 ('atom-predicate','(cons? ())','false'),
 ('atom-string','(string? "sample")','true'),
 ('live-owner','(let X (cons 42 ()) (if (cons? X) (hd X) 0))','42'),
 ('tl-rooted','(let X (cons 1 (cons 42 ())) (hd (tl X)))','42'),
 ('cons-tl-reuse','(let X (cons 1 (cons 42 ())) (hd (cons 2 (tl X))))','2'),
 ('sequential-inner-borrows','(let X (cons 42 ()) (= (hd X) (hd X)))','true'),
 ('branch-physical-reuse','(let X (cons 42 ()) (let A (if true (hd X) (hd ())) (if (= A 42) (hd X) 0)))','42'),
 ('trap-borrowed-cleanup','(trap-error (hd "bad") (lambda E 42))','42'),
 ('lexical-shadow','(let hd (lambda X 42) (hd ()))','42'),
 ('computed-arguments','(hd (cons 42 ()))','42'),
 ('computed-callee','(let F hd (F (cons 42 ())))','42'),
 ('undefined-before-atoms','(trap-error (callee-undefined "text" 42) (lambda E 42))','42'),
 ('callee-before-effects','(hd (do (defun hd (X) 99) (cons 42 ())))','42'),
 ('replacement-visible','(hd ())','99'),
 ('replacement-redefines-self','(let Ignore (defun string? (X) (do (defun string? (Y) false) (do (shen.gc) true))) (string? "sample"))','true'),
 ('replacement-after-self','(string? "sample")','false'),
 ('tail-replacement','(let Ignore (defun number? (X) (if (= X 0) true (number? (- X 1)))) (number? 10000))','true'),
 ]
params=' '.join(f'V{i}' for i in range(80))
values=' '.join(['41']+['0']*79)
cases.append(('heap-frame-borrowed-storage',f'(defun callee-slab ({params}) (+ V0 1))\n(callee-slab {values})','42'))
with tempfile.TemporaryDirectory(prefix='shen-rc-callee.') as tmp:
 work=Path(tmp);inp=work/'input';inp.mkdir()
 (inp/'kernel.kl').write_text('')
 (inp/'user.kl').write_text('\n'.join(expr for _,expr,_ in cases)+'\n')
 expected='\n'.join(value for _,_,value in cases)+'\n'
 for conservative in (False,True):
  out=work/('conservative' if conservative else 'optimized')
  cmd=[os.environ['RC_CALLEE_BUILDER']]
  if conservative:cmd+=['--conservative-ownership']
  subprocess.run(cmd+[str(inp),str(out)],env={**os.environ,'SHEN_C_HOME':str(root)},check=True,capture_output=True,text=True)
  emitted=(out/'app.c').read_text()
  if not conservative:
   assert 'rc_function_get_borrowed(' in emitted
   assert 'RcValue *borrowed_fn=NULL;' in emitted
   assert 'borrowed_fns[' not in emitted
   names=re.search(r'rc_module_symbol_names\[\]=\{(.*?)\};',emitted,re.S)[1]
   names=[json.loads(line.rstrip(',')) for line in names.splitlines() if line.strip()]
   tl=names.index('tl')
   assert f'rc_function_get_borrowed(ctx,symbols[{tl}])' in emitted
   assert 'rc_tail_call_reuse(' in emitted
  actual=subprocess.run([str(out/'app')],env={**os.environ,'SHEN_RC_PRINT':'1'},check=True,capture_output=True,text=True)
  assert actual.stdout==expected,(conservative,actual.stdout,expected)
 print(f'RC borrowed callee: {len(cases)} differential cases PASS')
PY
