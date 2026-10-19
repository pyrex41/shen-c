# Native C compiler and reference-counted runtime

The default build is independent of Boehm. `rc-build` reads KL with the RC
reader and emits native C functions; `shen-rc`/`shen-c` embeds the complete
compiled Shen 43.1 kernel. Loaded source, REPL expressions and explicit
`eval-kl` use native compile-before-execute by default. A system C compiler is
needed for uncached dynamic code; failures are reported as errors. Native
compilation thunks may defer a function body until its first call, without
interpreting it. `SHEN_RC_EXECUTION=interpret`, selected at context creation,
provides the explicit diagnostic interpreter.

## Build and run

```sh
make CC=clang
bin/shen-c --version
bin/shen-c eval -e '(+ 20 22)'
bin/shen-c script program.shen
```

The default Make and CMake builds perform no Boehm or pkg-config discovery.
C17, Make, and an archiver are sufficient for the toolchain; the source compiler
driver and validation scripts also use Python 3. Nix is optional.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The executable contains the kernel and can run outside the source tree.
`SHEN_RC_HOME` optionally sets a prefix for file loading. `--help` describes
evaluation, source loading, scripts, and the REPL.

## Compile Shen source

```sh
scripts/compile-shen-rc.sh program.shen out/program
out/program/app
```

The driver uses the standalone CLI's kernel reader/compiler to lower Shen
source to KL, then invokes the independent native compiler. It retains the
generated C and build inputs with a provenance record. Application top-level
expressions execute when the generated program runs. Source files are preserved.

The frontend runs the bundled reader and macro compiler in a fresh kernel
context. An explicit metadata sidecar preserves ordered package registrations,
definition arities and stored source, macros, datatypes, and synonyms. Auxiliary
functions created during reading are compiled to native C. The sidecar is
separate from ordinary program forms, so no user function name is reserved as
a metadata sentinel. An explicit `escaped-v1` transport preserves string bytes
and symbol identity, including atoms that cannot be written as ordinary KL
syntax; the raw KL reader remains unchanged.

Reader and macro processing occur at compile time. Compiler-generated fresh
names may differ from an interpreted load. Explicit `load` and `eval` operations
remain runtime operations and may require external files. The source compiler
reports unsupported persistent reader state instead of silently dropping it.
`SHEN_RC_CLI` and `RC_BUILDER` select explicit executables; `SHEN_C_HOME` selects
the compiler's kernel/runtime source installation.

## Compile KL directly

```sh
SHEN_C_HOME="$PWD" bin/rc-build test/fixtures/sum out/sum
out/sum/app
```

Input directories contain `kernel.kl`, `user.kl`, and optionally
`rc.manifest.txt` or `yggdrasil.manifest.txt`. The manifest supports `kernel=`,
repeated `user=`, and an optional `init=` function. `rc-build` emits `app.c` and
a Makefile and builds `app`. `CC`, `CPPFLAGS`, and `CFLAGS` select the C compiler
and instrumentation. The kernel source installation is a build-time dependency;
generated executables do not need it at runtime.

`SHEN_RC_PRINT=1` prints user top-level expression results for diagnostics.
`SHEN_RC_STATS=1` reports allocation, reference-count, closure-capture, and pair
reuse counters on stderr. `--conservative-ownership` disables ownership
optimizations for semantic and allocation comparisons.

## Ownership and reuse

The runtime exposes borrowed and explicitly consuming APIs. Borrowed calls
retain their inputs as needed; owned calls transfer argument slots and clear
the caller's slots. Returned values are owned. The header documents exceptions
such as `rc_require_boolean`, which consumes its owned input.

The emitter performs backward local liveness analysis across lexical scopes,
branches, short-circuit conditions, and error handlers. Last uses move values;
unused values are released. Closure captures contain lexical free variables.
Error handler references remain live through protected expressions. Native
functions adopt owned arguments, so unique inputs can stay unique across calls.

Pair reuse is selective. Recognized cons/tail traversals retain exact primitive
identity checks and detach unique pair storage only when the old value has no
remaining use. Shared pairs follow an allocation path that preserves aliases.
The runtime keeps detached storage rooted and valid through recursive calls,
errors, and collection. This supports actual recursive list rebuilding; it is
not generalized whole-program Perceus analysis or arbitrary allocation reuse.

Large generated frames use heap slabs and share one borrowed argument buffer
per function. Consecutive dead slots use compact cleanup loops, preserving
release order while avoiding excessive generated C and sanitizer stack frames.
Tail calls use the owned trampoline. Ordinary non-tail recursion
still uses the C stack. Cleanup is explicit; no nonlocal jump crosses runtime
ownership cleanup.

## Cycles

The default preserves unrestricted mutable vectors. Acyclic objects are freed
when their count reaches zero, using iterative destruction for deep graphs.
Unreachable cycles are reclaimed by an exact heap pass: copy counts, subtract
internal edges, mark external roots, and reclaim unmarked graphs. The collector
does not scan the C stack. Interned symbols and global/function bindings have
explicit context ownership.

Cycle reclamation is part of automatic memory management. The user confirmed
that it is acceptable; the requirement is complete removal of Boehm from the
compiler and emitted runtime. Perceus's collector-free guarantee itself applies
to cycle-free programs; see [the paper, section 2.7.4](https://www.microsoft.com/en-us/research/uploads/prod/2021/06/perceus-pldi21.pdf).

An optional restricted configuration is available:

```sh
make rc-acyclic
bin/rc-build --acyclic input-dir out/acyclic-app
```

`SHEN_RC_ACYCLIC_ONLY` omits the cycle collector and rejects a vector write
that would create a cycle, before changing the vector. This changes which Shen
programs are accepted. It is not the default language compatibility mode.

## Runtime compilation

Native expression modules return owned values without bootstrapping the kernel
or printing results. Captured runtime bindings preserve the identity of live
vectors, streams and closures. Numeric/string constants and symbols are also
bound separately, allowing identical code shapes to share cached machine code.
Root closures and definitions can compile their bodies lazily; exact curried
forwarders and identity bodies use precompiled native templates. Dynamic name
lookup, redefinition, partial application and escaping closures remain available.

`SHEN_RC_NATIVE_CC` selects the compiler and `SHEN_RC_NATIVE_CFLAGS` defaults to
`-O1`. `SHEN_RC_NATIVE_CACHE` selects a private cache directory (`off` uses a
private temporary directory); `SHEN_RC_NATIVE_CACHE_BYTES` bounds code artifacts
(default 1 GiB, with header/accounting overhead). `SHEN_RC_COMPILE_TIMEOUT` bounds
each compiler invocation (default 120 seconds). Compiler diagnostics are capped
at 8 MiB. Cache publication is serialized and atomic, and object receipts reject
corrupt artifacts. `SHEN_RC_NATIVE_STATS=1` reports compilation, allocation,
collection and ownership counters. Loaded native modules stay mapped until
process exit; the disk budget does not bound resident code growth.

Closed AOT applications execute without launching a compiler when they perform
no dynamic code generation. Applications using `load` or `eval-kl` share the
native backend. Both Make and CMake link runtime compilation support; generated
Makefiles include its sources and embedded-header configuration.

## Historical hot-definition tier (diagnostic interpreter only)

In explicit interpreter mode, source loading and `eval-kl` interpret runtime-defined defuns. With
`SHEN_RC_NATIVE=1`, the CLI counts calls to them; once a defun reaches
`SHEN_RC_NATIVE_THRESHOLD` calls (default 1000), it is compiled with this
emitter and the system C compiler, loaded with `dlopen`, and published while
its binding is unchanged. Objects are cached by content under
`SHEN_RC_NATIVE_CACHE` (`off` disables the cache). Any failure, including a
missing compiler, keeps interpreting. `SHEN_RC_NATIVE_STATS=1` reports
counters. A warm cache saves about 10% of suite CPU on the measured host,
but a cold compile costs several times the whole suite. See
[the measurements](performance/rc-native-tier-20261018.md). Generated
applications never attach the tier.

```sh
SHEN_RC_EXECUTION=interpret SHEN_RC_NATIVE=1 bin/shen-c eval -l program.shen
python3 scripts/test-native-tier.py bin/shen-c
```

## Validation

```sh
make test
make sanitize
scripts/test-rc.sh --no-build bin/rc-build
scripts/certify-rc.sh
scripts/test-rc-toolchain.sh --canonical-receipt evidence/rc/certify-TIMESTAMP/result.json
```

The canonical runner rebuilds a fresh standalone compiler, audits it, compiles
all 14 portable KL modules, and loads the unchanged S42 `runme.shen`. It requires
134 passed / 0 failed, clean exit, unchanged source hashes, and a generated
artifact audit. Each run records build commands, compiler flags, timings,
source identities, and executable hashes. Build/run time limits default to
300/600 seconds; `RC_BUILD_TIMEOUT` and `RC_SUITE_TIMEOUT` override them.
Timeouts and output-limit interruptions remain UNKNOWN. Output is capped at
64 MiB per supervised process.

The toolchain runner independently rebuilds the default toolchain with an
unavailable pkg-config path, checks both executable names, CLI behavior, real
Shen source compilation, native output, optimizer controls, and dependency
audits. `--skip-canonical` explicitly selects a focused run.

See [the validation record](rc-validation.md) for executed results and their
source snapshots. Historical Boehm benchmarks are not performance measurements
of this backend. The legacy overlay, dictionary, and typecheck-cache extension
interfaces remain historical source, documented in [LEGACY-BOEHM.md](LEGACY-BOEHM.md).
