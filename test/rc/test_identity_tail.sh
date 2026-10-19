#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_IDENTITY_ROOT="$root" RC_IDENTITY_BUILDER="$builder"
exec python3 - <<'PYCODE'
import os,re,subprocess,tempfile
from pathlib import Path
root=Path(os.environ['RC_IDENTITY_ROOT'])
source='''
(defun do (First Second) Second)
(defun first-only (A B) A)
(defun wrapped-second (A B) (type B T))
(defun atom-id (X) X)
(eval-kl (c-quote (defun interpreted-do-loop (N) (if (= N 0) 42 (do 0 (interpreted-do-loop (- N 1)))))))
(eval-kl (c-quote (interpreted-do-loop 40000)))
(eval-kl (c-quote (defun interpreted-do-bool (N) (and true (do 0 (if (= N 0) true (interpreted-do-bool (- N 1))))))))
(eval-kl (c-quote (interpreted-do-bool 20000)))
(eval-kl (c-quote (defun interpreted-do-bad (N) (and true (do 0 (if (= N 0) 7 (interpreted-do-bad (- N 1))))))))
(trap-error (eval-kl (c-quote (interpreted-do-bad 20000))) (lambda E 42))
(eval-kl (c-quote (defun snapshot-do (X) (do (do (defun do (A B) A) (shen.gc)) (atom-id X)))))
(eval-kl (c-quote (snapshot-do 42)))
(eval-kl (c-quote (do 1 42)))
(defun do (A B) B)
(eval-kl (c-quote (defun lexical-do (X) (let F do (F 0 (atom-id X))))))
(eval-kl (c-quote (lexical-do 42)))
(eval-kl (c-quote (defun computed-do (X) ((if true do missing) 0 (atom-id X)))))
(eval-kl (c-quote (computed-do 42)))
(eval-kl (c-quote (let F (do 0) (F 42))))
'''
expected='''interpreted-do-loop
42
interpreted-do-bool
true
interpreted-do-bad
42
snapshot-do
42
1
lexical-do
42
computed-do
42
42
'''
# The native KL emitter intentionally has no quote special form. Construct
# each interpreted AST through ordinary cons values, preserving symbol atoms.
tokens=iter(re.findall(r'[()]|[^\s()]+',source))
def parse(token):
 if token!='(':return token
 out=[]
 for token in tokens:
  if token==')':return out
  out.append(parse(token))
 raise AssertionError('unclosed fixture')
def literal(node):
 if not isinstance(node,list):return node
 out='()'
 for item in reversed(node):out='(cons '+literal(item)+' '+out+')'
 return out
def emit(node):
 if not isinstance(node,list):return node
 if node and node[0]=='c-quote':return literal(node[1])
 return '('+' '.join(map(emit,node))+')'
source='\n'.join(emit(parse(token)) for token in tokens)+'\n'
with tempfile.TemporaryDirectory(prefix='shen-rc-identity-tail.') as tmp:
 work=Path(tmp);inp=work/'input';inp.mkdir();(inp/'kernel.kl').write_text('');(inp/'user.kl').write_text(source)
 for conservative in [False,True]:
  dest=work/('borrowed' if conservative else 'owned')
  cmd=[os.environ['RC_IDENTITY_BUILDER']]+(['--conservative-ownership'] if conservative else [])+[str(inp),str(dest)]
  built=subprocess.run(cmd,env={**os.environ,'SHEN_C_HOME':str(root)},capture_output=True,text=True)
  assert built.returncode==0,(conservative,built.stdout,built.stderr)
  emitted=(dest/'app.c').read_text()
  constructor='rc_closure_return_second' if conservative else 'rc_closure_return_second_owned'
  # Optimized top-level definitions are wrapped to become immortal.
  constructor=constructor if conservative else 'rc_closure_static('+constructor
  assert emitted.count('='+constructor+'(')==2,(constructor,emitted.count('='+constructor+'('))
  actual=subprocess.run([str(dest/'app')],env={**os.environ,'SHEN_RC_PRINT':'1'},capture_output=True,text=True)
  assert actual.returncode==0,(conservative,actual.stdout,actual.stderr)
  assert actual.stdout==expected,(conservative,actual.stdout,actual.stderr)
print('RC compiled/interpreted identity forwarding: 14 cases x owned/borrowed PASS')
PYCODE
