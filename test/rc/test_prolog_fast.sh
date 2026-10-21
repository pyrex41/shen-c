#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_PROLOG_ROOT="$root" RC_PROLOG_BUILDER="$builder"
exec python3 - <<'PY'
import os,re,subprocess,tempfile
from pathlib import Path
root=Path(os.environ['RC_PROLOG_ROOT']);kernel=(root/'shen/src/kl/prolog.kl').read_text()
def definition(name):
 start=kernel.index('(defun '+name+' ');return kernel[start:kernel.index('\n\n',start)]
def wrapped(text):
 # Same meaning, different structure: the templates must not certify it.
 close=text.index(')',text.index('(',len('(defun ')))
 return text[:close+2]+'(let RcControl 0 '+text[close+2:-1]+'))'
def renamed(text):
 # Only bound variables change: the templates must still certify it.
 names=sorted(set(re.findall(r'\b[VWZ][0-9]+\b',text)))
 for i,old in enumerate(names):text=re.sub(r'\b'+old+r'\b','Rc'+str(i)+'Var',text)
 return text
original=definition('shen.pvar?')+'\n'+definition('shen.lazyderef')
control='\n'.join(wrapped(definition(n)) for n in ('shen.pvar?','shen.lazyderef'))
alpha=renamed(original)
assert alpha!=original
helpers='''
(defun make-pvar (N) (let V (absvector 2) (do (address-> V 0 shen.pvar) (address-> V 1 N))))
(defun make-store (X) (let V (absvector 3) (do (address-> V 1 X) V)))
'''
cases=[
 ('atom','(shen.pvar? 42)'),
 ('condition','(if (shen.pvar? (make-pvar 1)) 42 0)'),
 ('condition-empty','(if (shen.pvar? (absvector 0)) 0 42)'),
 ('condition-redefined','(trap-error (do (defun shen.pvar? (X) 7) (if (and true (shen.pvar? 42)) 0 0)) (lambda E (error-to-string E)))'),
 ('empty','(shen.pvar? (absvector 0))'),
 ('pvar','(shen.pvar? (make-pvar 1))'),
 ('short-pvar','(shen.pvar? (address-> (absvector 1) 0 shen.pvar))'),
 ('other','(shen.pvar? (absvector 2))'),
 ('lazy-atom','(shen.lazyderef 42 ())'),
 ('lazy-bound','(shen.lazyderef (make-pvar 1) (make-store 42))'),
 ('lazy-unbound','(let P (make-pvar 1) (= P (shen.lazyderef P (make-store shen.-null-))))'),
 ('chain','(let S (make-store (make-pvar 2)) (do (address-> S 2 42) (shen.lazyderef (make-pvar 1) S)))'),
 ('alias','(let F (function shen.lazyderef) (F (make-pvar 1) (make-store 42)))'),
 ('partial','(let F (shen.lazyderef (make-pvar 1)) (F (make-store 42)))'),
 ('partial-pvar','(let F (shen.pvar?) (F (make-pvar 1)))'),
 ('bad-index','(trap-error (shen.lazyderef (make-pvar -1) (make-store 42)) (lambda E (error-to-string E)))'),
 ('float-index','(trap-error (shen.lazyderef (make-pvar 1.0) (make-store 42)) (lambda E (error-to-string E)))'),
 ('short-lazy','(trap-error (shen.lazyderef (address-> (absvector 1) 0 shen.pvar) (make-store 42)) (lambda E (error-to-string E)))'),
 ('bad-store','(trap-error (shen.lazyderef (make-pvar 1) ()) (lambda E (error-to-string E)))'),
 ('overapply','(trap-error (shen.pvar? (make-pvar 1) 42) (lambda E (error-to-string E)))'),
 ('pvar-redefined','(do (defun shen.pvar? (X) false) (shen.lazyderef 42 ()))'),
 ('pvar-nonboolean','(trap-error (do (defun shen.pvar? (X) 7) (shen.lazyderef 42 ())) (lambda E (error-to-string E)))'),
 ('pvar-effect','(do (set *pg-effect* 0) (do (defun shen.pvar? (X) (do (set *pg-effect* (+ 1 (value *pg-effect*))) false)) (do (shen.lazyderef 42 ()) (value *pg-effect*))))'),
 ('lazy-redefined','(let F (function shen.lazyderef) (let P (make-pvar 1) (let S (make-store (make-pvar 2)) (do (defun shen.lazyderef (X Y) 99) (F P S)))))'),
 ('pvar-snapshot','(shen.pvar? (do (defun shen.pvar? (X) false) (make-pvar 1)))'),
 ('long','(do (defun fill-pvars (S N) (if (= N 0) (address-> S 0 42) (do (address-> S N (make-pvar (- N 1))) (fill-pvars S (- N 1))))) (let S (fill-pvars (absvector 4100) 4099) (shen.lazyderef (make-pvar 4099) S)))'),
 ('cyclic-result','(let V (absvector 1) (do (address-> V 0 V) (= V (shen.lazyderef (make-pvar 1) (make-store V)))))'),
]
for name,change in [('vectorp','(defun absvector? (X) false)'),('vget','(defun <-address (X N) shen.pvar)'),('eq','(defun = (X Y) false)')]:
 for when in ('before','argument'):
  expr='(do '+change+' (shen.pvar? (make-pvar 1)))' if when=='before' else '(shen.pvar? (do '+change+' (make-pvar 1)))'
  cases.append((name+'-'+when,'(trap-error '+expr+' (lambda E (error-to-string E)))'))
 if name=='eq':
  # 43.1 calls = even for an empty vector (on the trapped shen.not-pvar).
  for when,expr in (('before','(do (defun = (X Y) true) (shen.pvar? (absvector 0)))'),('argument','(shen.pvar? (do (defun = (X Y) true) (absvector 0)))')):
   cases.append(('eq-true-empty-'+when,'(trap-error '+expr+' (lambda E (error-to-string E)))'))
 cases.append(('lazy-'+name,'(let P (make-pvar 1) (let S (make-store 42) (do '+change+' (trap-error (shen.lazyderef P S) (lambda E (error-to-string E))))))'))
with tempfile.TemporaryDirectory(prefix='rc-prolog-fast.') as temp:
 work=Path(temp);env=dict(os.environ,SHEN_C_HOME=str(root),SHEN_RC_PRINT='1')
 for index,(name,expression) in enumerate(cases):
  outputs=[]
  for mode,defs in [('guarded',original),('control',control)]+([('renamed',alpha)] if index<3 else []):
   inp=work/(name+'-'+mode);inp.mkdir();(inp/'kernel.kl').write_text('');(inp/'user.kl').write_text(defs+'\n'+helpers+'\n(shen.pvar? (make-pvar 1))\n'+expression+'\n');dest=work/(name+'-'+mode+'-app')
   ran=subprocess.run([os.environ['RC_PROLOG_BUILDER'],str(inp),str(dest)],env=env,capture_output=True,text=True,timeout=120)
   assert ran.returncode==0,(name,mode,ran.stdout,ran.stderr)
   code=(dest/'app.c').read_text()
   assert ('rc_pvar_try_owned(' in code)==(mode!='control'),(name,mode,'pvar guard')
   assert ('rc_lazyderef_try_owned(' in code)==(mode!='control'),(name,mode,'lazy guard')
   ran=subprocess.run([str(dest/'app')],env=env,capture_output=True,text=True,timeout=30)
   assert ran.returncode==0,(name,mode,ran.stdout,ran.stderr)
   outputs.append(ran.stdout)
  assert len(set(outputs))==1,(name,outputs)
  print('Prolog differential PASS',name,flush=True)
 print('RC guarded Prolog: %d differential cases PASS; exact-body fallback, dependency guards, snapshots, partial/overapply, malformed/error, bounded walk and cyclic aliases'%len(cases))
PY
