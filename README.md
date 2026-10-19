# Shen-C

A [Shen](https://shenlanguage.org/) implementation in C17, based on
[otabat/shen-c](https://github.com/otabat/shen-c) and Mark Tarver's Shen 43.1
kernel. Shen provides pattern matching, macros, optional types, Prolog, and
Shen-YACC.

**Shen-C 0.3.0 brings a native C17 compiler, the complete Shen 43.1 portable
kernel, and reference counting with exact cycle reclamation.** The default
compiler, standalone CLI and generated C applications have no Boehm dependency.
The kernel is compiled to native C; source loading and explicit `eval-kl` use
the RC evaluator. See the [release notes](CHANGELOG.md).

## Full-suite performance

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
while bounded evaluation plans separately reduce repeated syntax classification
and lexical lookup. See the [compiler guide](docs/RC-BACKEND.md) and
[implementation record](docs/performance/rc-prepared-evaluator-20261017.md).

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
[MIT license](http://www.opensource.org/licenses/MIT).

The [43.1 continuation](docs/qualification/shen-43.1-continuation.md) records the current upgrade, matched comparator, and acceptance gates. Release manifests live in `shen/releases/`; select one with `SHEN_RELEASE_MANIFEST` for certification and source compilation, `--manifest` for benchmarks, `RC_RELEASE_MANIFEST` for Make, or `SHEN_RELEASE_MANIFEST` for CMake. Historical S42 timings remain separate.
