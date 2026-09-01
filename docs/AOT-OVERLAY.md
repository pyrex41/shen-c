# AOT overlay plan — close the runme gap the shen-rust way

Authority: `docs/AOT-GAP.md` (cheap[] empty), `docs/ENGINEERING-JOB.md`
(wrap done), shen-rust `README.md` / `PERFORMANCE.md` /
`design/aot-loaded-productionization-handoff.md` /
`crates/klcompile` / `src/aot/overlay.rs`.

This is the **next engineering job**. It is not intern-cache, `-O2` vs `-O3`,
mmap trampoline, fib unbox, JIT of `t*`, bytecode VM for one-shot runme,
rung 2, or a `Value(u64)` rewrite.

## Problem

AOT `bin/runme-aot-app` is **134/0** in ~**15 s**. shen-go is ~**5 s**.
The sample during L interpreter / prologinterp is sidecar **load**:

`shen_apply → native_kl_load → map → shen_eval_kl → eval_kl_object`

`interpreter.shen` and `prologinterp.shen` still typecheck as boxed KL.
L interpreter: C **16.2 s / 1.18M inf** vs Go **2.04 s**. Prologinterp:
**3.16 s** vs **0.43 s**. Kernel `t*` is already NativeFunctions; the
loaded tests are not.

## What shen-rust already proved

| Idea | Rust result | Steal for C? |
|------|-------------|--------------|
| **Overlay** | Load `.shen` for side effects, then swap defun cells for klcompile natives. Source hash + kernel digest; mismatch → silent fallback. `interpreter.shen` overlay **~2× VM / ~5× tree-walk** on `normal-form`. | **Yes. First. Kill-gated.** |
| Load-then-swap | `bootstrap` drops datatype effects; `.kl` is defuns only. | **Yes.** Do not replace `(load)` with shake-only of the sidecar (C `interp-aot-app` with `inferences=0` is the wrong shape). |
| `apply_direct` | AOT-to-AOT hits a raw fn-pointer table (`register_native` then `register_aot_direct`). Intern cache on call targets ~5.5%. Stale direct slot after `defun` was a live bug. | **Yes, after overlay.** |
| Inline ~18 prims in klcompile | `+`/`cons`/`hd`/`tl`/`if`… become `rt::` helpers. | **Yes, after overlay.** Same `emit.c`, not a new compiler. |
| **tc-cache** | Verdict memo. Cold kernel-tests **4.07→1.48 s**. | **Maybe**, parallel or after overlay. One-shot `runme` is exactly rust’s kernel-tests metric. |
| Bytecode VM | ~2.3× **warm/served** only; neutral/slower on one-shot kernel-tests. | **No** for this gap. |
| JIT of type-checker CPS | **−15%**. Parked. | **No.** |
| Tagged `Value(u64)` | Closed 17×→~3× vs shen-cl; leftover 3× is the boxed model. | **Later.** Heap rewrite, not this plan. |
| Large-stack worker | 1 GB thread for kernel-tests AOT typecheck. | Already have 16 MiB mmap trampoline. Not the sample hotspot (`mmap` 0%). |

Rust overlay is **not** a cold `--kernel-tests` lever (loaded defuns ~0% of
that wall). **C `runme` is the opposite:** most of the 15 s **is** those
loads. Overlay is the right C lever.

## Recommended path

1. Overlay `interpreter.shen` + `prologinterp.shen` into `runme-aot-app`.
2. Direct AOT-to-AOT apply (skip intern on generated-to-generated).
3. Inline hot prims in `emit.c`.
4. Optional tc-cache on remaining walker loads.
5. Retimed L-interp / full runme. Matching Go (~5 s) is success if L-interp
   typecheck drops toward ~2–4 s **and** both 134/0 hold. Do not claim it
   from fib 0.01 s.

## Constraints

| Frozen | Meaning |
|--------|--------|
| Interpreter certify | `nix develop -c make CC=clang certify` → 134/0 |
| AOT runme | `yes \| bin/runme-aot-app/app` cwd `shen/test/s42` → 134/0 |
| Boehm | `shen_context` / `GC_malloc` |
| Wrap | Lambdas stay `shen_native_closure`; no raw `NativeFunction*` in lists |
| Emit | Still per-defun C, not `eval_kl_object` of source text |
| Sidecar load | `(load "interpreter.shen")` still runs (effects). Overlay **after**. |
| No JIT `t*` | rust falsified it |
| No VM for one-shot | rust: not a kernel-tests win |
| No `Value(u64)` this plan | structural leftover vs SBCL, not the first 3× |

## DAG

### O0 — freeze

Do not shake-replace `load`. Do not JIT. Do not drop Boehm. Do not unbox
fib. Do not implement rung 2. Do not rewrite certified `eval_kl_object`
into a VM.

### O1 — generate overlay modules (codegen only)

Compile `shen/test/s42/interpreter.shen` and `prologinterp.shen` through
the **same** `emit.c` path rust uses (`bootstrap`/KL → natives).

- Script analog of `scripts/codegen-shen-aot.sh`: KL from a clean boot,
  then `yggdrasil-build` / `emit` for those defuns only.
- Record source hash + kernel digest + `(name, arity)` list
  (`shen-rust` `OverlayModule`).
- Datatypes/`declare` are **not** in the overlay; they stay on the load.

**Kill:** generated C has `eval_kl_object` of the sidecar source string, or
zero defuns compiled.

### O2 — install after load

In `runme-aot-app` (or `load` primitive): after a successful `load` of a
hashed file, `shen_register_defun` each overlay name (and fill the direct
table once O3 exists).

- All-or-nothing: any arity/hash mismatch → **no** install, walker keeps
  serving (`install_overlay_if_match`).
- `defun` of an overlaid name must clear the direct slot (rust two-table
  bug). Test that.

**Kill:** L interpreter typecheck wall does not drop vs ~16 s baseline
**or** AOT runme is not 134/0. Stop. Do not proceed to O3.

Success target (not fib): stdout still `typechecked in 1177672 inferences`
(or current S42 count) and **Shen run time for that group well under 16 s**.
Quote `evidence/runme-aot.log`.

**Killed (O2).** Load-then-swap is implemented (`shen_wrap_load_for_overlays`,
bootstrap KL drops datatypes, hash/arity/kernel-digest, silent fallback)
and unit-tested, but it cannot move L-interp. `load.kl` times
`shen.load-help` **before** the post-load swap; that window is the
typecheck of sidecar datatypes/`define` through already-AOT `t*`
(`1177672` inf). Overlay of `normal-form` et al. only affects the two
sub-millisecond calls after load. Measured with overlay in runme:
**17.160915 s** / **17.162476 s** after inferences (prior),
**16.543452 s** / **16.544883 s** (gap). After unplugging overlay from
runme-aot (`evidence/runme-aot.log` `21:10Z`): **16.845703 s** /
**16.845723 s** after inferences, still **134/0**. Wall stays ~16 s, not
well under. Do not start O3. Remaining lever for this wall is faster
`t*` (O4 prim inline) or O5 tc-cache, not sidecar-defun overlay.

### O3 — `apply_direct`

AOT-to-AOT: name already interned at install; call a `NativeFunction*`
table, not `shen_intern` + `shen_apply` every time.

- Register order: wrapped cell first, then direct pointer.
- Fallback to `shen_apply` when the target is not in the table.
- Coherence test: redefine a name; next call must not hit the stale native.

**Kill:** exclusive intern+apply share of a new sample does not move, **or**
134/0 breaks.

### O4 — inline hot prims in emit

**Shipped (O4).** Exact-arity unbound klcompile prims emit ABI helpers
(`+` `-` `*` `/` `<` `>` `<=` `>=` `=` `cons` `hd` `tl` `cons?`
`number?` `string?` `symbol?` `absvector?`). `if` was already `emit_if`.
Do **not** alias `vector?` to `absvector?` — it is a kernel defun
(absvector? plus slot 0). Locals of those names and partial application
stay intern+apply. Overlay stays **unplugged** (O2 kill: load-help clocks
typecheck before swap). AOT runme `evidence/runme-aot.log`
`23:50:26Z`–`23:50:47Z`: intern **9150→7465**, apply **6030→4355**,
`shen_eq=761` `shen_cons_p=858` `eval_kl_object=0` `shen_native_closure=282`,
still **134/0**, overlay_wrap=0, Boehm `libgc.1.dylib`. L-interp
**13.623762 s** / **1177672** inf (retry **14.267524 s**, prior **15.351 s**,
Go **2.161 s**) — still ~14 s, not well under. Prologinterp **1.854 s**.
Do not start O3. Remaining lever is O5 warm tc-cache (already shipped)
or faster already-AOT `t*` beyond boxed prim helpers.

### O5 — tc-cache (optional, parallel-ok after O2)

Memoize `shen.typecheck` verdicts keyed the way rust `tc_cache.rs` does
(nesting-sound). One-shot `runme` is the metric. Kill if wall-neutral
(rust cached `shen->kl` was wall-neutral — do not re-litigate that).

**Shipped (O5), env-gated.** `SHEN_C_TC_CACHE=<dir>` wraps `load` +
`shen.typecheck` only (`shen.shen->kl` is not wrapped). Nesting-sound FNV
key (kernel digest, load chain, enclosing digest, file hash, tc flag,
gensym). Off by default so certify is unchanged. Warm disk cache on AOT
runme (`evidence/runme-aot-tc-warm.log` `21:32:23Z`): L interpreter
**2.039747 s** / replay **14/14** (baseline **16.182 s**, Go **2.041 s**),
still **134/0**, Shen **20.55 s**, **12.91** real. Cold record
(`evidence/runme-aot-tc-cold.log`) L-interp **23.88 s** / **1177672** inf
— first shot is not the win; rust kernel-tests 4.07→1.48 was the same
warm-file shape. Prologinterp warm **4.54 s** stays near baseline
**3.16 s** (datatype/`shen->kl` leftover). Overlay stays unplugged.

### O6 — rebar

**Done (O6).** Both **134/0**. Quoted walls in `docs/AOT-GAP.md`
(`evidence/runme-aot.log` `21:52:10Z`, `evidence/compare-runme-shen-go.log`
`21:52:35Z`, `evidence/certify.log` `21:52:53Z`). Overlay stays **killed**:
AOT L-interp **15.350518 s** / **1177672** inf vs Go **2.161 s** /
**1178236** inf (baseline **16.182 s**). Full AOT runme **15.31** real /
Shen **27.895 s** vs Go **4.89** real / Shen **4.670 s**. Matching Go is
**nice**, not the kill; L-interp stayed ~16 s. `overlay_wrap=0`, Boehm
`libgc.1.dylib`, `shen_native_closure=282`, `eval_kl_object=0`. No JIT
`t*`, no VM for one-shot, no fib unbox, no `Value(u64)`. Do not start O3.

## First slice (one PR)

O1+O2 for **`interpreter.shen` only** (prologinterp second PR).

1. Codegen overlay C from that file’s KL defuns.
2. After `load` in AOT runme, swap those names if hash matches.
3. Require AOT runme 134/0 **and** L-interp typecheck wall down from ~16 s.
4. Interpreter `make certify` 134/0.

Out of first PR: O3–O5, `Value(u64)`, JIT, VM, prologinterp overlay unless
it falls out of the same script.

## Rejected

- Standalone shake of `interpreter.shen` as the runme typecheck (wrong
  effects; C already tried).
- JIT / bytecode VM to win one-shot runme.
- intern HashMap cache as the 3× closer (sample ~7% exclusive intern).
- Generated `-O3` as the plan (no sample leaf).
- Option 4 heap frames (mmap already 0% in sample).
- Matching SBCL / rust’s leftover 3× vs cl via tagged words **first**.

## Evidence (binding)

- Gap: `docs/AOT-GAP.md`, `evidence/sample-runme-aot-summary.txt`.
- Rust overlay: `crates/shen-rust/src/aot/overlay.rs`,
  `design/aot-loaded-productionization-handoff.md`,
  `scripts/codegen-shen-aot.sh`, `benches/normal_form_aot.rs`.
- Rust apply_direct: `klcompile` header comment; `aot/runtime.rs`.
- C emit/apply: `src/c/emit.c`, `src/c/abi.c`.
- C runme AOT: `bin/runme-aot-app`, `test/fixtures/runme-aot/`.

## Workflow

`/shen-c-aot-overlay` with `args.stage`: `overlay` | `direct` | `inline` |
`tc-cache` | `rebar` | `all`. Default `overlay` = first PR. Kill after
overlay if L-interp wall does not move.
