# Shen-C

A [Shen](https://shenlanguage.org/) implementation in C17, based on
[otabat/shen-c](https://github.com/otabat/shen-c). Shen provides pattern
matching, macros, optional types, Prolog, and Shen-YACC.

**Kernel lineage.** The main branch embeds the community ShenOSKernel 43.2,
the official [Shen-Language/shen-sources](https://github.com/Shen-Language/shen-sources)
release `shen-43.2` (commit `e6947f507ebc`), byte for byte. `(version)` is
`43.2`. This is the kernel of Shen/Scheme, Shen/CL and the other community
ports, including `dict`, `init` and the bundled extensions. Earlier community
kernels stay selectable: upstream `ec16a9f` (release 43.1 plus the
type-checker fix, `(version)` `43.1`) with
`RC_RELEASE_MANIFEST=shen/releases/shenoskernel-43.1-ec16a9f.json`, and the
official 43.1 release with
`RC_RELEASE_MANIFEST=shen/releases/shenoskernel-43.1.json`. Releases up to
and including 0.4.0 embedded Mark Tarver's S43 kernel (the S-series lineage
from shenlanguage.org) and ran its 136-test suite. See the
[43.1 migration record](docs/kernel-43.1-migration.md), the
[ec16a9f record](docs/kernel-43.1-ec16a9f.md) and the
[43.2 record](docs/kernel-43.2.md).

`bin/shen-c` and `bin/shen-rc` run the same standalone implementation. The
compiled kernel is embedded; the executable can run outside the source tree.
Like the other ports' images, it includes the Shen Standard Library (`sort`,
`for`, string, maths, tuple and file functions), the release's own
`klambda/stlib.kl`. `scripts/fetch-kernel.sh --check` verifies the vendored
kernel, sources and suite against the pinned release tarball.
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
23 release KLambda files (kernel, standard library and extensions) and the
port boot module, runs the pinned community suite in a fresh writable
directory, and audits both the compiler and generated executable. Passing
requires **154 passed, 0 failed**, all 40 intermediate reports, successful
process exit, matching source hashes, and no Boehm dependencies.
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

Shen: Copyright 2010–2022 Mark Tarver, [BSD license](shen/src/kl/LICENSE.txt).
Shen-C: Copyright 2022 Tatsuya Tsuda and contributors,
[MIT license](LICENSE).

The [43.2 record](docs/kernel-43.2.md) lists the current kernel gates. The [43.1 continuation](docs/qualification/shen-43.1-continuation.md) records the current upgrade, matched comparator, and acceptance gates. Release manifests live in `shen/releases/`; select one with `SHEN_RELEASE_MANIFEST` for certification and source compilation, `--manifest` for benchmarks, `RC_RELEASE_MANIFEST` for Make, or `SHEN_RELEASE_MANIFEST` for CMake. Historical S42 timings remain separate.
