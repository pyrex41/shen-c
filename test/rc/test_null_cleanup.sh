#!/bin/sh
# Exercise physical-slot facts across branches, traps, reuse, and quoted text.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_CLEANUP_ROOT="$root" RC_CLEANUP_BUILDER="$builder"
exec python3 - <<'PY'
import os,subprocess,tempfile
from pathlib import Path
root=Path(os.environ['RC_CLEANUP_ROOT'])
with tempfile.TemporaryDirectory(prefix='shen-null-cleanup.') as tmp:
    work=Path(tmp);source=work/'input';source.mkdir();(source/'kernel.kl').write_text('')
    # This source string resembles cleanup syntax, including an actual newline.
    fake='  if(t[123]) rc_release(t[123]); t[123]=NULL;\n  t[12]=NULL;'
    expressions=[
        '(defun cleanup-branch (B) (let X (cons 42 ()) (let Y (if B (cons 1 ()) (cons 2 ())) (+ (hd X) (hd Y)))))',
        '(cleanup-branch true)','(cleanup-branch false)',
        '(let X (cons 42 ()) (trap-error (let Y (cons 1 ()) (cons (hd Y) (simple-error "fault"))) (lambda E (hd X))))',
        '(let X (cons 42 ()) (cons 0 (tl X)))',
        '(let X (cons 42 ()) (let Y (hd X) (let Z (cons Y ()) (hd Z))))',
        '(= "'+fake+'" "'+fake+'")',
        '(trap-error (cleanup-branch (simple-error "branch fault")) (lambda E 42))',
    ]
    (source/'user.kl').write_text('\n'.join(expressions)+'\n')
    env=dict(os.environ,SHEN_C_HOME=str(root),SHEN_RC_PRINT='1')
    for mode,flags in [('optimized',[]),('conservative',['--conservative-ownership'])]:
        dest=work/mode
        subprocess.run([os.environ['RC_CLEANUP_BUILDER'],*flags,str(source),str(dest)],env=env,check=True,timeout=120)
        p=subprocess.run([str(dest/'app')],env=env,capture_output=True,text=True,timeout=30)
        assert p.returncode==0 and p.stdout=='43\n44\n42\n(0)\n42\ntrue\n42\n',(mode,p.returncode,p.stdout,p.stderr)
        generated=(dest/'app.c').read_text()
        assert 'if(t[123]) rc_release(t[123]); t[123]=NULL;' in generated
    # Both let bindings are transferred before their range drop; no helper
    # call remains. Text resembling a helper call must not force its emission.
    (source/'user.kl').write_text('(let A 1 (let B 2 (+ A B)))\n"  rc_drop_range(t,0,2);"\n')
    dest=work/'all-ranges-eliminated'
    subprocess.run([os.environ['RC_CLEANUP_BUILDER'],str(source),str(dest)],env=env,check=True,timeout=120)
    generated=(dest/'app.c').read_text()
    assert 'static void rc_drop_range' not in generated
    print('RC null cleanup: branches, traps, reuse, slot recycling, quoted cleanup syntax PASS (both modes)')
PY
