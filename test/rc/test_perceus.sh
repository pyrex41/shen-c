#!/bin/sh
# Borrowed signatures, borrowed captures, fusion and reuse must be unobservable:
# redefinition, raises through borrowed frames, deep recursion and aliasing.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
export RC_PERCEUS_ROOT="$root" RC_PERCEUS_BUILDER="$builder"
exec python3 - <<'PY'
import json
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(os.environ['RC_PERCEUS_ROOT'])
cases=[
 # A borrowed signature is a property of the closure, so redefinition replaces it.
 ('(defun blen (X) (if (cons? X) 1 0))',None),
 ('(defun caller (L) (let A (blen L) (let B (blen L) (cons A (cons B L)))))',None),
 ('(caller (cons 1 ()))','(1 1 1)'),
 ('(defun blen (X) X)',None),
 ('(caller (cons 1 ()))','((1) (1) 1)'),
 ('(defun blen (X) (cons X X))',None),
 ('(caller 5)','((5 | 5) (5 | 5) | 5)'),
 # The callee resolves before its arguments run.
 ('(defun target (X Y) (+ X Y))',None),
 ('(target 1 (do (defun target (X Y) (* X Y)) 5))','6'),
 ('(target 1 5)','5'),
 # A body may redefine itself and collect, then read its inputs.
 ('(defun self (X) (do (defun self (Y) 0) (do (shen.gc) (hd X))))',None),
 ('(self (cons 42 ()))','42'),
 ('(self (cons 42 ()))','0'),
 # Raises through borrowed frames, parameters and captures.
 ('(defun thrower (X) (if (cons? X) (simple-error "boom") X))',None),
 ('(defun mid (L) (let R (thrower L) (cons R L)))',None),
 ('(trap-error (mid (cons 1 ())) (lambda E (error-to-string E)))','"boom"'),
 ('(mid 3)','(3 | 3)'),
 ('(let L (cons 1 ()) (trap-error ((lambda Z (do (simple-error "c") L)) 0) (lambda E (cons (error-to-string E) L))))','("c" 1)'),
 ('(let L (cons 1 ()) (trap-error (cons (mid L) (simple-error "late")) (lambda E (cons (error-to-string E) L))))','("boom" 1)'),
 # Deep recursion, non-tail and tail, with a parameter that never changes.
 ('(defun deep (N L) (if (= N 0) (hd L) (+ 1 (deep (- N 1) L))))',None),
 ('(deep 20000 (cons 0 ()))','20000'),
 ('(defun sum (N L A) (if (= N 0) A (sum (- N 1) L (+ A (hd L)))))',None),
 ('(sum 1000000 (cons 1 ()) 0)','1000000'),
 ('(defun ev (N L) (if (= N 0) (hd L) (od (- N 1) L)))',None),
 ('(defun od (N L) (if (= N 0) (tl L) (ev (- N 1) L)))',None),
 ('(ev 1000001 (cons 1 (cons 2 ())))','(2)'),
 # A running closure outlives its last other owner.
 ('(defun mk (L) (lambda Z (do (set held 0) (do (shen.gc) (hd L)))))',None),
 ('(set held (mk (cons 7 ())))',None),
 ('((value held) 0)','7'),
 # Aliases never see reuse; unique inputs still produce the right values.
 ('(defun inc (L) (if (cons? L) (cons (+ 1 (hd L)) (inc (tl L))) ()))',None),
 ('(let L (cons 1 (cons 2 ())) (let R (inc L) (cons L R)))','((1 2) 2 3)'),
 ('(inc (inc (cons 1 ())))','(3)'),
 ('(inc 5)','()'),
 ('(defun app (X Y) (if (cons? X) (cons (hd X) (app (tl X) Y)) Y))',None),
 ('(let A (cons 1 ()) (let B (app A A) (cons A B)))','((1) 1 1)'),
 ('(app (app (cons 1 ()) (cons 2 ())) (cons 3 ()))','(1 2 3)'),
 ('(defun swap (L) (cons (tl L) (hd L)))',None),
 ('(swap (cons 1 2))','(2 | 1)'),
 ('(let L (cons 1 2) (cons (swap L) L))','((2 | 1) 1 | 2)'),
 ('(defun both (L) (cons (hd L) (cons (hd L) (tl L))))',None),
 ('(both (cons 1 ()))','(1 1)'),
 ('(defun keephd (L) (cons (hd L) ()))',None),
 ('(let P (cons (cons 1 ()) 2) (let R (keephd P) (cons R P)))','(((1)) (1) | 2)'),
 ('(defun twice (L) (cons L L))',None),
 ('(twice (cons 1 ()))','((1) 1)'),
 ('(defun trapping (L) (cons (hd L) (trap-error (simple-error "x") (lambda E (tl L)))))',None),
 ('(let L (cons 1 (cons 2 ())) (cons (trapping L) L))','((1 2) 1 2)'),
 ('(defun raises (L) (cons (hd L) (simple-error "after hd")))',None),
 ('(let L (cons 1 ()) (trap-error (raises L) (lambda E L)))','(1)'),
 ('(trap-error (raises (cons 1 ())) (lambda E (error-to-string E)))','"after hd"'),
 ('(trap-error (hd 5) (lambda E 0))','0'),
 # Partial, over and higher-order application of inferred signatures.
 ('(defun add3 (A B C) (+ A (+ B C)))',None),
 ('((add3 1) 2 3)','6'),
 ('(((add3 1) 2) 3)','6'),
 ('(defun const2 (A) (lambda B A))',None),
 ('(const2 1 2)','1'),
 ('(defun apply1 (F X) (F X))',None),
 ('(let L (cons 1 ()) (cons (apply1 (lambda Y (cons? Y)) L) L))','(true 1)'),
 ('(apply1 (lambda Y Y) (cons 1 ()))','(1)'),
 # Primitives replaced after callers compiled, with borrowed inputs.
 ('(defun second (L) (hd (tl L)))',None),
 ('(defun tl (X) (cons 9 X))',None),
 ('(second 5)','9'),
 ('(defun cons (A B) A)',None),
 ('(app 1 2)','2'),
 ('(+ 1 1)','2'),
]

# Each program and its output, from the base 8e2327e binary. Fixtures:
# fusion must leave replacements of hd and tl an intact pair, also when the
# replacement arrives between the two projections; a closure's last-use capture
# survives the call, so a second call sees it; (f v (g v)) holds v until the call.
GROUPS={
 'late':('(defun two (A B) (cons A B))\n(defun g1 (X) (hd X))\n(defun latet (V) (two V (g1 V)))\n(defun lateb (V) (hd (two V (g1 V))))\n(defun lateh (F V) (tl (two F (F V))))\n(defun late2 (V) (hd (two (g1 V) V)))\n(defun late3 (V) (hd (tl (two V (two V (g1 V))))))\n(latet (cons 1 ()))\n(lateb (cons 1 ()))\n(hd (lateh (lambda Z (cons Z ())) (cons 1 ())))\n(late2 (cons 1 ()))\n(late3 (cons 1 ()))\n(defun capk (L) (lambda Z L))\n(let C (capk (cons 1 ())) (cons (C 0) (C 0)))\n(defun caph (L) (lambda Z (hd L)))\n(let C (caph (cons 1 ())) (cons (C 0) (C 0)))\n(defun capt (L) (lambda Z (two L L)))\n(let C (capt (cons 1 ())) (cons (C 0) (C 0)))\n(let V (absvector 1) (do (address-> V 0 (cons 1 ())) (let A (<-address V 0) (let B (<-address V 0) (cons A (cons B (<-address V 0)))))))\n(let S "s" (let N (cn S S) (cons N S)))\n(defun sw (L) (cons (hd L) (tl L)))\n(let L (cons 1 2) (cons (sw L) L))\n(defun sw2 (L) (two (hd L) (tl L)))\n(let L (cons 1 2) (cons (sw2 L) L))\n(sw2 (cons 1 2))\n(defun last1 (L) (two (tl L) 0))\n(let L (cons 1 2) (cons (last1 L) L))\n(last1 (cons 1 2))\n(defun lets (L) (let H (hd L) (let T (tl L) (cons H T))))\n(let L (cons 1 2) (cons (lets L) L))\n(lets (cons 1 2))\n(defun letb (L C) (let H (hd L) (if C (cons H (tl L)) H)))\n(letb (cons 1 2) true)\n(letb (cons 1 2) false)\n(defun letn (L) (let T (tl L) (let B (cons 0 T) (cons B B))))\n(letn (cons 1 2))\n(defun letx (L) (let T (tl L) (trap-error (simple-error "x") (lambda E (cons T T)))))\n(letx (cons 1 2))\n(defun t1 (L) (let A (cons 1 2) (let T (tl L) (cons A T))))\n(t1 (cons 9 7))\n','((1) | 1)\n(1)\n(1)\n1\n(1)\n((1) 1)\n(1 | 1)\n(((1) 1) (1) 1)\n((1) (1) 1)\n("ss" | "s")\n((1 | 2) 1 | 2)\n((1 | 2) 1 | 2)\n(1 | 2)\n((2 | 0) 1 | 2)\n(2 | 0)\n((1 | 2) 1 | 2)\n(1 | 2)\n(1 | 2)\n1\n((0 | 2) 0 | 2)\n(2 | 2)\n((1 | 2) | 7)\n'),
 'fuse-tl':('(defun f (L) (let A (hd L) (let B (tl L) (cons A B))))\n(defun g (L) (cons (hd L) (tl L)))\n(defun h (L) (cons (tl L) (hd L)))\n(defun m (F L) (cons (F (hd L)) (tl L)))\n(defun b (L) (cons (hd L) (do (defun tl (X) (hd X)) (tl L))))\n(f (cons 1 2))\n(g (cons 1 2))\n(h (cons 1 2))\n(b (cons 1 2))\n(f (cons 1 2))\n(g (cons 1 2))\n(h (cons 1 2))\n(m (lambda X X) (cons 1 2))\n','(1 | 2)\n(1 | 2)\n(2 | 1)\n(1 | 1)\n(1 | 1)\n(1 | 1)\n(1 | 1)\n(1 | 1)\n'),
 'fuse-hd':('(defun f (L) (let B (tl L) (let A (hd L) (cons A B))))\n(defun g (L) (cons (tl L) (hd L)))\n(defun c (L) (let A (tl L) (do (defun hd (X) (tl X)) (cons A (hd L)))))\n(defun r (L) (let A (hd L) (do (defun tl (X) 7) (cons A (tl L)))))\n(f (cons 1 2))\n(c (cons 1 2))\n(f (cons 1 2))\n(g (cons 1 2))\n','(1 | 2)\n(2 | 2)\n(2 | 2)\n(2 | 2)\n'),
 'literal':('(defun app (X Y) (if (cons? X) (cons (hd X) (app (tl X) Y)) Y))\n(defun lit () (cons 1 (cons 2 ())))\n(defun bump (L) (cons (+ 1 (hd L)) (tl L)))\n(bump (lit))\n(lit)\n(defun ident (X) X)\n(ident true)\n(boolean? true)\n(ident foo)\n(symbol? foo)\n(= 1.0 1)\n(+ 1.0 1)\n(ident 1.5)\n(ident "s")\n(defun cap (L) (lambda Z (bump L)))\n(let C (cap (cons 1 ())) (cons (C 0) (C 0)))\n(defun vec (V) (bump (<-address V 0)))\n(let V (absvector 1) (do (address-> V 0 (cons 1 (cons 2 ()))) (let R (vec V) (cons R (<-address V 0)))))\n(set g (cons 1 (cons 2 ())))\n(defun gl () (bump (value g)))\n(cons (gl) (value g))\n(defun bumpset (L) (do (set g 0) (bump L)))\n(bumpset (value g))\n(trap-error (bump 5) (lambda E (error-to-string E)))\n(defun tf (L) (cons (hd L) (hd (tl L))))\n(trap-error (tf (cons 1 2)) (lambda E (error-to-string E)))\n(defun tr (L) (trap-error (cons (hd L) (simple-error "x")) (tl L)))\n(tr (cons (lambda Z 1) (lambda Z 2)))\n(defun tr2 (L) (trap-error (let A (tl L) (simple-error "x")) (hd L)))\n(tr2 (cons (lambda E 77) 2))\n(defun tr3 (L) (trap-error (let A (hd L) (simple-error "x")) (lambda E (hd L))))\n(tr3 (cons 1 2))\n','(2 2)\n(1 2)\ntrue\ntrue\nfoo\ntrue\ntrue\n2.0\n1.5\n"s"\n((2) 2)\n((2 2) 1 2)\n(1 2)\n((2 2) 1 2)\n(2 2)\n"hd expects a nonempty list"\n"hd expects a nonempty list"\n2\n77\n1\n'),
}
STATS={
 # Reused cell must learn it now reaches a vector, or this cycle is never freed.
 'cycle':("""(defun keep (V L) (cons V (tl L)))
(defun mkc (L) (let V (absvector 1) (do (address-> V 0 (keep V L)) 0)))
(defun rep (N) (if (= N 0) 0 (do (mkc (cons 1 ())) (rep (- N 1)))))
(rep 100)
(shen.gc)
""",lambda s:s['pair_reuses']>=100 and s['cycle_objects_freed']>=200),
 # The head list dies at the tl projection, before the second list is built.
 'drop-before-build':("""(defun mk (N A) (if (= N 0) A (mk (- N 1) (cons N A))))
(defun k (L N) (cons (tl L) (mk N ())))
(defun len (L A) (if (cons? L) (len (tl L) (+ A 1)) A))
(len (tl (k (cons (mk 30000 ()) ()) 30000)) 0)
""",lambda s:s['peak_objects']<45000),
 # A pair whose last projection precedes the cons is still the reuse token.
 'let-token':("""(defun fresh (N A) (if (= N 0) A (fresh (- N 1) (cons N A))))
(defun app2 (X) (if (cons? X) (let H (hd X) (let T (tl X) (cons H (app2 T)))) ()))
(defun walk (N L) (if (= N 0) (hd L) (walk (- N 1) (app2 L))))
(walk 100 (fresh 100 ()))
""",lambda s:s['pair_reuses']>=9000),
}
source='\n'.join(c[0] for c in cases)+'\n'
with tempfile.TemporaryDirectory(prefix='rc-perceus.') as temp:
    work=Path(temp);inputs=work/'input';inputs.mkdir();(inputs/'kernel.kl').write_text('');(inputs/'user.kl').write_text(source)
    env=dict(os.environ,SHEN_C_HOME=str(root))
    for mode,flags in [('optimized',[])]:
        dest=work/mode
        built=subprocess.run([os.environ['RC_PERCEUS_BUILDER'],*flags,str(inputs),str(dest)],env=env,capture_output=True,text=True,timeout=300)
        assert built.returncode==0,(mode,built.stdout,built.stderr)
        ran=subprocess.run([str(dest/'app')],env=dict(env,SHEN_RC_PRINT='1'),capture_output=True,text=True,timeout=120)
        assert ran.returncode==0,(mode,ran.stdout,ran.stderr)
        lines=ran.stdout.splitlines();tops=[c for c in cases if not c[0].startswith('(defun')]
        assert len(lines)==len(tops),(mode,len(lines),len(tops),ran.stderr)
        for c,got in zip(tops,lines):
            assert c[1] is None or got==c[1],(mode,c[0],'want',c[1],'got',got)
    # The lowering the semantics above must survive, and proof that reuse fires.
    (inputs/'user.kl').write_text('''(defun fresh (N A) (if (= N 0) A (fresh (- N 1) (cons N A))))
(defun app (X Y) (if (cons? X) (cons (hd X) (app (tl X) Y)) Y))
(defun walk (N L) (if (= N 0) (hd L) (walk (- N 1) (app L ()))))
(walk 100 (fresh 100 ()))
''')
    dest=work/'reuse'
    built=subprocess.run([os.environ['RC_PERCEUS_BUILDER'],str(inputs),str(dest)],env=env,capture_output=True,text=True,timeout=300)
    assert built.returncode==0,(built.stdout,built.stderr)
    code=(dest/'app.c').read_text()
    for needed in ('rc_cons_call_reuse_owned(','rc_primitive_hd_take(','rc_primitive_tl_last(','rc_condition_eq_borrowed('):
        assert needed in code,needed
    ran=subprocess.run([str(dest/'app')],env=dict(env,SHEN_RC_PRINT='1',SHEN_RC_STATS='1'),capture_output=True,text=True,timeout=120)
    assert ran.returncode==0 and ran.stdout=='1\n',(ran.stdout,ran.stderr)
    stats=json.loads(ran.stderr[ran.stderr.index('{'):ran.stderr.index('}')+1])
    # 100 walks over 100 unique cells: nearly every cons is an in-place rewrite.
    assert stats['pair_reuses']>=9000,stats
    assert stats['live_objects']<2000,stats
    for name,(program,want) in GROUPS.items():
        (inputs/'user.kl').write_text(program);dest=work/name
        built=subprocess.run([os.environ['RC_PERCEUS_BUILDER'],str(inputs),str(dest)],env=env,capture_output=True,text=True,timeout=300)
        assert built.returncode==0,(name,built.stdout,built.stderr)
        ran=subprocess.run([str(dest/'app')],env=dict(env,SHEN_RC_PRINT='1'),capture_output=True,text=True,timeout=120)
        assert ran.returncode==0 and ran.stdout==want,(name,ran.returncode,ran.stdout,want,ran.stderr)
    for name,(program,check) in STATS.items():
        (inputs/'user.kl').write_text(program);dest=work/name
        built=subprocess.run([os.environ['RC_PERCEUS_BUILDER'],str(inputs),str(dest)],env=env,capture_output=True,text=True,timeout=300)
        assert built.returncode==0,(name,built.stdout,built.stderr)
        ran=subprocess.run([str(dest/'app')],env=dict(env,SHEN_RC_STATS='1'),capture_output=True,text=True,timeout=120)
        assert ran.returncode==0,(name,ran.stderr)
        stats=json.loads(ran.stderr[ran.stderr.index('{'):ran.stderr.index('}')+1])
        assert check(stats),(name,stats)
print('RC perceus semantics: redefinition, raises through borrowed frames, deep recursion, aliases PASS')
PY
