#!/bin/sh
# Compare native condition lowering against conservative owned-value evaluation.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_CONDITION_ROOT="$root" RC_CONDITION_BUILDER="$builder"
exec python3 - <<'PY'
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(os.environ['RC_CONDITION_ROOT'])
source='''
(defun bpair (X) (if (cons? X) (hd X) 0))
(defun bnumber (X) (if (number? X) (+ X 0) 0))
(defun bstring (X) (if (string? X) (if (= X "yes") 42 0) 0))
(defun bequal (X Y) (if (= X Y) (+ X Y) 0))
(defun bgt (X Y) (if (> X Y) (+ X Y) 0))
(defun blt (X Y) (if (< X Y) (+ X Y) 0))
(defun bge (X Y) (if (>= X Y) (+ X Y) 0))
(defun ble (X Y) (if (<= X Y) (+ X Y) 0))
(bpair (cons 42 ()))
(bnumber 42)
(bstring "yes")
(bequal 21 21)
(bgt 40 2)
(blt 2 40)
(bge 21 21)
(ble 21 21)
(defun cpair (X) (if (cons? X) 42 0))
(defun cnumber (X) (cond ((number? X) 42) (true 0)))
(defun cstring (X) (if (string? X) 42 0))
(defun cequal (X Y) (if (= X Y) 42 0))
(defun cgt (X Y) (if (> X Y) 42 0))
(defun clt (X Y) (if (< X Y) 42 0))
(defun cge (X Y) (if (>= X Y) 42 0))
(defun cle (X Y) (if (<= X Y) 42 0))
(cpair (cons 1 ()))
(cpair ())
(cnumber 42)
(cnumber "42")
(cstring "42")
(cstring 42)
(cequal (cons 1 ()) (cons 1 ()))
(cequal (cons 1 ()) (cons 2 ()))
(cgt 7 2)
(clt 2 7)
(cge 7 7)
(cle 7 7)
(cgt 9223372036854775807 9223372036854775806)
(cle 9223372036854775807 9223372036854775807.0)
(cequal abc abc)
(cequal abc "abc")
(cequal "abc" abc)
(cequal abc abd)
(cequal () ())
(cequal () (cons 1 ()))
(cequal (cons 1 ()) ())
(cequal () "")
(cequal true true)
(cequal true false)
(cequal abc 1)
(cequal 1 abc)
(cequal abc 1.0)
(cequal (intern "abc") abc)
(if (and (cons? (cons 1 ())) (or (number? "no") (string? "yes"))) 42 0)
(if (and false (simple-error "unreached")) 0 42)
(if (or true (simple-error "unreached")) 42 0)
(let true false (if true 0 42))
(let cons? (lambda X false) (if (cons? (cons 1 ())) 0 42))
(trap-error (if (and true 1) 0 0) (lambda E (error-to-string E)))
(trap-error (if (and 1 true) 0 0) (lambda E (error-to-string E)))
(trap-error (if (or false 1) 0 0) (lambda E (error-to-string E)))
(trap-error (if (or 1 false) 0 0) (lambda E (error-to-string E)))
(trap-error (cond ((and true 1) 0)) (lambda E (error-to-string E)))
(trap-error (cgt "bad" 1) (lambda E (error-to-string E)))
(if (cons? (do (defun cons? (X) false) (do (shen.gc) (cons 1 ())))) 42 0)
(if (cons? (cons 1 ())) 0 42)
(if (= (do (defun = (X Y) false) 42) 42) 42 0)
(if (= 42 42) 0 42)
(defun string? (X) 7)
(trap-error (if (string? "x") 0 0) (lambda E (error-to-string E)))
(trap-error (if (and true (string? "x")) 0 0) (lambda E (error-to-string E)))
(trap-error (if (and (string? "x") true) 0 0) (lambda E (error-to-string E)))
(defun number? (A B) B)
(trap-error (if (number? 1) 0 0) (lambda E (error-to-string E)))
(defun < (X) (lambda Y true))
(if (< 1 2) 42 0)
(defun cons? (X) (and true 7))
(trap-error (if (cons? 1) 0 0) (lambda E (error-to-string E)))
(defun cons? (X) false)
(cpair (cons 1 ()))
(cequal (cons 1 ()) (cons 1 ()))
(trap-error (cstring "x") (lambda E (error-to-string E)))
(trap-error (cnumber 1) (lambda E (error-to-string E)))
(clt 1 2)
(defun cons? (X) (do (defun cons? (Y) false) (do (shen.gc) true)))
(cpair (cons 1 ()))
'''
# The symbol/[] rows exercise the identity fast path for =.
expected=['42']*8+['42','0','42','0','42','0','42','0','42','42','42','42','42','42',
 '42','0','0','0','42','0','0','0','42','0','0','0','0','42',
 '42','42','42','42','42',
 '"boolean expected"','"condition is not a boolean"','"boolean expected"','"condition is not a boolean"','"boolean expected"',
 '"Arithmetic requires numbers"','42','42','42','42','"condition is not a boolean"','"boolean expected"','"condition is not a boolean"','"condition is not a boolean"','42','"boolean expected"',
 '0','0','"condition is not a boolean"','"condition is not a boolean"','42','42']
with tempfile.TemporaryDirectory(prefix='rc-condition-emit.') as temp:
    work=Path(temp);inputs=work/'input';inputs.mkdir();(inputs/'kernel.kl').write_text('');(inputs/'user.kl').write_text(source)
    env=dict(os.environ,SHEN_C_HOME=str(root))
    for mode,flags in [('optimized',[]),('conservative',['--conservative-ownership'])]:
        dest=work/mode
        built=subprocess.run([os.environ['RC_CONDITION_BUILDER'],*flags,str(inputs),str(dest)],env=env,capture_output=True,text=True,timeout=120)
        assert built.returncode==0,(mode,built.stdout,built.stderr)
        code=(dest/'app.c').read_text()
        for api in ('consp','numberp','stringp','eq','gt','lt','ge','le'):
            assert ('rc_condition_'+api+'_borrowed(' in code)==(mode=='optimized'),(mode,api)
            # Last-use operands retain the consuming boundary, preserving the
            # roots visible to cycle collection at the condition safepoint.
            assert ('rc_condition_'+api+'_owned(' in code)==(mode=='optimized'),(mode,api)
        ran=subprocess.run([str(dest/'app')],env=dict(env,SHEN_RC_PRINT='1'),capture_output=True,text=True,timeout=30)
        assert ran.returncode==0,(mode,ran.stdout,ran.stderr)
        assert ran.stdout.splitlines()==expected,(mode,ran.stdout.splitlines(),expected,ran.stderr)
print('native conditions: 8 predicates, %d differential results (14 for = against symbols and []), live borrows, last-use consumption, snapshots and errors PASS'%len(expected))
PY
