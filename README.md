# Shen-C

A [Shen](https://shenlanguage.org/) implementation in C17, based on
[otabat/shen-c](https://github.com/otabat/shen-c) and Mark Tarver's Shen 43.1
kernel. Shen provides pattern matching, macros, optional types, Prolog, and
Shen-YACC.

The default toolchain uses **reference counting and exact cycle reclamation**.
The compiler, standalone executable, and generated C applications have no
Boehm dependency. The complete kernel is compiled to native C; explicit
`eval-kl` and source loading use the independent RC evaluator.

The compiler inserts last-use ownership moves and drops, captures free
variables, transfers arguments through owned calls, and reuses uniquely owned
cons cells. Shared values keep their aliases intact. These optimizations are
inspired by Perceus and implemented directly in C; Koka is not a build dependency.
Their implemented scope and evidence are described in
[RC-BACKEND.md](docs/RC-BACKEND.md) and
[the validation record](docs/rc-validation.md).

The current 43.1 candidate passes all 136 suite assertions, fatal sanitizer
qualification, Make/CMake, and the installed Darwin Nix checks. The performance
target remains **OPEN**: two five-pair batches were 10.3% and 8.1% slower in user
CPU than rebuilt Boehm, with about 58% lower peak RSS. Linux and remote CI remain
unverified. See the [43.1 qualification record](docs/qualification/shen-43.1-continuation.md).

The historical four alternating full S42 pairs passed 134/0 for both implementations.
RC remained 0.9% slower in paired wall time and 4.1% slower in paired user CPU,
with 62.9% lower peak memory. Other host workloads affected timing; this does not
establish parity or a speedup. Performance stays OPEN; see the
[identity-tail checkpoint](docs/performance/rc-suite-identity-tail-20261016.md)
for matched measurements and the exact correctness qualification scope.

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
