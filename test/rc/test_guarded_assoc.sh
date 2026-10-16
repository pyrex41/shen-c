#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_ASSOC_ROOT="$root" RC_ASSOC_BUILDER="$builder"
exec python3 - <<'PY'
import os,re,subprocess,tempfile
from pathlib import Path
root=Path(os.environ['RC_ASSOC_ROOT'])
# Preserve the entire pinned AST, including its literal newline error suffix.
kernel=(root/'shen/releases/s42/kl/sys.kl').read_text()
start=kernel.index('(defun assoc ');end=kernel.index('\n\n',start)
original=kernel[start:end]
cases=[
 ('nil','(assoc 2 ())'),
 ('nil-redefined-eq','(trap-error (do (defun = (X Y) false) (assoc 2 ())) (lambda E (error-to-string E)))'),
 ('hit','(assoc 2 (cons (cons 1 10) (cons (cons 2 42) ())))'),
 ('match-before-improper','(assoc 2 (cons (cons 2 42) 7))'),
 ('long-prefix','(do (defun many (N) (if (= N 0) () (cons (cons N N) (many (- N 1))))) (assoc 0 (many 4100)))'),
 ('miss','(assoc 3 (cons (cons 1 10) ()))'),
 ('atom-entries','(assoc 2 (cons skip (cons (cons 2 42) ())))'),
 ('alias','(let F assoc (F 2 (cons (cons 2 42) ())))'),
 ('snapshot','(assoc (do (defun assoc (X Y) 99) 2) (cons (cons 2 42) ()))'),
 ('captured-original','(let F (function assoc) (do (defun assoc (X Y) 99) (F 2 (cons (cons 1 10) (cons (cons 2 42) ())))))'),
 ('malformed','(trap-error (assoc 2 (cons skip 7)) (lambda E (error-to-string E)))'),
 ('redefined-error','(do (defun simple-error (X) X) (assoc 2 (cons skip 7)))'),
 ('cycle-key','(let A (absvector 1) (let B (absvector 1) (do (address-> A 0 A) (do (address-> B 0 B) (assoc A (cons (cons B 42) ()))))))'),
 ('retained-alias','(let L (cons (cons 2 42) ()) (let R (assoc 2 L) (cons R L)))'),
]
for name,definition in [('eq','(defun = (X Y) false)'),('consp','(defun cons? (X) false)'),('hd','(defun hd (X) ())'),('tl','(defun tl (X) ())')]:
 for when in ['before','argument']:
  call='(assoc 2 (cons (cons 1 10) (cons (cons 2 42) ())))'
  call='(do %s %s)'%(definition,call) if when=='before' else '(assoc (do %s 2) (cons (cons 1 10) (cons (cons 2 42) ())))'%definition
  cases.append((name+'-'+when,'(trap-error '+call+' (lambda E (error-to-string E)))'))
with tempfile.TemporaryDirectory(prefix='shen-rc-assoc.') as tmp:
 work=Path(tmp);env=dict(os.environ,SHEN_C_HOME=str(root),SHEN_RC_PRINT='1')
 for name,expression in cases:
  outputs=[]
  for mode,definition in [('guarded',original),('control',original.replace('V3546','Key').replace('V3547','Pool'))]:
   inp=work/(name+'-'+mode);inp.mkdir();(inp/'kernel.kl').write_text('');(inp/'user.kl').write_text(definition+'\n'+expression+'\n');dest=work/(name+'-'+mode+'-app')
   p=subprocess.run([os.environ['RC_ASSOC_BUILDER'],str(inp),str(dest)],env=env,capture_output=True,text=True,timeout=90)
   assert p.returncode==0,(name,mode,p.stdout,p.stderr)
   generated=(dest/'app.c').read_text();assert ('rc_assoc_try_owned(' in generated)==(mode=='guarded'),(name,mode)
   p=subprocess.run([str(dest/'app')],env=env,capture_output=True,text=True,timeout=30);assert p.returncode==0,(name,mode,p.stdout,p.stderr);outputs.append(p.stdout)
  assert outputs[0]==outputs[1],(name,outputs)
  print('assoc differential PASS',name,flush=True)
 # A partial closure can itself be installed as assoc. Its remaining arity is
 # one: recursion in the original body then overapplies that partial binding.
 outputs=[]
 for mode,definition in [('guarded',original),('control',original.replace('V3546','Key').replace('V3547','Pool'))]:
  inp=work/('partial-'+mode);inp.mkdir();(inp/'kernel.kl').write_text(definition);(inp/'user.kl').write_text('');dest=work/('partial-'+mode+'-app')
  subprocess.run([os.environ['RC_ASSOC_BUILDER'],str(inp),str(dest)],env=env,capture_output=True,text=True,check=True,timeout=90)
  (dest/'partial.c').write_text(r'''#define SHEN_RC_NO_MAIN
#include "app.c"
int main(void){RcContext*c=rc_context_new();if(!rc_bootstrap_kernel(c))return 1;
RcValue*s=rc_symbol(c,"assoc"),*f=rc_function_get(c,s),*key=rc_integer(c,2);
RcValue*part=rc_apply(c,f,1,&key);if(!part||!rc_function_set(c,s,part))return 2;
RcValue*k1=rc_integer(c,1),*val=rc_integer(c,42),*e1=rc_cons(c,k1,val),*e2=rc_cons(c,key,val),*end=rc_cons(c,e2,rc_nil()),*pool=rc_cons(c,e1,end);
RcValue*r=rc_apply(c,part,1,&pool);if(r){rc_print(stdout,r);putchar('\n');}else printf("ERROR %s\n",rc_error_message(c));
rc_release(r);rc_release(pool);rc_release(end);rc_release(e1);rc_release(e2);rc_release(k1);rc_release(val);rc_release(part);rc_release(key);rc_release(f);rc_release(s);rc_context_free(c);return 0;}''')
  import shlex
  cmd=shlex.split(os.environ.get('CC','cc'))+shlex.split(os.environ.get('CFLAGS','-O2'))+['-std=c17','-I'+str(root/'src/rc'),str(dest/'partial.c')]+[str(root/'src/rc'/x) for x in ['rc.c','primitives.c','eval.c','read.c']]+['-lm','-o',str(dest/'partial')]
  p=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=90);assert p.returncode==0,p.stderr
  p=subprocess.run([str(dest/'partial')],env=env,capture_output=True,text=True,timeout=30);assert p.returncode==0,(p.returncode,p.stderr);outputs.append(p.stdout)
 assert outputs[0]==outputs[1] and outputs[0].startswith('ERROR '),outputs
 print('RC guarded assoc: %d differential cases PASS; exact AST control, aliases/snapshots, every dependency redefinition, malformed/error and cyclic equality'%len(cases))
PY
