# Option 5 rung 2 — last-use / regions (documentation only)

Stage: `rung2-doc`. This file is a **proof list**, not an implementation and
not evidence that rung 2 ships. Do not treat `docs/rung2.md` (short pointer
to this file) as a completed heap.

Keep Boehm on rung 1. **Do not implement rung 2 in this stage.** Do not
change `nix develop -c make CC=clang certify` (`passed ... 134` /
`failed ... 0`) as a side effect of documentation.

## Two rungs

| | Rung 1 (this tree, live) | Rung 2 (later, this file) |
|---|---|---|
| Input | Shaken KLambda (`kernel.kl` + user `.kl` + `yggdrasil.manifest.txt`) | Same shake, but only if the **live set** is eval-free |
| Output | C `NativeFunction`s in `app.c` | Lifetime-inserted **non-GC** C |
| Allocations | `shen_context` / Boehm `GC_malloc` (`src/c/context.h`, `src/c/kl.h`) | Region / last-use reclaim; **not** Boehm for KLObject graphs |
| `eval-kl` | Allowed for `needs-eval` shakes (`shen_eval_kl` in `src/c/abi.h`) | **Forbidden in the live set** |
| REPL | Interpreter `shen-c repl` (`src/c/main.c`, `src/c/repl.c`) | **No REPL** in the artifact |
| What it is not | Not Chicken-on-C-stack, not libc `malloc`/`free` of `KLObject`, not C++, not `eval_kl_object` of the source string | Same rejections; also not “drop Boehm and hope” |

Rung 1 is option 5 as it exists: generate C from shaken KLambda. Each
`defun` becomes a C function (or trampoline step) that uses intern /
cons / primitives on `shen_context`. Allocations stay on Boehm.

Rung 2 is last-use analysis plus region insertion so those same C
functions can reclaim cells without a tracing GC. It only applies when
the program cannot evaluate new KLambda at runtime and cannot enter a
REPL.

## Rung 1 facts (do not regress)

Cited paths:

- Heap root: `/Users/reuben/projects/shen-c/src/c/context.h` (`shen_gc_malloc` → `GC_malloc`). `src/c/kl.h` maps `malloc`/`realloc`/`calloc` onto that context and `free` onto `GC_free`.
- Codegen: `/Users/reuben/projects/shen-c/src/c/emit.c`, `/Users/reuben/projects/shen-c/src/c/emit.h`. `emit_defun_into` writes `static KLObject* native_* (...)`. Self-tails become `goto tail_start_*` (`is_self_tail`); other tails call `shen_tail_apply`. Atoms/calls use `shen_intern` / `shen_cons` / `shen_apply` / `shen_vector` via `/Users/reuben/projects/shen-c/src/c/abi.h`. Lambdas/freezes are `lambda_*` + `shen_native_closure` (`collect_free`). `trap-error` is `trap_body_*` / `trap_handler_*` + `shen_trap_error` (`emit_trap`). Nested 0-arity `do` chains may flatten to `init_step_*`.
- Stage-2 builder: `/Users/reuben/projects/shen-c/tools/yggdrasil-build.c` reads a shake dir and writes `app.c` + Makefile + CMakeLists.txt, then links `bin/libshenc.a`. Wired from `/Users/reuben/projects/yggdrasil/builders.json` (`"c"`) and `/Users/reuben/projects/yggdrasil/builders/c/build.sh`.
- Interpreter eval: `/Users/reuben/projects/shen-c/src/c/evaluator.c` (`eval_kl_object`, `eval_eval_kl_expression`). `/Users/reuben/projects/shen-c/src/c/abi.c` `shen_eval_kl` is a thin wrap of `eval_kl_object` for **needs-eval generated C**, not a substitute for per-defun emission.
- REPL: `/Users/reuben/projects/shen-c/src/c/main.c` default/`repl` → `run_shen_repl()`. Generated `main` in `emit.c` is `shen_boot` → install NativeFunctions → `shen_apply_port_overwrites` → kernel toplevels → optional `(shen.initialise)` → user toplevels. It never calls the REPL.
- Link: `/Users/reuben/projects/shen-c/CMakeLists.txt` — C17, Nix `pkg-config` `bdw-gc` only (Homebrew `/opt/homebrew` is a hard error), `libshenc` + `yggdrasil-build`.
- Profile of rung 1 vs tree-walker vs shen-go: `/Users/reuben/projects/shen-c/evidence/option5-profile.md`. Full-kernel tree-walker vs shen-go: `/Users/reuben/projects/shen-c/evidence/profile.md`. Darwin sample of certify is ~79% Boehm leaves on `eval_kl_object` stacks — that is the interpreter, not a license to rip GC out of rung 1.
- There is **no** last-use / region / bump-arena pass in `src/c` (`region_reset`, `bump_arena`, `shen_region` do not exist). Rung 2 is not secretly implemented.
- Eval-free generated **source** is NativeFunctions, not a source-string wrap: `bin/fib-ygg-app/app.c` contains `static KLObject* native_kl_fib_54 (...)` that intern/`shen_apply`/`shen_number_l` on `shen_context`, not `eval_kl_object` of the original `(define fib …)` text. `make test` greps `! grep -F shen_eval_kl` on fib/hello/sum and `grep -F shen_eval_kl` on `tc-ygg`.

Forbidden as “codegen” on either rung:

- Wrapping `eval_kl_object` on the original source string (or `load_kl_file` of the user program) and calling that an AOT app.
- Chicken Scheme on the C stack.
- `malloc`/`free` of today’s tagged `KLObject` cells as a GC replacement without last-use proofs.
- A C++ rewrite of the runtime.

`emit.c` still lowers `(eval-kl X)` to `shen_eval_kl(ctx, tN)` because
kernel `declare` / needs-eval slices contain it. Rung 1 tests
(`/Users/reuben/projects/shen-c/test/c/test_emit.c`, Makefile greps) require
**zero** `shen_eval_kl` on eval-stripped fib/hello/sum and **nonzero** on
`test/fixtures/tc-ygg`.

Shake host (stage 1) is Shen, not C. Reference: sibling shen-cl; fallback
shen-go (`/Users/reuben/projects/shen-go-shen42/cmd/yggdrasil-build/main.go`
is the **Go** stage-2 builder, analogous in contract). Lua analog:
`/Users/reuben/projects/shen-lua-shen42/bin/yggdrasil-build.lua`.
`shen-c eval -l yggdrasil.shen` exists (`*hush*` is stdout-only) but
`yggdrasil.shake` on shen-c is unverified — use shen-cl or shen-go until
that is proven. **Stage-2 for `--target c` must be C** (`bin/yggdrasil-build`
+ `emit.c`), not the Go or Lua builders.

First eval-free fixture: `/Users/reuben/projects/yggdrasil/tests/fib.shen`
(`(define fib …)` then `(output "fib 20 = ~A~%" (fib 20))`). Live shake
manifest `/Users/reuben/projects/shen-c/test/fixtures/fib-ygg/yggdrasil.manifest.txt`:
`needs-eval=false`, `cannot-reach=eval`, `fn=fib 1`. That is the shape
rung 2 would be allowed to **attempt**. `tests/tc-interp.shen` /
`test/fixtures/tc-ygg` (`needs-eval=true`, `reaches=eval`,
`primitive=eval-kl`) is not a rung 2 candidate.

Do not confuse Yggdrasil **capabilities** with Shen **eval entry points**:

- Manifest `cannot-reach=eval` / `reaches=eval` is the `eval-kl` gateway
  (`*capabilities*` in `/Users/reuben/projects/yggdrasil/yggdrasil.shen`;
  design note `/Users/reuben/projects/yggdrasil/docs/reachability.md`).
  `reaches=read` on fib-ygg means the `read-byte` I/O gateway is live, **not**
  that `(read …)` eval is live.
- Eval-stripping uses `*eval-entry-points*`: `eval`, `eval-kl`, `load`,
  `tc`, `spy`, `track`, `step`, `it`, `read`, `read-from-string`,
  `lineread`, `input`, `input+`, `bootstrap`. Those names in the **user
  call set** keep the typechecker / reader / `eval-kl` in the shake.

fib-ygg still `reaches=read,write,file,clock` because kernel initialise and
`output` use `read-byte` / `write-byte` / `open` / `close` / `get-time`.
Rung 2 must still model those primitives (retain vs consume); it must not
treat `reaches=read` as an automatic eval-kl fail.

## Rung 2 shape

When implemented, a rung 2 emitter would still:

1. Parse shaken KL (same `shen_read_kl_path`).
2. Emit one C function (or trampoline step) per `defun`, plus lambdas /
   freezes as `NativeFunction`s.
3. Call intern / cons / primitives — not a different Shen semantics.

It would **additionally**:

- Refuse the shake unless the live set is eval-free (below).
- Not link REPL, reader-eval loop, or `shen_eval_kl`.
- Annotate every allocation with a last-use and insert region enter /
  bump / reset (or equivalent lifetime reclaim).
- Leave the certified interpreter (`bin/shen-c` + Boehm + `eval_kl_object`)
  and all rung 1 apps on Boehm.

Rung 2 is not a second interpreter. It is a stricter backend for a
closed, eval-stripped NativeFunction program.

## What the emitter would have to prove

Nothing below is implemented. Each item is a **proof obligation** that
must be discharged (static analysis + generated-C greps + a closed
fixture run) before any non-GC heap is wired.

### 1. Live-set eligibility (gate; fail closed)

Prove the shaken program cannot introduce new KLambda at runtime:

- Manifest `needs-eval=false` and `cannot-reach=eval` (Yggdrasil eval
  capability; see `/Users/reuben/projects/yggdrasil/README.md` and
  `yggdrasil/docs/reachability.md`).
- User seeds do not intersect `*eval-entry-points*` (quoted above). I/O
  capabilities (`read`/`write`/`file`/`clock`) are orthogonal.
- The union of kernel + user forms contains **no** special-form or
  primitive in the eval family: `eval-kl`, `eval`, `load`, `tc`, Shen
  `read` / `input+` / `read-from-string` / `lineread` / `bootstrap` /
  `spy` / `track` / `step` / `it`, and any other name Yggdrasil treats as
  eval-capable.
- Generated C contains **zero** `shen_eval_kl`, `eval_kl_object`,
  `load_kl_file`, `run_shen_repl` (source grep). Rung 1 already does
  this for eval-stripped fib/hello/sum; **source grep is not a closed
  link set**.
- The artifact `main` is the emitted boot sequence only — no
  `src/c/main.c` REPL command table. Rung 1 fib-ygg `main` is
  `shen_boot` → install NativeFunctions → `shen_apply_port_overwrites`
  → `(shen.initialise)` → user toplevels; it never calls
  `run_shen_repl`.
- The **link set** does not define unused interpreter entry points.
  Today `bin/libshenc.a` is fat: `nm bin/fib-ygg-app/app` still shows
  `T _eval_kl_object`, `T _shen_eval_kl`, `T _load_kl_file`,
  `T _run_shen_repl` even when `app.c` never mentions them. That is
  still Boehm/rung 1. A rung 2 artifact must not archive
  `evaluator.c` / `repl.c` (or must prove those symbols are unreferenced
  and stripped). Zero `shen_eval_kl` in C text while the binary still
  exports `eval_kl_object` does **not** discharge this obligation.
- `declare` signature thunks that lower to `eval-kl` are either absent
  (eval-stripped shakes already drop `declare` as build-time-only) or
  the emit is rejected.

If any check fails, stay on rung 1 / Boehm. `tc-ygg` and metaeval are
permanent rung 1 (or interpreter) programs.

### 2. Closed world of functions

Prove every call target is in the emitted NativeFunction set or a
declared primitive:

- Head-position symbols that are not locally bound are either a shaken
  `defun`, a KL special form the emitter lowers (`if`/`cond`/`let`/`do`/
  `lambda`/`freeze`/`trap-error`/`and`/`or`/`type`/`defun`), or a
  primitive registered in `src/c/primitive.c` / port overwrites
  (`src/c/overwrite.c` via `shen_apply_port_overwrites`).
- Higher-order values (`(F X)` where `F` is a parameter or `value` of a
  global) only receive functions from that closed set. Unknown runtime
  functions are an escape (see §5) and block region reset of their
  arguments.
- No path installs a new `defun` body as a `UserFunction` KL tree for
  `eval_kl_object` to interpret (`src/c/kl.h` `KL_FUNCTION_TYPE_USER_FUNCTION`
  / `KL_FUNCTION_TYPE_CLOSURE` with KL `body` in `src/c/function.h`).
  Rung 2 closures must be native (`shen_native_closure`). Interpreter
  `Closure` objects that carry a KL body are out of the live set.
- Nested `defun` (emit currently registers it and returns the interned
  name) still counts as a NativeFunction in the closed set, not as a
  runtime `eval-kl`.

### 3. Last-use of every allocation

Rung 1 already names SSA-like temps (`tN` in `emit.c`). Last-use is the
latest program point that may still observe the object. The emitter must
prove, for each `shen_cons` / `shen_vector` / `shen_number_*` /
`shen_string` / `shen_native_closure` / exception / argument vector:

- A last-use program point in the CFG of the generated function,
  including:
  - `if`/`cond` join
  - `do` sequencing (`emit_do`; 0-arity flattened `init_step_*`)
  - `let` binding extent (`push_bind` / restore `nbind`)
  - `trap-error` body vs handler (`trap_body_*`, `trap_handler_*`,
    `sigsetjmp` / `longjmp` in `shen_trap_error`)
  - `goto tail_start_*` (self-tail)
  - `shen_tail_apply` bounce (`abi.c`: `bounce_pending` /
    `bounce_fn` / `bounce_args`; `shen_apply` saves/restores bounce
    across nested apply)
  - lambda/freeze helpers (`lambda_*`; `allow_self_goto` is forced off)
- Alias accounting: `hd`/`tl` do not extend the cons’s life past the
  last use of the pair **and** of any alias stored in a vector, global
  (`set`/`value`), symbol function cell, or captured lambda/freeze.
- `intern` / interned strings / empty list / booleans are **immortal**
  (symbol table in `kl.h`). Last-use does not free them.
- Numbers and fresh strings are ordinary cells unless interned.
- Recursive `fib` (non-tail): `(+ (fib (- N 1)) (fib (- N 2)))` — the
  first recursive result and the boxed intermediates for `-` must stay
  live until `+` returns. A region reset after the first call is a
  use-after-free.
- Self-tail `goto`: live parameters after the assignment to `t*` temps
  (`emit.c` `is_self_tail` copies through `n<fresh>_<i>` then writes
  `self_params`) must survive `goto tail_start_*`. Dead iteration
  garbage may be reset **after** copying the new arguments and
  **before** the next body, never while those arguments still alias
  the old region.
- `shen_tail_apply` / bounce: arguments and the function object must
  outlive the bounce into `shen_apply`. Nested apply already saves
  bounce state; rung 2 regions must nest the same way or refuse
  tail-bounce reclaim. A bounced call that returns `NULL` from
  `shen_tail_apply` must not be treated as a last-use of the result
  temp (`tN = NULL` after self-tail goto is a placeholder, not a live
  value).

### 4. Region insertion (soundness of reclaim)

Given last-use, prove a region (bump arena, frame pool, or equivalent)
such that:

- Enter at a domination point (function entry, `let`, apply frame, loop
  header / `tail_start_*`).
- Reset or pop only when every allocation in that region has passed its
  last-use and has not escaped (§5).
- Nested regions for nested `let`/`do`/`apply` / trap body /
  `init_step_*`.
- Closures: captured `KLObject*` are either copied into a region that
  outlives the closure, or the closure’s region is the parent’s and is
  not reset while the closure is reachable. `collect_free` is the
  capture list to start from; it is not itself a last-use proof.
- `trap-error` (`emit_trap`, `shen_trap_error`): the handler may run
  after the body aborted. Body-local regions must reset on both normal
  return and `longjmp`; the exception object is live in the handler;
  the captured env vector (`tenv_*`) is live for both body and handler.
  Bounce flags restored on both paths in `abi.c` must stay consistent
  with region nesting.
- No conservative stack scan. Rung 2 is the claim that roots are
  explicit. If a proof cannot name the roots, keep Boehm (rung 1).

### 5. Escape / sharing

Treat as escaped (cannot reset the allocating region) any object that:

- Is stored in a global (`set`), symbol `variable_value` / `function`,
  vector/`absvector` slot (`address->`), dictionary, or stream.
- Is captured by `lambda`/`freeze` (`collect_free` already lists
  captures; those temps escape into `shen_native_closure`).
- Is returned from the NativeFunction (caller’s region, not callee’s).
- Is passed to an unknown or primitive function that may retain it
  (`cons`, `set`, `vector`, output that boxes, `error-to-string`, …).
  Primitives need a retain/no-retain table keyed to
  `src/c/primitive.c` (and overwrites). Over-approximation
  (assume retain) is the only safe default.
- Shares identity with another live name (`eq` / `=` of cons cells).
  Freeing one name frees the other.

Over-approximation is required: if last-use or escape is unknown, the
cell stays in a long-lived region or the emit is rejected back to
Boehm.

### 6. KL semantic preservation

Prove observational equivalence with rung 1 on the same shake:

- Arithmetic, `if`/`cond`/`let`/`do`, `trap-error`, `freeze`,
  partial apply / arity, self-tail vs other-tail.
- Pointer identity where KL exposes it (`eq` on cons, interned symbols).
- `vector` / `address->` mutation: last-use of a vector is after the
  last read of any slot that still aliases it.
- Port overwrites still apply after install (`shen_apply_port_overwrites`
  in generated `main`) so `pr` / streams match the interpreter.
- Stdout of `/Users/reuben/projects/yggdrasil/tests/fib.shen` remains
  `fib 20 = 6765`. Hello remains `hello from shaken shen`.
- Special emit identities (`shen.demodulate` closed form, `shen.lambda-entry`
  NULL/non-symbol short-circuit) remain observationally equal.

### 7. Negative proofs (what does **not** count)

The emitter has **not** proved rung 2 if it only:

- Links Boehm and times fib (that is rung 1; see `evidence/option5-profile.md`).
- Calls `malloc`/`free` on `KLObject` without last-use (use-after-free
  under sharing / closures / bounce).
- Wraps `eval_kl_object` on the source string.
- Emits Chicken or C++.
- Drops `eval-kl` from the C text while the shake still has
  `needs-eval=true` (hidden interpreter still required).
- Drops `eval-kl` from the C text while `nm` of the artifact still
  defines `eval_kl_object` / `run_shen_repl` (fat `libshenc.a`).
- Ships a REPL in the artifact.
- Relies on Boehm conservative stack scanning while claiming regions.

### 8. Interpreter / certify isolation

Prove rung 2 cannot leak into the certified tree-walker:

- `bin/shen-c` still boots `src/c/repl.c` + `eval_kl_object` + Boehm.
- `nix develop -c make CC=clang certify` still records 134/0 in
  `evidence/certify.log` without requiring a rung 2 heap.
- `CMakeLists.txt` / Makefile still require Nix `bdw-gc` for
  `libshenc` and rung 1 apps.

Rung 2, if it ever exists, is a separate emit mode and a separate
artifact link set (no `evaluator.c` eval-kl, no REPL object). It does
not replace `shen_context` as the rung 1 GC root.

## Implementation status

Not started. No last-use pass, no region allocator, no eval-kl strip in
`emit.c` beyond “don’t emit it if the KL does not contain it”. Keep
Boehm on rung 1 (`shen_gc_malloc` → `GC_malloc`; generated apps still
`U _GC_malloc`). This document is the stage `rung2-doc` deliverable.
Do not treat a passing `make test` / `make certify` as rung 2.

Related stub: `/Users/reuben/projects/shen-c/docs/rung2.md`.
