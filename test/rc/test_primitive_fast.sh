#!/bin/sh
# Primitive shortcuts preserve selected closures, ownership, and tail checks.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_FAST_ROOT="$root" RC_FAST_BUILDER="$builder"
exec python3 - <<'PYTEST'
import os,pathlib,subprocess,tempfile,atexit,shutil
w=pathlib.Path(tempfile.mkdtemp(prefix='shen-rc-primitive-fast.'));atexit.register(shutil.rmtree,w)
env=dict(os.environ,SHEN_C_HOME=os.environ['RC_FAST_ROOT'],SHEN_RC_PRINT='1');inp=w/'specific-input';inp.mkdir(exist_ok=True);(inp/'kernel.kl').write_text('');(inp/'user.kl').write_text('''(defun leaf-bool (X) (and true (= X 42)))
(leaf-bool 42)
((+ 20) 22)
((cons 42) ())
(trap-error (+ "bad" 1) (lambda E 42))
(trap-error (hd ()) (lambda E 42))
(+ (do (shen.gc) 20) 22)
(let + (lambda X 42) (+ 0))
(hd (do (defun hd (X) 0) (cons 42 ())))
(hd (cons 42 ()))
(+ (do (defun + (X Y) 99) 20) 22)
(+ 20 22)
(defun bool-after (X) (and true (cons? X)))
(defun cons? (X) 42)
(trap-error (bool-after ()) (lambda E 42))
''');cmd=[os.environ['RC_FAST_BUILDER'],str(inp),str(w/'specific-app')];log=(w/'specific.log').open('w');p=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=90);assert p.returncode==0
p=subprocess.run([str(w/'specific-app/app')],env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30);log.write(p.stdout+p.stderr);log.close();assert p.returncode==0,(p.returncode,p.stderr);expected=['true','42','(42)','42','42','42','42','42','0','42','99','42'];assert p.stdout.splitlines()==expected,p.stdout
print('specific primitive identity, snapshots, shadows, partials, errors, GC, tail boolean PASS')
PYTEST
