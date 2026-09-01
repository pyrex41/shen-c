# AOT runme gap vs shen-go — sample, not emit churn

Stage: `tagged-immediates` + `kernel-aot-install_all` + `o7-apply-direct-lex-env`
+ `o6-rebar` + `aot-overlay-killed` + `o4-prim-inline-shipped` +
`tc-cache-shipped`.
This file is the proof that matching Go’s **~5 s** suite wall is **not** a
cheap CFLAGS / intern-leaf / mmap-per-tail patch, **and not** a rust-style
load-then-swap overlay of sidecar defuns. Final rebar shipped tagged
immediates (fixnum / empty-list / bool; heap cons/symbol/string tags 2/4/6
with `GC_register_displacement`) plus always-AOT kernel `install_all` on
`bin/shen-c` (686 defuns after tree-walker boot; C overwrites last). That is
**not** overlay-after-load: `interpreter.shen` / `prologinterp.shen` stay
runtime load sidecars. Evaluator small-arity apply uses a stack `Vector`.
Both bars **134/0**. Cold AOT `runme` wall **5.72 s** / L-interp **4.244 s**
(`1177672` inf) vs this-job Go **4.84 s** / **2.141 s** (`1178236` inf), and
vs O7 baseline **8.33 s** / **6.555 s**. That is **low-teens-or-better, not
5 s**: still **~1.18×** this-job Go wall and **~2.0×** L-interp. Do not
claim 5 s; the quoted wall is **5.72**. Overlay stays **unplugged**
(`overlay_wrap=0`). O5 tc-cache remains env-gated; warm L-interp can
match Go, cold one-shot does not. Do not unbox fib. Do not C++. Do not
implement rung 2. Do not JIT `t*`. Do not bytecode-VM one-shot runme.
Keep Boehm. Keep both 20→**134/0** bars.

`cheap[]` is empty. The live Darwin sample of `bin/runme-aot-app` during
L interpreter / Prolog interpreter **unblesses** those three flag/intern/mmap
ideas. O1+O2 overlay (bootstrap KL of `interpreter.shen` / `prologinterp.shen`,
load sidecar for datatype/`define` effects, then swap defun cells on
source-hash + kernel-digest + arity match) was wired into runme-aot and
**killed**: L-interp typecheck stayed **16.54–17.16 s** with overlay vs
baseline **16.182 s** (`1177672` inf). After unplug (`evidence/runme-aot.log`
`21:10:54Z`, still **134/0**): **16.845703 s** / **16.845723 s** after
30→inferences. `load` prints `run time` around `shen.load-help` *before* any
post-load swap; that clock is typecheck of the sidecar, not the later
`normal-form` calls (~0.0005 s). Overlay emit/install remain in `abi.c` /
`emit.c` / tests; runme-aot no longer registers or wraps `load`.
Do not re-litigate overlay-after-load for this wall.

## Bars (do not regress)

| bar | evidence | status |
|-----|----------|--------|
| AOT runme **passed 134 / failed 0** | `evidence/runme-aot.log` (`app_pipe_exit=0`, SHA `a6b9e240418fdce4c5ce65d70379cbe72ecb94a1`, app sha256 `ba7db6737b109668493eb707162604063200051dbdfb0b339693f2826a75eb6f`, `15:16:29Z`–`15:16:35Z`, **5.72** real) | keep |
| Interpreter certify **passed 134 / failed 0** | `evidence/certify.log` `15:20:14Z`–`15:20:20Z`, Shen **6.182 s**, `exit=0` | keep; **re-ran this job** (kernel AOT + tagged heap) |
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
cold L-interp **15.35 s → 6.55 s**. This rebar’s tagged immediates +
stack-apply moved cold L-interp **6.555 s → 4.244 s** and wall **8.33 s →
5.72 s**, not to 5 s. `interpreter.shen` / `prologinterp.shen` remain
**runtime load sidecars** whose typecheck walks boxed KL (`eval_kl_object`
+ `lookup_environment`). It is **not** `-O2` vs `-O3`, **not** mmap-per-tail,
**not** overlay-after-load, **not** fib unbox, **not** C++, **not** rung 2,
**not** a bytecode VM, **not** JIT `t*`.

## Timed windows (quoted)

Final rebar tagged immediates + stack apply (`evidence/runme-aot.log`,
`start_utc: 2026-09-01T15:16:29Z`, `cwd` `shen/test/s42`, `defuns=597`,
`eval_kl_object=0` in generated `app.c`, `overlay_wrap=0`, intern **2426**,
`apply_direct` **4259**, `shen_apply(` **96**, git SHA
`a6b9e240418fdce4c5ce65d70379cbe72ecb94a1`):

```
L interpreter: (load "interpreter.shen") = loaded
run time: 4.244059 secs
typechecked in 1177672 inferences

Prolog interpreter: (load "prologinterp.shen") = loaded
run time: 0.791181999999999 secs
typechecked in 1634304 inferences

passed ... 134
failed ... 0

run time: 8.150703 secs
        5.72 real         7.79 user         0.38 sys
app_pipe_exit=0
```

Go same suite (`evidence/compare-runme-shen-go.log`,
`end_utc: 2026-09-01T15:19:44Z`):

```
L interpreter: (load "interpreter.shen") = loaded
run time: 2.141432458 secs
typechecked in 1178236 inferences

Prolog interpreter: (load "prologinterp.shen") = loaded
run time: 0.4519664169999995 secs
typechecked in 1634868 inferences

passed ... 134
failed ... 0

run time: 4.6181311670000005 secs
        4.84 real         7.34 user         0.16 sys
```

Interpreter certify re-run (`evidence/certify.log`, `bin/shen-c` with
kernel `install_all`, `start_utc: 2026-09-01T15:20:14Z`):

```
passed ... 134
failed ... 0

run time: 6.174035 secs
loaded

run time: 6.182002 secs
exit=0
end_utc: 2026-09-01T15:20:20Z
```

Certify L interpreter typecheck **3.182 s** / **1177672** inf; prologinterp
**0.651 s** / **1634304** inf (O7 certify L-interp **20.870 s**, Shen
**38.92 s**). Wall variance is real, bar is **134/0**. Kernel AOT does not
AOT the sidecars.

O7 apply_direct + lex env (`00:28:53Z`): **8.33** real, L-interp **6.555 s**.
O6 cold AOT (`21:52:10Z`): **15.31** real, L-interp **15.351 s**.
This rebar is **5.72** real / **4.244 s** L-interp vs this-job Go **4.84** /
**2.141 s**, and vs the O7-quoted Go **4.94** / **2.211 s**: about **1.18×**
wall, **2.0×** L-interp, **not 5 s**. User>real on AOT is still trampoline
hops (pthread), not proof mmap is the hotspot. Overlay remains killed.

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
certify 134/0. Boehm stays (`otool libgc.1.dylib`). No fib unbox, no C++,
no rung 2. Fib **0.01 s** is shaken-boot (55 defuns), not the gap.

Emit intern/apply (`src/c/emit.c`): O7 named calls emit
`shen_apply_direct` / `shen_tail_apply_direct` (intern-static + stack
args). Unbound *value* atoms still intern. Self-tails `goto`. O6 sample
binary (`intern=14852` `apply=11515`) is **not** this binary; live O7/rebar
`app.c` is intern **2426** `apply_direct` **4259**. `yggdrasil-build.c` Makefile `CFLAGS ?= -O2`
(top-level Makefile `CFLAGS ?= -O3` for `libshenc`). `abi.c`
`apply_on_fresh_stack`: `mmap(16MiB+guard)+pthread_attr_setstack+GC_pthread_create/join`
per hop when generated `NativeFunction` and stack low — not on-CPU in this
sample.

Wall times (`yes | /usr/bin/time -l`; this rebar `app_pipe_exit=0`;
earlier logs may show pipe exit **141** from `yes` SIGPIPE after 134/0 ok):

- AOT final rebar (this job, `15:16:29Z`): **5.72** real / **7.79**
  user / **0.38** sys, Shen **8.151 s**, RSS **51.9 MiB** (`54394880`),
  passed **134** / failed **0**. L interpreter typecheck **4.244 s** /
  **1177672** inf. Prologinterp **0.791 s** / **1634304** inf.
  intern **2426**, apply_direct **4259**. `overlay_wrap=0`. Boehm
  `libgc.1.dylib`. **Not 5 s** (Go this job **4.84** real / L-interp
  **2.141 s**; O7-quoted Go **4.94** / **2.211 s**).
- AOT clean O7 (`00:28:53Z`): **8.33** real / **11.24** user / **0.61**
  sys, Shen **11.822 s**, RSS **59.8 MiB** (`62734336`), passed **134** /
  failed **0**. L interpreter typecheck **6.555 s** / **1177672** inf.
  Prologinterp **1.290 s** / **1634304** inf.
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
- Go this job `.bin/shen-go script runme.shen` (`15:19:44Z`): **4.84**
  real / **7.34** user / **0.16** sys, Shen **4.618 s**, RSS **217.0 MiB**
  (`227557376`), L interpreter typecheck **2.141 s** / **1178236** inf,
  prologinterp **0.452 s** / **1634868** inf, **134/0**.
- Go clean O6 rebar (`21:52:35Z`): **4.89** real / **7.36** user / **0.15**
  sys, Shen **4.670 s**, L-interp **2.161 s** / **1178236** inf.
- Prior AOT (`19:32Z`): **15.13** real / **27.92** user, Shen **29.046 s**,
  L-interp **16.182 s**. Prior Go (`19:33Z`): **4.57** / **7.20**, Shen
  **4.391 s**, L-interp **2.041 s**. `option5-profile` (`18:58Z`): AOT
  **15.36** / **25.19** Shen **26.14 s**; Go **5.37** / **7.80** Shen
  **5.05 s**. Variance high; AOT is **~1.18×** this-job Go wall, not a match.
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
**4.244 s** vs Go native **2.141 s** on the same **~1.18 M** inferences. Do
not chase trampoline, fib unbox, C++, or rung 2.

## Why `cheap[]` is empty (code the sample did not bless)

### 1. Generated `CFLAGS ?= -O2` vs `libshenc` `-O3`

`tools/yggdrasil-build.c` writes `CFLAGS ?= -O2` into
`bin/runme-aot-app/Makefile`. Top-level Makefile uses `CFLAGS ?= -O3` for
`bin/libshenc.a` / `bin/shen-c`. The sample has **zero** symbols that are
an optimization-level leaf. Flipping the generated flag is a rebuild, not
a named hotspot.

### 2. Intern every prim

Unbound atoms intern (`src/c/emit.c` `emit_atom`):

```c
cbuf_printf(e->stmt, "  KLObject* t%d = shen_intern(ctx, ", t);
```

O4 already inlines `+` / `cons` / `hd` / `tl` / `=` to `shen_add` /
`shen_cons` / … in generated `t*`. Remaining intern is unbound *value*
atoms, not those prim call sites. Exclusive `intern_kl_string`+`shen_intern`
on the O6 sample ≈ **7.0%**. Caching intern harder does not close the
sidecar gap.

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

1. Faster sidecar typecheck (still `eval_kl_object` of `interpreter.shen`).
   Generated `t*` already uses Prim*-style `shen_add` / `shen_cons`. Go
   typechecks loaded files as native. O5 tc-cache is env-gated
   (`SHEN_C_TC_CACHE`); warm L-interp **2.040 s** / **14/14** replay
   (`evidence/runme-aot-tc-warm.log`). Do not cache `shen->kl`.
2. Do **not** overlay-after-load the sidecar’s own defuns. Measured and
   **killed** (O2 gate): wall **16.54–17.16 s**, inferences unchanged.
3. Do **not** chase another trampoline. Do not JIT `t*`. Do not bytecode-VM
   one-shot runme. Do not unbox fib. Do not C++.

Kernel `t*` / `typecheck` being NativeFunctions is already true
(`shen_register_defun(ctx, "shen.t*", 6, &native_kl_shen_2et_2a_507)`).
`bin/shen-c` additionally overwrites boot UserFunctions via
`shen_kernel_aot_install_all` (686 defuns). The remaining gap is the
**sidecar** walking boxed KL through those natives.

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

- Tagged immediates: fixnum tag **1**, empty-list **3**, false/true **5/7**;
  heap cons/symbol/string tags **2/4/6**. Overflow longs and doubles stay
  atomic Boehm cells. `GC_register_displacement(2/4/6)` before `GC_init`.
  Cons stays **one** Boehm `KLObject` with an inline pair.
- Always-AOT kernel `install_all` (`tools/kernel-aot-build.c`, 686 defuns)
  linked only into `bin/shen-c`. Weak no-op in `libshenc`. After boot,
  overwrite kernel UserFunctions; C map/pr/… and tc-cache wrap last.
  `SHEN_C_NO_AOT` skips the call. Not overlay-after-load. Sidecar tests
  stay on the tree-walker. Generated kernel C has no `int main` and no
  `interpreter.shen`.
- Tree-walker small-arity primitive apply uses a stack `Vector` (no
  CONS-then-`create_vector` on the typecheck hot path). Pointer identity
  is the first check in `is_kl_object_equal`.
- Overlay stays **killed**. No JIT `t*`, no bytecode VM for one-shot runme,
  no fib unbox, no C++, no rung 2, no trampoline/mmap rewrite, no Homebrew.
- AOT runme **134/0** (`evidence/runme-aot.log`): L-interp **4.244 s**,
  wall **5.72 s**. Go this job **2.141 s** / **4.84 s**. O7 baseline
  **6.555 s** / **8.33 s**. Certify **134/0**, Shen **6.182 s**
  (`evidence/certify.log`). Honest: **not 5 s**.

## What this job did not do

- Overlay emit/install remain in `abi.c` / `emit.c` / tests; runme-aot
  does **not** register overlays or wrap `load` (`overlay_wrap=0`).
- No CFLAGS flip in `tools/yggdrasil-build.c`.
- No `apply_on_fresh_stack` / mmap / pthread rewrite.
- No JIT of `t*`, no bytecode VM for one-shot runme.
- No Boehm removal, no fib unbox, no C++, no rung 2 (`docs/rung2.md` remains
  documentation only).
- Did not AOT `interpreter.shen` / `prologinterp.shen`.
