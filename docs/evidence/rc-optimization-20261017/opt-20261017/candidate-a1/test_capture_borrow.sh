#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_CAPTURE_ROOT="$root" RC_CAPTURE_BUILDER="$builder"
exec python3 - <<'PY'
import json,os,re,subprocess,tempfile
from pathlib import Path
root=Path(os.environ['RC_CAPTURE_ROOT'])
prefix='''
(defun capture-adder (F X) (lambda Y (F X Y)))
(defun capture-sum (X Y) (+ X Y))
(defun capture-symbol-snapshot (F) (freeze (F 40 (do (defun capture-sum (A B) 99) 2))))
(defun capture-self (X) (do (defun capture-self (Y) 99) (lambda Z (+ X Z))))
(defun capture-count (N F) (if (= N 0) 0 (+ (F 1) (capture-count (- N 1) F))))
'''
cases=[
 ('call-captured-symbol','((capture-adder capture-sum 40) 2)','42'),
 ('call-captured-closure','((capture-adder (lambda X (lambda Y (+ X Y))) 40) 2)','42'),
 ('captured-symbol-snapshot','((capture-symbol-snapshot capture-sum))','42'),
 ('replacement-visible','((capture-adder capture-sum 40) 2)','99'),
 ('self-replaced-and-escape','(let F (capture-self 40) (do (shen.gc) (F 2)))','42'),
 ('self-new-binding','(capture-self 0)','99'),
 ('closure-argument-replaces-maker','(let F (capture-adder (lambda X (lambda Y (+ X Y))) 40) (F (do (defun capture-adder (A B) 0) (do (shen.gc) 2))))','42'),
 ('nested-capture-escape','(let X (cons 42 ()) (let F (freeze (freeze X)) (let G (F) (do (shen.gc) (hd (G))))))','42'),
 ('branch-inactive-owner','(let A (cons 42 ()) (let B (cons 99 ()) ((lambda P (if P (hd A) (hd B))) true)))','42'),
 ('capture-shadow','(let X 99 ((lambda X (+ X 2)) 40))','42'),
 ('captured-nonfunction-argument-effects','(let F 0 (trap-error ((freeze (F (set *capture-effect* 42)))) (lambda E (value *capture-effect*))))','42'),
 ('undefined-captured-symbol-before-effects','(do (set *capture-effect* 42) (let F missing-captured (trap-error ((freeze (F (set *capture-effect* 99)))) (lambda E (value *capture-effect*)))))','42'),
 ('captured-error-handler','(let X (cons 42 ()) (let F (freeze (trap-error (simple-error "bad") (lambda E (hd X)))) (do (shen.gc) (F))))','42'),
 ('cycle-capture','(let V (absvector 1) (let F (freeze (<-address V 0)) (do (address-> V 0 F) (do (shen.gc) (= (F) F)))))','true'),
 ('tail-captured-call','(let F (lambda X X) (let G (lambda X (F X)) (G 42)))','42'),
 ('shared-closure-repeat','(let X (cons 42 ()) (let F (freeze X) (= (F) (F))))','true'),
 ('owned-result-outlives-self','(let X ((lambda Y (freeze Y)) (cons 42 ())) (let V (X) (do (shen.gc) (hd V))))','42'),
 ('capture-traffic','(let X 41 (let F (lambda Y (+ X Y)) (capture-count 500 F)))','21000'),
]
with tempfile.TemporaryDirectory(prefix='shen-capture-borrow.') as tmp:
 work=Path(tmp);source=work/'input';source.mkdir()
 (source/'kernel.kl').write_text('');(source/'user.kl').write_text(prefix+'\n'.join(x[1] for x in cases)+'\n')
 expected='\n'.join(x[2] for x in cases)+'\n';stats={}
 for mode,flags in [('optimized',[]),('conservative',['--conservative-ownership'])]:
  out=work/mode
  subprocess.run([os.environ['RC_CAPTURE_BUILDER'],*flags,str(source),str(out)],check=True,capture_output=True,text=True,timeout=120)
  code=(out/'app.c').read_text()
  if mode=='optimized':
   assert re.search(r'\(t\[\d+\] \? t\[\d+\] : rc_capture\(self,\d+\)\)',code),'captured-callee proof path not emitted'
  run=subprocess.run([str(out/'app')],env={**os.environ,'SHEN_RC_PRINT':'1','SHEN_RC_STATS':'1'},capture_output=True,text=True,timeout=30)
  assert run.returncode==0 and run.stdout==expected,(mode,run.returncode,run.stdout,expected,run.stderr)
  stats[mode]=json.loads(re.search(r'^RC_STATS (.+)$',run.stderr,re.M)[1])
 assert stats['optimized']['retain_calls']<stats['conservative']['retain_calls'],stats
 print('Capture borrowing: %d differential cases PASS; symbol snapshots, callbacks, escape, alias, mutation/cycle, tail and error order'%len(cases))
 print(json.dumps(stats,sort_keys=True))
PY
