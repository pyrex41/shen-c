# Option 5 rung 2 — documentation only

Stage `rung2-doc`. This is not an implementation.

The proof list lives in `docs/option5-rung2.md`. Do not treat either file
as evidence that rung 2 ships.

Rung 1 (this tree) generates C from shaken KLambda. Each `defun` becomes a
C `NativeFunction` that intern/cons/applies primitives. Allocations stay
on `shen_context` / Boehm `GC_malloc`. `eval-kl` remains available for
`needs-eval` shakes. The certified interpreter is unchanged.

Rung 2, when it exists, is a later stage:

* lifetime-inserted non-GC C (no Boehm for KLObject graphs)
* no `eval-kl` in the live set
* no REPL in the artifact
* not Chicken-on-C-stack, not a C++ rewrite, not libc `malloc`/`free` of
  today's `KLObject` tagged cells as a “GC replacement”

Keep Boehm on rung 1.
