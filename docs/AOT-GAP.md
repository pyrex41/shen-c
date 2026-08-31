# AOT runme gap vs shen-go — sample, not emit churn

Stage: `o6-rebar` + `aot-overlay-killed` + `o4-prim-inline-shipped` +
`tc-cache-shipped`.
This file is the proof that the remaining **~3×** wall on AOT `runme` is
**not** a cheap CFLAGS / intern-leaf / mmap-per-tail patch, **and not** a
rust-style load-then-swap overlay of sidecar defuns. O6 rebar (this job)
re-timed clean AOT vs Go and re-ran certify: both **134/0**. One-shot
L-interp is still **15.351 s** (`1177672` inf) vs Go **2.161 s**
(`1178236` inf). Overlay stays **unplugged** (`overlay_wrap=0` in
`evidence/runme-aot.log`): the O2 kill gate is L-interp seconds, not
matching Go’s 5 s suite wall. O4 inlined exact-arity `+` `-` `cons`
`hd` `tl` in `emit.c`. Generated intern **14852→9150**, apply
**11515→6030**; one-shot L-interp **16.182 s → 15.351 s**, still **~16 s**.
O5 tc-cache (verdict memo, not `shen->kl`) is the lever that moves
L-interp on a **warm** disk cache: **16.182 s → 2.040 s** (Go **2.041 s**),
AOT still **134/0**. Off by default (`SHEN_C_TC_CACHE=<dir>`). Cold
one-shot records and is not faster. Do not unbox fib. Do not C++. Do
not implement rung 2. Do not start O3 `apply_direct`. Keep Boehm. Keep
both **134/0** bars.

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
| AOT runme **passed 134 / failed 0** | `evidence/runme-aot.log` (`app_pipe_exit=0`, SHA `8c62fea4b82dc3af33d2d09738caf774f826f012`, `21:52:10Z`–`21:52:25Z`) | keep |
| Interpreter certify **passed 134 / failed 0** | `evidence/certify.log` `21:52:53Z`–`21:54:26Z`, Shen **91.27 s**, `exit=0` | keep; **re-ran this job** |
| Boehm | `otool -L bin/runme-aot-app/app` → Nix `libgc.1.dylib` | stays |
| Fib AOT **0.01 s** | shaken-boot (`test/fixtures/fib-ygg`, 55 defuns) | **not the gap** |

Do not treat `evidence/sample-interpreter.txt` as this binary. That file is
tree-walker `bin/shen-c` pid **89994** (`eval_kl_object` of the interpreter),
not `bin/runme-aot-app`. Live AOT sample extract:
`evidence/sample-runme-aot-summary.txt` (pid **24166**).

## Thesis

AOT runme is already **134/0**. Matching Go on wall is **native typecheck /
less intern+apply on already-AOT `t*` / `typecheck`**, plus the fact that
`interpreter.shen` / `prologinterp.shen` are **runtime load sidecars** whose
typecheck still walks boxed KL (`eval_kl_object` + `lookup_environment`).
It is **not** another trampoline, **not** intern-hash cache as a 3× closer,
**not** `-O2` vs `-O3`, **not** mmap-per-tail, **not** fib unbox, **not**
C++, **not** rung 2.

## Timed windows (quoted)

O6 rebar clean unsampled AOT (`evidence/runme-aot.log`,
`start_utc: 2026-08-31T21:52:10Z`, `cwd` `shen/test/s42`, `defuns=597`,
`eval_kl_object=0` in generated `app.c`, `shen_native_closure=282`,
`overlay_wrap=0`, intern **9150**, apply **6030**, `shen_add=41`,
`shen_cons=2923`, SHA `8c62fea4b82dc3af33d2d09738caf774f826f012`):

```
L interpreter: (load "interpreter.shen") = loaded
run time: 15.350518 secs
typechecked in 1177672 inferences

Prolog interpreter: (load "prologinterp.shen") = loaded
run time: 2.855191 secs
typechecked in 1634304 inferences

passed ... 134
failed ... 0

run time: 27.895186 secs
       15.31 real        26.73 user         1.24 sys
app_pipe_exit=0
end_utc: 2026-08-31T21:52:25Z
```

Go same suite (`evidence/compare-runme-shen-go.log`,
`start_utc: 2026-08-31T21:52:35Z`):

```
L interpreter: (load "interpreter.shen") = loaded
run time: 2.1612937920000004 secs
typechecked in 1178236 inferences

Prolog interpreter: (load "prologinterp.shen") = loaded
run time: 0.47708995899999973 secs
typechecked in 1634868 inferences

passed ... 134
failed ... 0

run time: 4.669908917000001 secs
        4.89 real         7.36 user         0.15 sys
app_pipe_exit=0
end_utc: 2026-08-31T21:52:40Z
```

Interpreter certify re-run (`evidence/certify.log`, tree-walker `bin/shen-c`,
`start_utc: 2026-08-31T21:52:53Z`):

```
passed ... 134
failed ... 0

run time: 91.158998 secs
loaded

run time: 91.266322 secs
exit=0
end_utc: 2026-08-31T21:54:26Z
```

Certify L interpreter typecheck **46.932444 s** / **1177672** inf; prologinterp
**13.340782 s** / **1634304** inf. Prior certify (`21:45:04Z`) was Shen
**77.06 s**; wall variance is real, bar is **134/0**.

tc-ygg (same typecheck class, `evidence/option5-profile.md`): AOT steady
**~4.6–5.3 s** vs Go **~1.7 s**. Full-kernel tree-walker vs Go apply
(`evidence/profile.md`): Go `kl.apply`+trampoline **55.2%** cum, `kl.vmExec`
**36.4%**; C tree-walker **45.3 s** vs Go **2.08 s** on that profile.

AOT stays **~3.1×** Go wall (**15.31** vs **4.89** real), not a match.
L-interp is **~7.1×** Go (**15.351 s** vs **2.161 s**) on the same
~1.18 M inferences. User>real on AOT is trampoline hops (pthread),
**not** proof mmap is the hotspot. Prior pass (`19:32Z`) was AOT
**15.13** / Go **4.57**; `18:58Z` was AOT **15.36** / Go **5.37**;
variance is real, ratio stays **~3×**. Overlay remains killed: 15.35 s
is still **~16 s**, not well under.

## Live sample (quoted)

Live **2026-08-31** Darwin arm64. Job: `time yes|bin/runme-aot-app` vs
shen-go `script runme` (cwd `shen/test/s42`). Sample AOT pid **30 s** during
L interpreter / prologinterp. Bars kept: AOT runme 134/0 and interpreter
certify 134/0 (certify **re-ran** this job; `21:52:53Z`–`21:54:26Z` Shen
**91.27 s**). Boehm stays (`otool libgc.1.dylib`). No fib unbox, no C++,
no rung 2. Fib **0.01 s** is shaken-boot (55 defuns), not the gap.

Emit intern/apply (`src/c/emit.c`): unbound symbols emit
`KLObject* tN = shen_intern(ctx, "...")`; calls emit Vector +
`shen_apply` / `shen_tail_apply` (self-tails `goto`). Generated `app.c`:
`intern=14852` `apply=11515` `tail_apply=1067` `eval_kl_object=0`
`shen_eval_kl=10`. `yggdrasil-build.c` Makefile `CFLAGS ?= -O2`
(top-level Makefile `CFLAGS ?= -O3` for `libshenc`). `abi.c`
`apply_on_fresh_stack`: `mmap(16MiB+guard)+pthread_attr_setstack+GC_pthread_create/join`
per hop when generated `NativeFunction` and stack low — not on-CPU in this
sample.

Wall times (`yes | /usr/bin/time -l`; this rebar `app_pipe_exit=0`;
earlier logs may show pipe exit **141** from `yes` SIGPIPE after 134/0 ok):

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
the repo root (fixture `test/fixtures/runme-aot`). This doc does **not**
rebuild.

## What this job did not do

- O6 rebar only: re-timed AOT runme vs Go, re-ran certify, overlay stays
  **killed**. L-interp **15.351 s** is still ~16 s vs Go **2.161 s**.
- Overlay emit/install remain in `abi.c` / `emit.c` / tests; runme-aot
  does **not** register overlays or wrap `load` (`overlay_wrap=0`).
- O4 `src/c/emit.c` prim inline (`+` `-` `cons` `hd` `tl`) **was** done
  earlier; intern **9150**, apply **6030**. Overlay stays unplugged.
- No CFLAGS flip in `tools/yggdrasil-build.c`.
- No intern cache rewrite.
- No `apply_on_fresh_stack` / mmap / pthread rewrite.
- No JIT of `t*`, no bytecode VM for one-shot runme, no `Value(u64)`.
- No Boehm removal, no fib unbox, no C++, no rung 2 (`docs/rung2.md` remains
  documentation only).
- Certify **was** re-run this job; 134/0 is `evidence/certify.log`
  `21:52:53Z`–`21:54:26Z`.
