#!/bin/sh
# Large constant trees compile to static tables. Compare them with the
# straight-line conservative baseline, including callee redefinition order.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_CONST_ROOT="$root" RC_CONST_BUILDER="$builder"
exec python3 - <<'PY'
import os
import random
from pathlib import Path
import subprocess
import tempfile
root=Path(os.environ['RC_CONST_ROOT'])
random.seed(43)
leaves=['()','sym','true','false','"plain"','"tab\tand\nnewline"','"café λ"','""',
        '0','-42','4611686018427387904','9223372036854775807','-9223372036854775808',
        '1.5','-0.25','1e300','(protect V1)','(protect (protect W2))']
def tree(depth):
    if depth==0 or random.random()<0.25: return random.choice(leaves)
    return f'(cons {tree(depth-1)} {tree(depth-1)})'
big=' '.join(f'(cons {tree(4)}' for _ in range(40))+' ()'+')'*40
# A bound variable anywhere keeps the enclosing calls as code; the constant
# subtrees beside it may still use tables.
mixed=f'(defun mixed (X) (cons X (cons {big} (cons X ()))))'
source=f'''(defun protect (X) X)
(defun big () {big})
{mixed}
(big)
(= (big) (big))
(mixed 7)
(defun protect (X) (do (set *calls* (cons X (value *calls*))) (cons X ())))
(set *calls* ())
(big)
(value *calls*)
(defun cons (A B) kons)
(big)
(trap-error (do (defun protect (X) (simple-error "stop")) (big)) (lambda E (error-to-string E)))
'''
with tempfile.TemporaryDirectory(prefix='shen-rc-const.') as tmp:
 work=Path(tmp);inp=work/'input';inp.mkdir()
 (inp/'kernel.kl').write_text('')
 (inp/'user.kl').write_text(source)
 outputs={}
 for mode,flags in [('optimized',[]),('conservative',['--conservative-ownership'])]:
  out=work/mode
  subprocess.run([os.environ['RC_CONST_BUILDER'],*flags,str(inp),str(out)],env={**os.environ,'SHEN_C_HOME':str(root)},check=True,capture_output=True,text=True)
  emitted=(out/'app.c').read_text()
  tables=emitted.count('=rc_constant_data(ctx,')
  if mode=='optimized': assert tables>=2,tables
  else: assert tables==0,tables
  run=subprocess.run([str(out/'app')],env={**os.environ,'SHEN_RC_PRINT':'1'},check=True,capture_output=True)
  outputs[mode]=run.stdout
 assert outputs['optimized']==outputs['conservative'],(outputs['optimized'][:400],outputs['conservative'][:400])
 assert b'kons' in outputs['optimized'] and b'stop' in outputs['optimized']
 print('RC constant data: optimized tables match conservative straight-line construction PASS')
PY
