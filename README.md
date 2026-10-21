# Shen-C

A [Shen](https://shenlanguage.org/) implementation in C17, based on
[otabat/shen-c](https://github.com/otabat/shen-c) and Mark Tarver's Shen 43.1
kernel. Shen provides pattern matching, macros, optional types, Prolog, and
Shen-YACC.

**Shen-C 0.4.0 brings a native C17 compiler, the complete Shen 43.1 portable
kernel, and reference counting with exact cycle reclamation.** The default
compiler, standalone CLI and generated C applications have no Boehm dependency.
The default backend also compiles loaded source, REPL forms, and explicit
`eval-kl` to native C before execution. Function bodies may compile on their
first call; they never run interpreted in native mode. See the
[compile-first design and qualification](docs/design/compile-first-runtime.md)
and [released-version notes](CHANGELOG.md).

## Binary downloads

Download a macOS or Linux archive for arm64 or x86_64 from the
[latest release](https://github.com/pyrex41/shen-c/releases/latest), verify it
against the attached `SHA256SUMS`, and extract it. Add its `bin` directory to
`PATH`; the whole directory can be moved without installation. The archive
includes `shen-c`, `shen-rc`, `rc-build`, the Shen source compiler and runtime
sources. Linux binaries target Ubuntu 24.04 or newer compatible glibc systems;
macOS binaries target macOS 15 or newer. macOS downloads are not notarized.

Dynamic source, REPL and `eval-kl` compilation require a C17 compiler (`cc` on
`PATH`, or set `SHEN_RC_NATIVE_CC`). Source application builds also require
Make and Python 3.9+. On macOS, install the Xcode Command Line Tools with
`xcode-select --install`. Closed AOT applications need no runtime compiler.

The complete 136-test suite passed in the latest packed-cache native qualification
in **8.17 seconds wall / 7.27 seconds user CPU**, including startup, loading
and typechecking, with zero compiler calls or interpreter entries. This is one
shared-host Darwin run, excluding initial compilation; repeated current-backend
performance acceptance remains open. See the
[timing receipt](docs/evidence/native-tail-qualification-20261019/release-suite.json).

## Released 0.3.0 full-suite performance

On the measured Darwin arm64 host, RC used **16.4% and 16.6% less user CPU than
rebuilt Boehm** in two batches of five alternating pairs, winning all ten CPU
comparisons and using roughly half the peak memory. All twenty executions
passed the complete **136-test Shen 43.1 suite**.

| Complete suite, median of ten runs per implementation | RC | Boehm |
|---|---:|---:|
| Wall time | **3.785 s** | 4.510 s |
| User CPU | **3.675 s** | 4.405 s |

These times include process startup, source loading, typechecking and test
execution. Both implementations used the same pinned kernel, suite, compiler/SDK
and input; each execution used a fresh writable suite copy. RC's largest peak
RSS was 32,063,488 bytes, 3.49% above the frozen pre-optimization RC baseline
and below Boehm in every pair. Ordinary macOS services remained active in the
user-accepted measurement window. These are host-specific comparisons of the
complete implementations; they do not isolate the cost of either memory manager.

The [qualification report](docs/performance/rc-43.1-final-20261017.md) preserves
every sample, artifact identity, reproduction command and rejected experiment.
These released-version measurements precede the compile-first backend; its cold
compilation and cached execution are measured separately. The current
[loaded execution controls](docs/performance/compiled-execution-20261019.md) and
[ownership and compile-time experiments](docs/performance/native-optimization-20261019.md)
record current evidence; repeated whole-suite acceptance remains open.
Historical [S42 measurements](docs/performance/rc-suite-identity-tail-20261016.md)
remain separate from the 43.1 comparison.

## How it works

The ownership optimizations are inspired by
[Perceus](https://www.microsoft.com/en-us/research/publication/perceus-garbage-free-reference-counting-with-reuse/),
the reference-counting and reuse approach implemented in
[Koka](https://koka-lang.github.io/koka/doc/book.html).
Shen-C moves values at their last use and inserts releases where ownership ends.
When a list cell has one owner, its storage can be reused instead of allocating
a replacement; shared cells follow an allocation path that preserves aliases.
This lets functional list transformations reuse memory while preserving their
observable results. Shen-C implements these techniques in its own C runtime.

[FP²: Fully in-Place Functional Programming](https://www.microsoft.com/en-us/research/publication/fp2-fully-in-place-functional-programming/)
formalizes the stronger goal of checking that functional algorithms need no
allocation. It is related research: Shen-C currently provides local ownership
analysis and selective reuse, without FP²'s checker or a guarantee of execution
without allocation. Exact cycle reclamation supports Shen's mutable graphs,
while the explicit diagnostic interpreter retains bounded evaluation plans. See the [compiler guide](docs/RC-BACKEND.md) and
[implementation record](docs/performance/rc-prepared-evaluator-20261017.md).

Runtime compilation uses a system C compiler and a persistent cache. Symbol and
live literal bindings are supplied to compiled modules separately, allowing code
with the same shape to share machine code while preserving object identity and
dynamic redefinition. Compiled closures use the same ownership moves, releases,
and guarded reuse as ahead-of-time applications. If the C compiler fails or
exceeds `SHEN_RC_COMPILE_TIMEOUT`, that form is interpreted instead, with one
stderr notice; `SHEN_RC_VERIFY_NATIVE=1` makes compiler failures errors.
`SHEN_RC_EXECUTION=interpret` explicitly selects the diagnostic backend when
creating a context.

Cold compilation costs more than cached execution. `SHEN_RC_NATIVE_CC` selects
the compiler, `SHEN_RC_NATIVE_CACHE` selects the cache directory (`off` disables
persistence), and `SHEN_RC_NATIVE_STATS=1` reports compilation counters. The disk
cache defaults to 1 GiB (`SHEN_RC_NATIVE_CACHE_BYTES`); loaded modules remain
resident until process exit. Closed AOT applications need no runtime compiler;
uncached dynamic code does.

## String and vector work

The experimental [`perf/string-vector-fusion` branch](https://github.com/pyrex41/shen-c/tree/perf/string-vector-fusion)
constructs concatenated strings (`cn`, used by `@s`) directly in their final
buffer, removing a temporary allocation and a second copy. Focused concatenation
probes used **20–32% less CPU**; complete-suite timing was effectively unchanged.

An optional `tlstr` prototype shares immutable string storage, with a
bounded retention policy. An 8 KiB scan copied **4,701 bytes instead of
33.6 MB**, but this did not improve the complete suite, so sharing stays off
by default. Mutable vector tails (`tlv`) need independent storage to preserve
aliases; fusing `@s`/`@v` chains into one fresh final allocation is the next
candidate, and remains unimplemented. See the
[measurements and semantic controls](https://github.com/pyrex41/shen-c/blob/perf/string-vector-fusion/docs/performance/string-vector-fusion-20261018.md).

## Build

A C17 compiler, Make, Python 3.9 or newer, and an archiver are sufficient. Nix is optional.

```sh
make CC=clang
./bin/shen-c --version
./bin/shen-c eval -e '(+ 20 22)'
```

`bin/shen-c` and `bin/shen-rc` run the same standalone implementation. The
compiled kernel is embedded; the executable can run outside the source tree.
Like the other ports' images, it includes the Shen Standard Library (`sort`,
`for`, string, maths, tuple and file functions), taken unchanged from the
shen-sources 43.1 `klambda/stlib.kl` and pinned in the release manifest.
`bin/rc-build` compiles KL input into native C applications.

CMake also defaults to the Boehm-free toolchain:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/shen-c eval -e '(+ 20 22)'
```

## Use Shen

```sh
./bin/shen-c                       # REPL; stdin EOF exits
./bin/shen-c eval -e EXPR
./bin/shen-c eval -l FILE
./bin/shen-c script FILE ARGUMENTS
```

The script receives `[FILE | ARGUMENTS]` in `*argv*`. As in other ports'
launchers, a script prints its program output without echoing each form's
return value or a run time; a `load` it calls prints as an ordinary load.
`eval -l` keeps its interactive load output. `eval -q` quiets language output to stdout while
retaining explicit file writes.

Commands run on a 1 GiB evaluator stack, so deep non-tail recursion works;
`SHEN_RC_STACK_SIZE` (for example `4G`) changes it. Exhausting the stack is a
`stack exhausted` error that `trap-error` can catch, not a crash.

## Compile applications

See [the compiler guide](docs/RC-BACKEND.md) for Shen source compilation, KL
manifests, generated C, ownership controls, and standalone deployment. The
default runtime preserves mutable cycles and reclaims unreachable cycles.
An optional acyclic-only configuration rejects writes that would create
cycles; it is a restricted mode, not the default Shen semantics.

## Validate

```sh
make test
make sanitize
scripts/test-rc.sh --no-build bin/rc-build
scripts/certify-rc.sh
```

The certification command rebuilds a fresh Boehm-free compiler, compiles all
14 bundled kernel modules and the standard library, runs the pinned release suite in a fresh writable directory, and audits both the
compiler and generated executable. Passing requires **136 passed, 0 failed**, all 36 intermediate reports, and the expected redirect output,
successful process exit, matching source hashes, and no Boehm dependencies.
Each run has bounded execution and a separate evidence directory.

The [validation record](docs/rc-validation.md) distinguishes the earlier
experimental checkpoint from the current compiler and ownership changes.
Historical Boehm results are described in
[LEGACY-BOEHM.md](docs/LEGACY-BOEHM.md).

## Learn Shen

- [Official website](https://shenlanguage.org/)
- [Shen OS Kernel manual](https://shenlanguage.org/learn-shen/index.html)
- [Community wiki](https://github.com/Shen-Language/wiki/wiki)

## License

Shen: Copyright 2010–2022 Mark Tarver, [BSD license](shen/LICENSE.txt).
Shen-C: Copyright 2022 Tatsuya Tsuda and contributors,
[MIT license](LICENSE).

The [43.1 continuation](docs/qualification/shen-43.1-continuation.md) records the current upgrade, matched comparator, and acceptance gates. Release manifests live in `shen/releases/`; select one with `SHEN_RELEASE_MANIFEST` for certification and source compilation, `--manifest` for benchmarks, `RC_RELEASE_MANIFEST` for Make, or `SHEN_RELEASE_MANIFEST` for CMake. Historical S42 timings remain separate.
