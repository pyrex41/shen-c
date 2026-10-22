#!/bin/sh
# Large native bodies must preserve callee snapshots and unwind one shared frame.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_CHUNK_ROOT="$root" RC_CHUNK_BUILDER="$builder"
exec python3 - <<'PY'
import os,subprocess,tempfile
from pathlib import Path
root=Path(os.environ['RC_CHUNK_ROOT'])
# The nest sits under a primitive so that no tail call reaches a module
# definition: a body with a musttail call is never split into chunks.
def nest(leaf):return '(hd (cons '+'(identity '*800+leaf+')'*800+' ()))'
message='expected goto cleanup; RcValue int test_'
source='''(defun identity (X) X)
(defun mutate () (do (defun identity (X) 99) (do (shen.gc) 42)))
'''
source+='(defun giant () '+nest('(mutate)')+')\n(giant)\n(identity 0)\n'
source+='(defun die (X) (simple-error X))\n'
source+='(defun giant-error () '+nest('(die "'+message+'")')+')\n'
source+='(trap-error (giant-error) (lambda E (error-to-string E)))\n'
with tempfile.TemporaryDirectory(prefix='rc-native-chunks.') as temp:
    work=Path(temp);inputs=work/'input';inputs.mkdir()
    (inputs/'kernel.kl').write_text('');(inputs/'user.kl').write_text(source)
    env=dict(os.environ,SHEN_C_HOME=str(root))
    for name,flags in [('chunks',[]),('conservative',['--conservative-ownership'])]:
        dest=work/name
        p=subprocess.run([os.environ['RC_CHUNK_BUILDER'],*flags,str(inputs),str(dest)],env=env,capture_output=True,text=True,timeout=180)
        assert p.returncode==0,(name,p.stdout,p.stderr)
        code=(dest/'app.c').read_text()
        assert ('static bool rc_native_chunk_' in code)==(name=='chunks'),name
        p=subprocess.run([str(dest/'app')],env=dict(env,SHEN_RC_PRINT='1'),capture_output=True,text=True,timeout=30)
        assert p.returncode==0,(name,p.stdout,p.stderr)
        assert p.stdout.splitlines()==['42','99','"'+message+'"'],(name,p.stdout,p.stderr)
    # The same giant bodies ending in a tail call to a module definition use
    # musttail and stay unchunked; their results must not change.
    tail_source=source.replace("(hd (cons ","").replace(" ()))","")
    tail_inputs=work/'tail-input';tail_inputs.mkdir()
    (tail_inputs/'kernel.kl').write_text('');(tail_inputs/'user.kl').write_text(tail_source)
    dest=work/'musttail'
    p=subprocess.run([os.environ['RC_CHUNK_BUILDER'],str(tail_inputs),str(dest)],env=env,capture_output=True,text=True,timeout=180)
    assert p.returncode==0,('musttail',p.stdout,p.stderr)
    assert 'static bool rc_native_chunk_' not in (dest/'app.c').read_text(),'musttail body was chunked'
    p=subprocess.run([str(dest/'app')],env=dict(env,SHEN_RC_PRINT='1'),capture_output=True,text=True,timeout=30)
    assert p.returncode==0 and p.stdout.splitlines()==['42','99','"'+message+'"'],('musttail',p.stdout,p.stderr)
print('native chunks: large bodies, callee snapshots across mutation/collection, literal bytes and error cleanup PASS')
PY
