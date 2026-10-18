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

The approach combines compiler knowledge about ownership with less repeated
work in the evaluator:

- **Move values at their last use.** Generated C transfers ownership when a
  value is no longer needed, captures only free variables, and drops values at
  known boundaries. A uniquely owned cons cell can be reused; shared cells keep
  their aliases intact. Certified internal call boundaries can borrow existing
  roots and skip redundant argument copies or cleanup scans. Public borrowed
  and owned C contracts remain unchanged.
- **Prepare hot interpreted functions once.** Frequently used function bodies
  get bounded evaluation plans that record expression kinds and lexical slots.
  Repeated calls avoid walking the same syntax and searching the same names.
  Parameter and local environments are materialized when a fallback or escaping
  closure needs them. Cold and unsupported code uses the general evaluator.
- **Guard specialized paths.** Native predicates and selected Prolog operations
  avoid unnecessary temporary values and ownership work when their exact
  preconditions hold. Runtime function guards and dependency-cache invalidation
  preserve dynamic redefinition; uncertain cases use the general paths.
- **Keep full graph semantics.** Reference counting handles ordinary lifetime
  management, while exact cycle reclamation handles unreachable cyclic graphs.
  Mutable data, shared values and escaping closures retain their semantics.
  Reuse caches and evaluation plans have explicit bounds.

Separate diagnostic runs recorded **22.7% fewer retain/release operations** and
**7.6% fewer managed allocations** than the pre-optimization RC implementation.
Those counters explain part of the reduced work; diagnostic timings are not
used for the release performance claim.

The ownership optimizations are inspired by Perceus and implemented directly
in C. This is a bounded set of compiler/runtime optimizations; Koka is not a
build dependency. The [compiler guide](docs/RC-BACKEND.md),
[implementation record](docs/performance/rc-prepared-evaluator-20261017.md) and
[validation record](docs/rc-validation.md) describe their scope and controls.
Qualification includes the complete generated kernel under fatal ASan/UBSan,
default/boxed runtime and emitted semantic controls, Make/CMake, installed Nix
packages, and Linux Clang/GCC and macOS CI.

## Build

A C17 compiler, Make, Python 3.9 or newer, and an archiver are sufficient. Nix is optional.

```sh
make CC=clang
./bin/shen-c --version
./bin/shen-c eval -e '(+ 20 22)'
```

`bin/shen-c` and `bin/shen-rc` run the same standalone implementation. The
compiled kernel is embedded; the executable can run outside the source tree.
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

The script receives `[FILE | ARGUMENTS]` in `*argv*`. `eval -q` quiets
language output to stdout while retaining explicit file writes.

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
14 bundled kernel modules, runs the pinned release suite in a fresh writable directory, and audits both the
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
