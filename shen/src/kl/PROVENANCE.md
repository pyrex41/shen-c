# Kernel provenance

## ShenOSKernel 42.2 (community kernel) — current

The kernel under `shen/` is the community **ShenOSKernel-42.2** release,
vendored byte-for-byte. `(version)` is `"42.2"`.

- Upstream: [Shen-Language/shen-sources](https://github.com/Shen-Language/shen-sources),
  tag `shen-42.2`, commit `73507e069fe147aefb728d632c4ce40bc6a7b314`.
- Release archive: `ShenOSKernel-42.2.tar.gz`,
  SHA-256 `1d02273003654d34ec020ae67cb214ecec410dee3350419ff807d82b652678d8`.
- Reference port for behaviour: shen-scheme 0.50 (`(version)` = 42.2).

Lineage choice: option (a) of the 42.2 upgrade — adopt the community kernel
wholesale and replace the Tarver S42 lineage below. The two lineages are not
mixed: no Tarver `.kl` file, `declarations.kl` top-level form or
`types.kl` declare form is loaded any more.

Vendored paths (only this file is non-upstream):

| Repo path | Upstream path |
|-----------|---------------|
| `shen/src/kl/*.kl` | `klambda/{core,declarations,dict,init,load,macros,prolog,reader,sequent,sys,t-star,toplevel,track,types,writer,yacc,stlib}.kl` |
| `shen/src/extensions/*.kl` | `klambda/extension-*.kl` |
| `shen/src/shen/` | `sources/*.shen`, `sources/README.txt`, `extensions/*.shen` |
| `shen/test/kernel/` | `tests/` (the 42.2 suite, 143 tests) |
| `shen/doc/`, `shen/VERSION`, `shen/CHANGELOG.md`, `shen/LICENSE.txt` | `doc/`, `VERSION`, `CHANGELOG.md`, `LICENSE.txt` |

### Boot

The 42.2 `.kl` files hold only `defun`s, so their load order does not
matter. `load_shen_kl_files` (`src/c/repl.c`) does:

1. Load the 16 kernel modules: `sys writer core reader declarations toplevel
   macros load prolog sequent track t-star yacc types dict init`.
2. `shen_kernel_aot_install_all()` — overwrite those defuns with the
   NativeFunctions that `bin/kernel-aot-build` generated from the same files
   (`obj/kernel_aot/*.c`, dict and init included). `SHEN_C_NO_AOT=1` skips it.
3. `shen_apply_port_overwrites()` — the C port overwrites (`hash`, `map`,
   `vector`, `shen.fillvector`, `pr`, `shen.char-stoutput?`, …).
4. `call_shen_initialise()` — `(shen.initialise)` from `init.kl`, which sets
   the globals, builds `*property-vector*` as a `shen.dict`, the arity table,
   the lambda forms and the datatype declarations. It runs after the C `hash`
   is installed, so the property dict is written and read with one hash.

REPL entry is `shen.repl` (S42's was `shen.shen`). `stlib.kl` and the
`extension-*.kl` files are vendored but not booted, so `stlib.initialise` is
not called.

### Native audit against 42.2

- No C native reads the property store or lambda table: `get`, `put`,
  `unput`, `arity`, `fn`, `shen.lambda-form-entry` and the `shen.dict*`
  functions run the 42.2 kernel code (AOT-compiled). The old unprefixed
  `dict`/`<-dict`/… C primitives are not used by the 42.2 kernel.
- There is no native `macroexpand` / `shen.macroexpand-h`, so the 42.1
  source-form handler argument needs nothing from the port.
- The C overwrites of kernel functions (`hash`, `symbol?`, `integer?`,
  `boolean?`, `variable?`, `not`, `map`, `fix`, `shen.fix-help`, `reverse`,
  `append`, `element?`, `assoc`, `occurrences`, `nth`, `limit`, `length`,
  `shen.fillvector`, `vector`, `shen.byte->digit`, `shen.arg->str`, `pr`)
  replace definitions that are identical (up to variable names) between S42
  and 42.2.
- `hash` maps an integral float to the integer before hashing, so values that
  are `=` (1 and 1.0) land in the same `shen.dict` bucket.
- `read-byte` returns 0..255 and -1 at end of stream. On stdin, EOF right
  after a newline (or with no input) still exits the REPL; EOF in the middle
  of a line returns -1 once so the kernel reader reads a final line that has
  no trailing newline.
- `pr` keeps the host override: `*hush*` gates stdout only.

### Tests

`make certify` runs `shen/test/kernel/runme.shen`: **passed 143 / failed 0**.
`shen/test/s42/` (Tarver's S42 tests) is kept because the Yggdrasil AOT
fixtures under `test/fixtures/` (shaken from the S42 kernel) read
`interpreter.shen` and friends from it.

## History: Shen 42.0 (S42), Mark Tarver — replaced

Until the 42.2 upgrade the 15 canonical `.kl` files were byte-identical
copies of Mark Tarver's S42 archive (`KLambda/`).

Canonical source: `pyrex41/shen-upstream`, tag `s42-pristine-20260825`.
Original archive: https://www.shenlanguage.org/Download/S42.zip
Archive SHA-256: `30abdc7e5a1e27b7a20109c1ed141e4712885e31f24d9710d16415fbbd4dfb23`

Canonical files: `backend.kl`, `core.kl`, `declarations.kl`, `load.kl`,
`macros.kl`, `prolog.kl`, `reader.kl`, `sequent.kl`, `sys.kl`, `t-star.kl`,
`toplevel.kl`, `track.kl`, `types.kl`, `writer.kl`, and `yacc.kl`.
`backend.kl` was vendored for audit completeness but not booted.

Boot order followed S42 `install.lsp` (without `backend.kl`):

    sys writer core reader declarations toplevel macros load
    prolog sequent track t-star yacc types

S42 had no `dict.kl`, `init.kl`, or `shen.initialise`. Kernel globals, the
arity table, and `*property-vector*` were set by top-level forms in
`declarations.kl` (`shen.build-lambda-table`), with `types.kl` declare
forms; REPL entry was `shen.shen`. `get`/`put`/`unput` lived in `sys.kl` on
property vectors. Shen-C overwrote `vector` and `shen.fillvector` with
iterative C so `(vector 20000)` does not recurse 20000 `eval_kl_object`
frames (still true on 42.2). Community 22.4 C overwrites of the Prolog engine
were not applied. That kernel passed Tarver's suite 134 / 0.
