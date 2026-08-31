# Kernel provenance

## Shen 42.0 (S42), Mark Tarver

The 15 canonical `.kl` files are byte-identical copies of Mark Tarver's S42
archive (`KLambda/`).

Canonical source: `pyrex41/shen-upstream`, tag `s42-pristine-20260825`.
Original archive: https://www.shenlanguage.org/Download/S42.zip
Archive SHA-256: `30abdc7e5a1e27b7a20109c1ed141e4712885e31f24d9710d16415fbbd4dfb23`

Canonical files: `backend.kl`, `core.kl`, `declarations.kl`, `load.kl`,
`macros.kl`, `prolog.kl`, `reader.kl`, `sequent.kl`, `sys.kl`, `t-star.kl`,
`toplevel.kl`, `track.kl`, `types.kl`, `writer.kl`, and `yacc.kl`.

The `extension-*.kl` files are Shen port extensions and remain outside the
canonical inventory (`shen/src/extensions/`). `backend.kl` is vendored for
audit completeness but is not booted, matching upstream's precompiled Common
Lisp backend behavior.

Boot order follows S42 `install.lsp` (without `backend.kl`):

    sys writer core reader declarations toplevel macros load
    prolog sequent track t-star yacc types

There is no `dict.kl`, `init.kl`, or `shen.initialise`. Kernel globals, the
arity table, and `*property-vector*` are set by top-level forms in
`declarations.kl`. REPL entry is `shen.shen`.

`get`/`put`/`unput` live in `sys.kl` on property vectors. Shen-C overwrites
`vector` and `shen.fillvector` with iterative C after `sys.kl` so
`(vector 20000)` does not recurse 20000 `eval_kl_object` frames. Community 22.4
C overwrites of the Prolog engine are not applied: they crash on the first
`declare` in `types.kl`. The evaluator stays recursive; there is no Option 4
trampoline.
