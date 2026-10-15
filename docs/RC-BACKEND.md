# Reference-counted native C backend

The experimental RC backend compiles KL definitions to C and links them to
`src/rc`. It is separate from the existing `KLObject`/Boehm runtime. Ordinary
generated functions execute native C; explicit `eval-kl` uses the new dynamic
evaluator. Loading Shen source uses the compiled Shen kernel to translate source
to KL, then evaluates that KL in the RC runtime.

## Build and run

The compiler host uses the project's existing Nix toolchain and Boehm reader:

```sh
nix develop -c make CC=clang bin/rc-build
nix develop -c env SHEN_C_HOME="$PWD" bin/rc-build test/fixtures/sum bin/rc-sum
bin/rc-sum/app
```

The builder accepts a directory containing `kernel.kl`, `user.kl`, and an
optional `rc.manifest.txt` or `yggdrasil.manifest.txt`. Manifest `kernel=`, repeated
`user=`, and optional `init=` entries identify the input files and initializer.
It emits `app.c` and a Makefile, then builds `app`. `CC` and `CFLAGS` select
the generated application's compiler and instrumentation. `SHEN_RC_PRINT=1`
prints user top-level results for diagnostic use.

Runtime tests can build on their own with a C17 toolchain, without Boehm or
pkg-config:

```sh
make -f Makefile.rc test
make -f Makefile.rc sanitize
cmake -S . -B build-rc -DSHEN_RC_ONLY=ON -DSHEN_RC_SANITIZE=ON
cmake --build build-rc
ctest --test-dir build-rc --output-on-failure
```

Generated semantic tests, large-frame regressions (Python 3 required), and
artifact dependency checks:

```sh
scripts/test-rc.sh --no-build bin/rc-build
scripts/test-rc.sh --audit-only bin/rc-sum/app
```

The complete bundled kernel and untouched canonical S42 suite have a separate
certification command (requires Python 3 for bounded process supervision):

```sh
scripts/certify-rc.sh
```

It assembles all 14 portable KL modules in boot order and compiles a user form
that loads `runme.shen`. It does not use the older shaken fixture, whose omitted
definitions depended on legacy native overrides. The raw kernel performs its
own top-level initialization and has no synthetic `shen.initialise` entry.

Each run writes logs and a JSON record under a fresh `evidence/rc/certify-*`
directory. Success requires the final 134 passed / 0 failed report, successful
process exit, a Boehm dependency/symbol audit, and unchanged source hashes.
The record includes the checkout revision, verifies that suite files match that
revision, and hashes the builder binary and generated C/Makefile. Source hashes
record the current checkout; they do not prove that a custom builder binary was
compiled from those sources. Preserve the compiler-host build log separately.
Interrupted runs remain UNKNOWN with their termination reason.
Build and run timeouts default to 300 and 600 seconds; override with
`RC_BUILD_TIMEOUT` and `RC_SUITE_TIMEOUT`. Each process output is capped at
64 MiB. The compiler binary can be selected with `RC_BUILDER`.

## Ownership and collection

Public runtime inputs are borrowed and returned values are owned, except
`rc_require_boolean`, which consumes its owned input (as declared in `rc.h`). Heap edges
retain their targets. Assignments retain the new value before releasing the
old value, including self-assignment. Generated functions own temporary slots,
drop consumed expression temporaries, move final local results, and release
remaining slots on normal, error, and tail-return paths. Large generated frames
use a heap slab; each function reuses one borrowed argument/capture buffer to
keep C stack use bounded by its largest call. Non-tail recursion still uses the
C stack. Closures currently
capture all visible local bindings conservatively.

Zero reference counts trigger iterative destruction, including deep lists.
The cycle pass copies counts, subtracts internal heap edges, marks from objects
with residual external references, and frees unmarked graphs. It follows
vectors, pairs, closure captures, partial arguments, and tail requests. It does
not scan the C stack. Context-owned interned symbols and global/function slots
are explicit roots. Interned symbols live until context teardown.

Tail requests own their function and arguments before the caller cleans up.
The trampoline handles partial and excess application. Boolean tail results
carry validation through the trampoline; errors return through explicit paths.
There is no `longjmp` across ownership cleanup.

This is automatic memory management with cycle collection. It does not claim
collector-free unrestricted Shen, complete Perceus precision, allocation reuse,
or an established performance improvement over Boehm.

## Compatibility boundaries

- This backend produces applications, not a replacement `shen-c` CLI/REPL.
- Global definitions execute in user source order; lexical function bindings
  take precedence. Calls resolve their function before argument side effects.
- Dynamic `eval-kl` starts with a global lexical environment. Some legacy
  evaluator behavior differs from this and from its own AOT emitter.
- Strings preserve explicit byte lengths and unsigned bytes, including NUL.
  Byte conversion rejects out-of-range input. Integer overflow raises an error
  instead of invoking C signed-overflow behavior.
- Error handler values are strings. Diagnostic `rc_print` is not the Shen
  pretty-printer; the loaded kernel supplies language printing.
- Legacy dictionary extensions, `c-loop`, `c-recur`, `c-mcons`, native overlay
  installation, and typecheck cache integration have not been migrated.
- The earlier region-only/no-eval rung-2 proposal describes a different design.
  Its acceptance obligations are not discharged by this RC backend.

See [the validation record](rc-validation.md) for executed checks and remaining
product gates. The historical default-backend results in the main README are
not RC performance or qualification evidence.
