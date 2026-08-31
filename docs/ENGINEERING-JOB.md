# ENGINEERING-JOB — first-class AOT lambdas through C map/apply

## Problem

The real engineering job is **AOT-C completeness of first-class functions**, not more emission, not a new GC, and not a stack rewrite.

Rung 1 fib/hello AOT already works. `emit.c` already lowers `lambda`/`freeze` to `shen_native_closure(ctx, arity, &lambda_N, caps)` — boxed `KL_TYPE_FUNCTION` PrimitiveFunctions. Generated `native_kl_macroexpand_290` does that for `lambda_51` (`(lambda Z (tl Z))` over `*macros*`).

AOT `bin/tc-ygg-app` still traps on `(load "interpreter.shen")`:

```
apply expects a function or interned symbolinferences = 2778
```

`2778` is kernel declare-table inferences after `(shen.initialise)` / `(tc +)`. There is no `normal-form = 7`. The typecheck of `interpreter.shen` never runs.

The trap object is a **raw C `NativeFunction*`** (e.g. `lambda_51`) stored in a Shen list and later `shen_apply`'d. Cause: leftover eval-kl / C-overwrite apply still treats **any** `is_kl_function` car as a tree-walker `Closure`. `Function` is a tagged union (`PrimitiveFunction*` / `UserFunction*` / `Closure*`). `eval_kl_list` does:

```c
if (is_kl_function(evaluated_car_object))
  return eval_kl_list_closure_function_application(...);
```

`get_closure_body` on a PrimitiveFunction reads `native_function` as `KLObject*` — the raw code pointer. C `map` (and `shen.compose`, `dict-fold`, bind/lzy 0-arg `CONS(fn)` + `eval_kl_object`) synthesize a KL list and re-enter that path. After `shen_apply_port_overwrites`, `map` is the live `macroexpand` / `walk` / `*macros*` path (`macros.kl`: `(map (lambda Z (tl Z)) (value *macros*))`).

Tree-walker `bin/shen-c script tc-interp.shen`: ~7s, `inferences=224811`, `normal-form=7`. shen-go: ~0.75s, `231769`, `normal-form=7`. Full `runme` C ~78s vs Go ~5s because Go’s typechecker is native Go — **not this job**.

`interpreter.shen` is a **runtime load sidecar**, not an AOT compile of that file. `needs-eval=true` keeps types / t-star / eval-kl / load in the 568-defun shake. `emit.c` lowers `(eval-kl X)` to `shen_eval_kl` only (declare thunks). `make certify` is 134/0 and must stay that way.

## Recommended path

Unify leftover apply with `shen_apply`. Do not emit more eval-kl. Do not unwrap lambdas.

1. Keep emit wrapping. Never assign `&lambda_N` into cons/vector slots. Never teach `shen_apply` to accept raw `NativeFunction*`.
2. Dispatch `eval_kl_list` (and trap-error handlers) by `KLFunctionType`: primitive → `eval_kl_list_primitive_function_application` or `shen_apply`; user → user path; closure → existing closure path only.
3. Point C HOFs that `CONS` a function object then `eval_kl_object` it at `shen_apply(fn, Vector of already-eval’d args)`.
4. `eval_simple_closure_function_application` must call arity-0 primitives (native freeze) instead of union-punning `Closure.body` as `NativeFunction*`.
5. ABI test that fails today: wrap, cons, map/tl/apply, assert `is_kl_function` / `is_primitive_kl_function`.
6. Rebuild `bin/tc-ygg-app` only. Success: `inferences=224811` and `normal-form=7`. Then `make certify` still 134/0.

**Kill criterion:** if a milestone does not move tc-interp inferences off `2778`, stop and reassess. Do not “fix” frames, rung 2, or Go wall-time until wrapping is complete.

## Constraints (must not regress)

| Frozen | Meaning |
|--------|--------|
| Certify | `nix develop -c make CC=clang certify` → `passed ... 134` / `failed ... 0`. Interpreter tree-walker is the certified artifact. |
| Sidecar | Load `interpreter.shen` at runtime from `test/fixtures/tc-ygg`. Do not add it to the AOT defun set. |
| Shake | Existing needs-eval fixture: 568 NativeFunctions, 10 `shen_eval_kl`, types/t-star in `kernel.kl`. Do not re-shake unless wrapping is proven on this fixture. |
| Emit | `emit_lambda_or_freeze` stays `shen_native_closure`. `emit_eval_kl` stays for declare/update-lambdatable thunks. `fn` stays the kernel defun. `emit_and_or` RHS stays non-tail (bounce NULL). |
| Heap | `context.h` is `{gc_ready}` + Boehm. Keep Boehm on interpreter and rung-1 AOT. |
| Tails | Self-tail `goto`; other tails `shen_tail_apply` bounce into `shen_apply`. |
| Rung 2 | `docs/rung2.md` / `docs/option5-rung2.md` only. |

## DAG (ordered milestones)

### M0 — freeze (no code that violates this)

Do not AOT `interpreter.shen`. Do not change certify 134/0. Keep Boehm. Rung 2 docs-only. `context.h` stays GC root only. No C++ rewrite, Chicken-on-C-stack, or dropping GC. Do not treat emit wrap as missing. Do not wrap the program as `eval_kl_object` of a source string.

### M1 — ABI regression that fails on today’s runtime

File: `test/c/test_abi.c`.

- `shen_native_closure`-wrap a `NativeFunction` (macroexpand-shaped: `(lambda Z (tl Z))` and/or a `*macros*` assoc `(name . fn)`).
- `CONS` it into a list; run C `map` (the overwrite that `CONS`es F + `eval_kl_object`) and/or `tl` then `shen_apply`.
- Assert the mapped cell `is_kl_function` and `is_primitive_kl_function`; `shen_apply` of it succeeds.
- Fail if the cell is a raw `NativeFunction*`.

Repro in generated C: `bin/tc-ygg-app/app.c` `lambda_51` + `native_kl_macroexpand_290`.

### M2 — `eval_kl_list` dispatch by function type

File: `src/c/evaluator.c`.

Today any `is_kl_function(car)` goes to `eval_kl_list_closure_function_application` (`evaluator.c` ~1577). That is the PrimitiveFunction/Closure union pun.

- Primitive → `eval_kl_list_primitive_function_application` (or `shen_apply` after evaluating args).
- User → `eval_kl_list_user_function_application`.
- Closure → existing closure path **only**.
- `eval_trap_error_expression` handlers: same dispatch; do not always `eval_closure_function_application`.

Never `get_kl_function_closure` / `get_closure_body` on an AOT `lambda_N` PrimitiveFunction.

### M3 — C HOF overwrites call `shen_apply`

File: `src/c/overwrite.c`. `overwrite.c` currently has **zero** `shen_apply` calls.

Rewrite at least:

- `primitive_function_map` — live `*macros*` / `macroexpand` path after `shen_apply_port_overwrites`.
- `primitive_function_shen_compose`.
- `primitive_function_dict_fold`.
- bind / lzy 0-arity `CONS(fn, EL)` then `eval_kl_object`.
- `eval_simple_closure_function_application` (value/or, absvector/or, dict/or, and freeze-style 0-arg): if primitive arity 0, call the NativeFunction / `shen_apply`; if closure, keep eval of body.

Contract: `shen_apply(fn, Vector of already-eval’d args)`. Do not synthesize a KL list whose car is a native closure and send it back through `eval_kl_object`. Do not accept unwrapped `NativeFunction*`. Keep `native_partial` inside `shen_apply`.

### M4 — emit stays wrapping; needs-eval stays `shen_eval_kl`

File: `src/c/emit.c` (no wrap rewrite).

- Keep `tN = shen_native_closure(ctx, arity, &lambda_N, caps_N)`.
- Keep `(eval-kl X)` → `shen_eval_kl` (tc-ygg already has 10).
- Leave `fn` as kernel `defun` (`kernel.kl`); dynamic `fn` still uses `eval-kl` of `shen.lambda-function`.
- After M2+M3, confirm `shen.build-lambda-table` / `shen.update-lambdatable` store wrapped functions.

### M5 — rebuild AOT tc-ygg only; primary success numbers

```
nix develop -c make CC=clang bin/libshenc.a bin/tc-ygg-app
cd test/fixtures/tc-ygg && ../../bin/tc-ygg-app/app
```

Required stdout:

- `normal-form = 7`
- `inferences = 224811`

Not: `apply expects a function or interned symbol` + `inferences = 2778`.

Same needs-eval shake (`test/fixtures/tc-ygg`). Load sidecar only. Fib/hello AOT must still pass.

**KILL:** if inferences stay `2778`, stop. Do not proceed to timing, frames, or a larger rewrite.

### M6 — certify invariant

```
nix develop -c make CC=clang certify
```

`evidence/certify.log` must still contain `passed ... 134` and `failed ... 0`.

### M7 — times only after correctness (not first PR)

When M5+M6 hold, time AOT tc-ygg vs tree-walker ~7s vs Go ~0.75s; write `evidence/option5-profile.md`. Target is boxed KLObject intern/`shen_apply` on Boehm, not fib-unbox. Matching Go on **full `runme`** is a later milestone, not this slice. Fib 20 0.01s vs Go 0.13s is shaken-boot noise.

### M8 — Option 4 frames deferred

`shen_context` stays `{gc_ready}` plus Boehm. No frame stacks, shadow stacks, or trampoline PC/locals on the GC root. Do not trampoline certified `eval_kl_object` (`LoopFrame` stays `c-loop`/`c-recur` only). If wrapping-complete AOT `t*` then overflows the C stack, **measure depth on that artifact** before designing heap frames. Do not sell frames as a speed play versus Go.

## Success tests

| Gate | Pass |
|------|------|
| AOT tc-interp | `bin/tc-ygg-app/app` cwd `test/fixtures/tc-ygg` prints `inferences = 224811` and `normal-form = 7` |
| Certify | `passed ... 134` / `failed ... 0` |
| ABI | native closure through cons/map/apply remains `is_primitive_kl_function`; never a raw code pointer |
| Rung 1 smoke | fib-ygg `fib 20 = 6765`; hello-ygg still prints |
| Times | Secondary; only after the two primary numbers |
| Full runme vs Go | **Not** a success criterion for this job |

## First slice (one PR)

Small enough for one PR; this is the whole “now” correctness path:

1. `test_abi` case (fails first) — wrap NativeFunction, cons, map/apply, assert boxed primitive.
2. `eval_kl_list` + trap-error handler dispatch by `KLFunctionType`.
3. C `map` and CONS+`eval_kl_object` siblings in `overwrite.c`, plus `eval_simple_closure` 0-arity primitives, call `shen_apply`.
4. Rebuild `test_abi` + `bin/tc-ygg-app` only.
5. Require `224811` + `normal-form=7`, then certify 134/0.

Out of this PR: Go timing, Option 4, rung 2, AOT of `interpreter.shen`, unboxing, matching Go `runme`.

If after `eval_kl_list` + `map` inferences are still `2778`, **stop** (kill criterion). Expand only to the remaining overwrite call sites if that is what the trap still names.

## Rejected alternatives

- AOT-compile `interpreter.shen` or drop it as the load sidecar.
- Change certify 134/0 or replace the certified tree-walker.
- Treat emit wrap as missing; re-emit lambda/freeze as eval-kl of source lists.
- Teach `shen_apply` to accept unwrapped `NativeFunction*` / raw `&lambda_N`.
- Rewrite AOT lambdas as tree-walker Closure bodies to “fix” apply.
- Wrap the program as `eval_kl_object` of a source string.
- C++ rewrite; Chicken-on-C-stack; drop Boehm; libc `malloc`/`free` of `KLObject`.
- Implement rung 2 non-GC / regions.
- Option 4 heap frames as the tc-ygg correctness or speed fix.
- Match Go on full `runme` / port `types.go` in the first PR.
- Treat fib 20 0.01s as a compute win or unbox KL arithmetic to chase it.
- Treat missing quote/`fn` emit special-forms as the trap.
- Remove `shen_eval_kl` from needs-eval declare / update-lambdatable thunks.
- Make the interpreter a trampoline VM as the AOT fix.

## Evidence (binding)

- Trap + numbers: `evidence/tc-ygg.log`, `evidence/option5-profile.md` (AOT 2778 + apply trap; tree-walker 224811 / 7 ~7s; Go 231769 / 7 ~0.75s).
- Generated: `bin/tc-ygg-app/app.c` (`lambda_51`, `native_kl_macroexpand_290`, `t13915 = shen_native_closure(..., &lambda_51, ...)`, 10 `shen_eval_kl`).
- Emit wrap: `src/c/emit.c` `emit_lambda_or_freeze`.
- Apply ABI: `src/c/abi.c` `shen_native_closure` / `shen_apply` / `apply_exact` (already dispatches primitive vs user vs closure).
- Pun: `src/c/evaluator.c` `eval_kl_list` ~1577; `eval_simple_closure_function_application` ~1367.
- C map: `src/c/overwrite.c` `primitive_function_map` ~314 (`CONS` + `eval_kl_object`); also compose ~1678, dict-fold ~781, bind ~1311.
- Types: `src/c/kl.h` `Function` union; `src/c/function.h` `is_primitive_kl_function` / `is_closure_kl_function`.
- Kernel path: `shen/src/kl/macros.kl` `macroexpand` / `walk` / `*macros*`.
- Fixture: `test/fixtures/tc-ygg/tc-interp.shen`, `kernel.kl` (`fn`, `update-lambdatable` eval-kl).
- GC root: `src/c/context.h` — not frames.
- Rung 2: `docs/rung2.md`, `docs/option5-rung2.md`.

## Later (not this job)

- Time boxed AOT tc-interp vs Go ~0.75s (M7).
- Match Go on full `runme` only if kernel typecheck is compiled-C-complete **and** that is a separately authorized milestone.
- Option 4 heap frames only after a wrapping-complete AOT typecheck overflows the C stack, or after rung-2 explicit roots.
- Rung 2 non-GC C: still documentation.
