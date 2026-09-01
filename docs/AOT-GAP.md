# AOT runme gap vs shen-go — sample, not emit churn

Stage: `o7-apply-direct-lex-env` + `o6-rebar` + `aot-overlay-killed` +
`o4-prim-inline-shipped` + `tc-cache-shipped`.
This file is the proof that matching Go’s **~5 s** suite wall is **not** a
cheap CFLAGS / intern-leaf / mmap-per-tail patch, **and not** a rust-style
load-then-swap overlay of sidecar defuns. O7 (this job) shipped rust
analogs that **are** worth doing on the live sample: `shen_apply_direct`
+ intern-static cache, lexical env as `EnvBinding` vec-by-SymId with a
bound-id slot (no miss cache), cons still one Boehm `KLObject` (no
separate `Pair*`; TLS cons slabs were tried and **killed** — Boehm does
not treat `__thread` as a root). Both bars **134/0**. Cold AOT `runme`
wall **8.33 s** / L-interp **6.555 s** (`1177672` inf) vs Go **4.94 s** /
**2.211 s** (`1178236` inf). That is **low-teens-or-better, not 5 s**:
still **~1.7×** wall and **~3.0×** L-interp. Overlay stays **unplugged**
(`overlay_wrap=0`). O5 tc-cache remains env-gated; warm L-interp can
match Go, cold one-shot does not. Do not unbox fib. Do not C++. Do not
implement rung 2. Do not JIT `t*`. Do not bytecode-VM one-shot runme.
Do not start `Value(u64)` / tagged immediates. Keep Boehm. Keep both
**134/0** bars.

`cheap[]` is empty. The live Darwin sample of `bin/runme-aot-app` during
L interpreter / Prolog interpreter **unblesses** those three flag/intern/mmap
ideas. O1+O2 overlay (bootstrap KL of `interpreter.shen` / `prologinterp.shen`,
load sidecar for datatype/`define` effects, then swap defun cells on
source-hash + kernel-digest + arity match) was wired into runme-aot and
**killed**: L-interp typecheck stayed **16.54–17.16 s** with overlay vs
baseline **16.182 s** (`1177672` inf). After unplug (`evidence/runme-aot.log`
`21:10:54Z`, still **134/0**): **16.845703 s** / **16.845723 s** after
inferences. `load` prints `run time` around `shen.load-help` *before* any
post-load swap; that clock is typecheck of the sidecar, not the later
`normal-form` calls (~0.0005 s). Overlay emit/install remain in `abi.c` /
`emit.c` / tests; runme-aot no longer registers or wraps `load`.
Structural work (faster already-AOT `t*` / Prim*-style boxed ops, or
tc-cache) is later. Do not re-litigate overlay-after-load for this wall.

## Bars (do not regress)

| bar | evidence | status |
|-----|----------|--------|
| AOT runme **passed 134 / failed 0** | `evidence/runme-aot.log` (`app_pipe_exit=0`, SHA `0ebea4da80c9cec0d0102c6e1bf39d0f975e4cd3`, `00:28:53Z`–suite end, **8.33** real) | keep |
| Interpreter certify **passed 134 / failed 0** | `evidence/certify.log` `00:29:27Z`–`00:30:07Z`, Shen **38.92 s**, `exit=0` | keep; **re-ran this job** (touched evaluator/env) |
| Boehm | `otool -L bin/runme-aot-app/app` → Nix `libgc.1.dylib` | stays |
| Fib AOT **0.01 s** | shaken-boot (`test/fixtures/fib-ygg`, 55 defuns) | **not the gap** |

Do not treat `evidence/sample-interpreter.txt` as this binary. That file is
tree-walker `bin/shen-c` pid **89994** (`eval_kl_object` of the interpreter),
not `bin/runme-aot-app`. Live AOT sample extract:
`evidence/sample-runme-aot-summary.txt` (pid **24166**).

## Thesis

AOT runme is already **134/0**. Matching Go on wall is still **native
typecheck of the sidecars**, not another trampoline. O7 cut intern+apply
churn on already-AOT calls (`apply_direct` + intern-static) and made
`lookup_environment` id-indexed with a bound-id slot — enough to move
cold L-interp **15.35 s → 6.55 s**, not enough to 5 s. `interpreter.shen`
/ `prologinterp.shen` remain **runtime load sidecars** whose typecheck
walks boxed KL (`eval_kl_object` + `lookup_environment`). It is **not**
`-O2` vs `-O3`, **not** mmap-per-tail, **not** overlay-after-load, **not**
fib unbox, **not** C++, **not** rung 2, **not** tagged immediates.

## Timed windows (quoted)

O7 apply_direct + lex env (`evidence/runme-aot.log`,
`start_utc: 2026-09-01T00:28:53Z`, `cwd` `shen/test/s42`, `defuns=597`,
`eval_kl_object=0` in generated `app.c`, `overlay_wrap=0`, intern **2426**,
`apply_direct` **4259**, `shen_apply(` **96**, SHA
`0ebea4da80c9cec0d0102c6e1bf39d0f975e4cd3`):

```
L interpreter: (load "interpreter.shen") = loaded
run time: 6.554886 secs
typechecked in 1177672 inferences

Prolog interpreter: (load "prologinterp.shen") = loaded
run time: 1.290217 secs
typechecked in 1634304 inferences

passed ... 134
failed ... 0

run time: 11.822422 secs
        8.33 real        11.24 user         0.61 sys
app_pipe_exit=0
```

Go same suite (`evidence/compare-runme-shen-go.log`,
`start_utc: 2026-09-01T00:29:13Z`):

```
L interpreter: (load "interpreter.shen") = loaded
run time: 2.2113229580000002 secs
typechecked in 1178236 inferences

Prolog interpreter: (load "prologinterp.shen") = loaded
run time: 0.47066749999999935 secs
typechecked in 1634868 inferences

passed ... 134
failed ... 0

run time: 4.729523416 secs
        4.94 real         7.48 user         0.19 sys
```

Interpreter certify re-run (`evidence/certify.log`, tree-walker `bin/shen-c`,
`start_utc: 2026-09-01T00:29:27Z`):

```
passed ... 134
failed ... 0

run time: 38.865483 secs
loaded

run time: 38.919785 secs
exit=0
end_utc: 2026-09-01T00:30:07Z
```

Certify L interpreter typecheck **20.870 s** / **1177672** inf; prologinterp
**3.890 s** / **1634304** inf (prior certify L-interp **46.93 s**, Shen
**91.27 s**). Wall variance is real, bar is **134/0**.

Prior O6 cold AOT (`21:52:10Z`): **15.31** real, L-interp **15.351 s**.
O4 unsampled (`23:50:26Z`): L-interp **13.624 s**, **18.52** real. O7 is
**8.33** real / **6.555 s** L-interp vs Go **4.94** / **2.211 s**: about
**1.7×** wall, **3.0×** L-interp, **not 5 s**. User>real on AOT is still
trampoline hops (pthread), not proof mmap is the hotspot. Overlay remains
killed.

Generated intern **7465→2426** (named callees no longer intern at the
call site). Named calls emit `shen_apply_direct` / `shen_tail_apply_direct`
(stack arg array + intern-static); `shen_apply(` remains for computed
heads (**96**). `apply_direct` still enters `shen_apply` so
`apply_depth` / tail bounce / hop stay correct — skipping that loop
SIGSEGV’d hello-ygg at `shen.change-pointer-value`.

## Live sample (quoted)

Live **2026-08-31** Darwin arm64. Job: `time yes|bin/runme-aot-app` vs
shen-go `script runme` (cwd `shen/test/s42`). Sample AOT pid **30 s** during
L interpreter / prologinterp. Bars kept: AOT runme 134/0 and interpreter
certify 134/0 (certify **re-ran** this job; `21:52:53Z`–`21:54:26Z` Shen
**91.27 s**). Boehm stays (`otool libgc.1.dylib`). No fib unbox, no C++,
no rung 2. Fib **0.01 s** is shaken-boot (55 defuns), not the gap.

Emit intern/apply (`src/c/emit.c`): O7 named calls emit
`shen_apply_direct` / `shen_tail_apply_direct` (intern-static + stack
args). Unbound *value* atoms still intern. Self-tails `goto`. O6 sample
binary (`intern=14852` `apply=11515`) is **not** this binary; live O7
`app.c` is intern **2426** `apply_direct` **4259**. `yggdrasil-build.c` Makefile `CFLAGS ?= -O2`
(top-level Makefile `CFLAGS ?= -O3` for `libshenc`). `abi.c`
`apply_on_fresh_stack`: `mmap(16MiB+guard)+pthread_attr_setstack+GC_pthread_create/join`
per hop when generated `NativeFunction` and stack low — not on-CPU in this
sample.

Wall times (`yes | /usr/bin/time -l`; this rebar `app_pipe_exit=0`;
earlier logs may show pipe exit **141** from `yes` SIGPIPE after 134/0 ok):

- AOT clean O7 (this job, `00:28:53Z`): **8.33** real / **11.24**
  user / **0.61** sys, Shen **11.822 s**, RSS **59.8 MiB** (`62734336`),
  passed **134** / failed **0**. L interpreter typecheck **6.555 s** /
  **1177672** inf. Prologinterp **1.290 s** / **1634304** inf.
  intern **2426**, apply_direct **4259**. `overlay_wrap=0`. Boehm
  `libgc.1.dylib`. **Not 5 s** (Go this job **4.94** real / L-interp
  **2.211 s**).
- AOT clean O6 rebar (no sample, `21:52:10Z`): **15.31** real / **26.73**
  user / **1.24** sys, Shen **27.895 s**, RSS **69.2 MiB** (`72613888`),
  **306846384894** inst, passed **134** / failed **0**. L interpreter
  typecheck **15.351 s** / **1177672** inf. Prologinterp typecheck
  **2.855 s** / **1634304** inf. `overlay_wrap=0`. Boehm
  `libgc.1.dylib`.
- AOT sampled (pid **24166**, sample attached at L interpreter load; no
  `/usr/bin/time` parent): Shen **32.969 s**, L interpreter **18.850 s** /
  **1177672** inf, prologinterp **3.057 s** / **1634304** inf, still
  **134/0**.
- Go clean O6 rebar `.bin/shen-go script runme.shen` (`21:52:35Z`): **4.89**
  real / **7.36** user / **0.15** sys, Shen **4.670 s**, RSS **216.4 MiB**
  (`226869248`), L interpreter typecheck **2.161 s** / **1178236** inf,
  prologinterp **0.477 s** / **1634868** inf, **134/0**.
- Prior AOT (`19:32Z`): **15.13** real / **27.92** user, Shen **29.046 s**,
  L-interp **16.182 s**. Prior Go (`19:33Z`): **4.57** / **7.20**, Shen
  **4.391 s**, L-interp **2.041 s**. `option5-profile` (`18:58Z`): AOT
  **15.36** / **25.19** Shen **26.14 s**; Go **5.37** / **7.80** Shen
  **5.05 s**. Variance high; AOT still **~3×** Go wall, not a match.
  User>real on AOT is trampoline hops (pthread), not proof mmap is the
  hotspot.

Sample: `/usr/bin/sample 24166 30 1` starting **2026-08-31 14:35:59 -0500**
when log hit `L interpreter: (load "interpreter.shen") = loaded`.
**10966** samples/thread (~11 s of remaining life; 30 s window outlived the
process; captured L interpreter typecheck and into later tests/prologinterp).
Main + 9 GC-marker threads. Extract:
`evidence/sample-runme-aot-summary.txt`.

Quoted top of main call graph: **3169/10966 = 28.9%**
`start → main → shen_apply → apply_exact → native_kl_load_324 → primitive_function_map → lambda_66 → shen_eval_kl → eval_kl_object`
(sidecar load, not AOT of `interpreter.shen`). Remaining **71.1%** are
truncated stacks (no `start`/`main`) rooted at `eval_kl_object` /
`eval_let_expression` / `shen_apply` / `apply_exact` /
`native_kl_shen_2esystem_2dS_2dh_497` / `search_user_datatypes` — hops onto
fresh C stacks, mmap itself not sampled.

Exclusive top-of-stack (same collapsed, main **10966**): `__psynch_cvwait`
**87153** is GC-marker idle (9 threads), ignore for app CPU. App:
`lookup_environment` **1746 (15.9%)**, `GC_malloc_kind` **1173 (10.7%)**,
`eval_kl_object` **655 (6.0%)**, `intern_kl_string` **545 (5.0%)**,
`shen_apply` **441 (4.0%)**, `apply_exact` **355 (3.2%)**,
`eval_symbol_function_application` **249 (2.3%)**, `shen_intern` **226 (2.1%)**,
`GC_generic_malloc_many` **168 (1.5%)**. Recursive-on-stack (>=5):
`eval_kl_object` 580565+450761+250245, `eval_let_expression` 552716,
`shen_apply` 327579, `apply_exact` 233629,
`eval_symbol_function_application` 107938+95273,
`native_kl_shen_2ebind_21_390` 82723,
`native_kl_shen_2esystem_2dS_2dh_497` 23360,
`native_kl_shen_2esearch_2duser_2ddatatypes_505` 22496,
system-S 22487. `shen_intern` recursive 807+226;
`intern_kl_string` 544+358.

Cheap wins only if sample names them — it does not: (1) CFLAGS `-O2` vs `-O3`
is a build flag, zero sample symbols; (2) intern-every-prim is real in emit
(14852 sites) but exclusive intern+`intern_kl_string` ≈ **7.0%**, not the
gap; (3) mmap stack per tail: `apply_on_fresh_stack`/`mmap`/`munmap`/
`GC_pthread_create`/`native_trampoline_worker` = **0** samples;
`shen_tail_apply` **9 (0.08%)**. Structural later: matching Go on runme is
native typecheck / less intern+apply, not another trampoline. Proof: L
interpreter is a runtime load sidecar whose typecheck is `eval_kl_object` +
`lookup_environment` + system-S-h/`t*` via `shen_apply` of boxed KL,
**16.2 s** vs Go native **2.04 s** on the same **1.18 M** inferences. Do
not chase trampoline, fib unbox, C++, or rung 2.

## Why `cheap[]` is empty (code the sample did not bless)

### 1. Generated `CFLAGS ?= -O2` vs `libshenc` `-O3`

`tools/yggdrasil-build.c` writes `CFLAGS ?= -O2` into
`bin/runme-aot-app/Makefile`. Top-level `Makefile` uses `CFLAGS ?= -O3` for
`bin/libshenc.a` / `bin/shen-c`. The sample has **zero** symbols that are
an optimization-level leaf. Flipping the generated flag is a rebuild, not
a named hotspot.

### 2. Intern every prim

Unbound atoms intern (`src/c/emit.c` `emit_atom`):

```c
cbuf_printf(e->stmt, "  KLObject* t%d = shen_intern(ctx, ", t);
```

Calls allocate a `Vector` then `shen_apply` / `shen_tail_apply`. Live
`native_kl_shen_2et_2a_507` (AOT kernel `t*`) starts:

```c
KLObject* t31257 = shen_intern(ctx, "+");
KLObject* t31259 = shen_number_l(ctx, 1);
/* Vector of 2 → shen_apply  — this is (+ Infs 1), not PrimNumberAdd */
```

`intern_kl_string` already caches via `lookup_string_table`. Exclusive
`intern_kl_string`+`shen_intern` ≈ **7.0%**. Caching intern harder does not
close 3×.

### 3. mmap stack per tail

`src/c/abi.c` `apply_on_fresh_stack` mmaps **16 MiB** + guard, then
`pthread_attr_setstack` + `GC_pthread_create`/`join` when a generated
`NativeFunction` is applied with stack low. That is a **hop**, not every
`shen_tail_apply` (`shen_tail_apply` bounces). Sample:
`apply_on_fresh_stack` / `mmap` / `munmap` / `GC_pthread_create` /
`native_trampoline_worker` = **0**. Go already spends **55%** cum in
`kl.apply`+trampoline. Matching Go is MakeNative / `types.go` / `t-star.go`
class, not hop rewrite.

## Structural (later; not this file)

1. Emit Prim*-style boxed ops for `+` / `cons` / `=` / `hd` / `tl`
   (direct `shen_cons` / `shen_hd` / number add, not intern symbol +
   `shen_vector` + `shen_apply`). Go: `codegen/codegen.go`
   `PrimNumberAdd` / `PrimCons` / `PrimEqual`; `cmd/shen/t-star.go`
   `PrimNumberAdd(V, MakeNumber(1))`; `kl/types.go` `MakeInteger` fixnum.
2. Faster already-AOT `t*` (O4 prim inline / less intern+apply), not
   overlay of the sidecar's own defuns. L interpreter / prologinterp stay
   `eval_kl_object`+`lookup_environment` via `load`/`map`/`eval-kl`
   (**28.9%** load graph; exclusive `lookup_environment` **15.9%**)
   during `shen.load-help`. Kernel `t*` is already a `NativeFunction`.
   Load-then-swap overlay of `interpreter.shen` defuns was measured and
   **killed** (O2 gate): wall **16.54–17.16 s**, inferences unchanged.
   Go `cmd/shen` typechecks loaded files as native. O5 tc-cache is
   env-gated (`SHEN_C_TC_CACHE`); warm L-interp **2.040 s** / **14/14**
   replay (`evidence/runme-aot-tc-warm.log`). Do not cache `shen->kl`.
3. Do **not** chase another trampoline.

Kernel `t*` / `typecheck` being NativeFunctions is already true
(`shen_register_defun(ctx, "shen.t*", 6, &native_kl_shen_2et_2a_507)`).
The gap is the **sidecar** walking boxed KL through those natives, plus
prims still going intern+apply.

## How to run (exact)

AOT runme (the 134/0 bar this file quotes):

```
cd /Users/reuben/projects/shen-c/shen/test/s42
yes | /usr/bin/time -l /Users/reuben/projects/shen-c/bin/runme-aot-app/app
```

Warm O5 tc-cache (second process; first records):

```
CACHE=/tmp/shen-c-tc-runme
mkdir -p "$CACHE"
cd /Users/reuben/projects/shen-c/shen/test/s42
yes | env SHEN_C_TC_CACHE="$CACHE" SHEN_C_TC_CACHE_STATS=1 \
  /usr/bin/time -l /Users/reuben/projects/shen-c/bin/runme-aot-app/app
```

Expect `passed ... 134`, `failed ... 0`. `yes` SIGPIPE after the suite
(exit **141** on the pipe) is ok if the app itself printed 134/0
(`evidence/runme-aot.log` wrapped that as `exit=0`).

Go same suite:

```
cd /Users/reuben/projects/shen-c/shen/test/s42
yes | /usr/bin/time -l /Users/reuben/projects/shen-go/.bin/shen-go script runme.shen
```

Interpreter certify (re-run this job):

```
cd /Users/reuben/projects/shen-c
nix develop -c make CC=clang certify
```

Sample AOT during L interpreter / prologinterp (Darwin). Sample the **app**
pid, not `/usr/bin/time`:

```
cd /Users/reuben/projects/shen-c
# start yes | bin/runme-aot-app/app from shen/test/s42 (no time wrapper)
# then: /usr/bin/sample <app-pid> 30 1 when the log hits
# L interpreter: (load "interpreter.shen") = loaded
bash evidence/sample-runme-aot.sh
```

Rebuild of the app is `nix develop -c make CC=clang bin/runme-aot-app` from
the repo root (fixture `test/fixtures/runme-aot`). Relink after `libshenc`
changes (`rm bin/runme-aot-app/app` if the fixture did not change).

## What this job did

- `shen_apply_direct` / `shen_tail_apply_direct`: named AOT calls intern
  via a pointer cache of C string literals (`shen_intern_static`) and pass
  a stack `KLObject*[]` (no `shen_vector` at the call site). Dispatch still
  goes through `shen_apply` so tail bounce and trampoline hops work.
- Environment frames store `EnvBinding {id, value}` (vec-by-SymId). Bound
  locals also sit in a TLS slot keyed by id; **misses are not cached**
  (caching NULL made `(cn X "!")` typecheck as `number --> number`).
  `trap-error` rewinds the slot on throw.
- Cons stays **one** Boehm `KLObject` with an inline pair. A TLS cons/number
  slab was measured then **killed**: interior cells plus unscanned TLS
  SIGSEGV’d hello-ygg.
- Overlay stays **killed**. No JIT `t*`, no bytecode VM for one-shot runme,
  no `Value(u64)` / tagged immediates, no fib unbox, no C++, no rung 2.
- AOT runme **134/0** (`evidence/runme-aot.log`): L-interp **6.555 s**,
  wall **8.33 s**. Go **2.211 s** / **4.94 s**. Certify **134/0**, Shen
  **38.92 s** (`evidence/certify.log`). Honest: **not 5 s**.

## What this job did not do

- Overlay emit/install remain in `abi.c` / `emit.c` / tests; runme-aot
  does **not** register overlays or wrap `load` (`overlay_wrap=0`).
- No CFLAGS flip in `tools/yggdrasil-build.c`.
- No `apply_on_fresh_stack` / mmap / pthread rewrite.
- No JIT of `t*`, no bytecode VM for one-shot runme, no `Value(u64)`.
- No Boehm removal, no fib unbox, no C++, no rung 2 (`docs/rung2.md` remains
  documentation only).
- Tagged immediates stay a separate heap project.
