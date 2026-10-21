# Migration to ShenOSKernel 43.1

Shen-C 0.4.0 embeds Mark Tarver's S43 kernel (`S43.zip`, S-series lineage)
plus the community `stlib.kl`, and reports `43.1`. This branch replaces that
kernel with the community **ShenOSKernel 43.1** release, the kernel used by
Shen/Scheme, Shen/CL, Shen/Go and the other pyrex41 ports.

- Source of record: Shen-Language/shen-sources, tag `shen-43.1`, commit
  `715824a52b55e9c81a9501df8548ff3edd8411f5`.
- Release asset: `ShenOSKernel-43.1.tar.gz`, sha256
  `900eedef44f440af78f9395ed06e689ce1604546fc309f5bc7c8bc84cc651920`.

## Plan

1. **Inventory** every dependency on kernel specifics: kernel input directory
   and build scripts, native overrides and primitive tables, emitter special
   cases, stlib handling, the version string and the test suite.
2. **Kernel input.** Vendor the 23 `klambda/*.kl` files byte for byte into
   `shen/src/kl`, pinned by a new release manifest
   (`shen/releases/shenoskernel-43.1.json`) whose hashes are checked on every
   build. `scripts/fetch-kernel.sh` downloads (or takes) the tarball, verifies
   its sha256 and either refreshes or checks the vendored copy, as Shen/CL's
   script does. The only local kernel-directory file is the port boot module.
3. **Boot.** The community kernel has no top-level forms; `dict.kl` and
   `init.kl` provide `shen.initialise`. A port boot module runs
   `shen.initialise`, the extension initialisers (as Shen/Scheme does) and
   `stlib.initialise`. Drop Tarver's `backend.kl` and `stlib-init.kl`.
4. **Overrides.** Re-audit each native override and emitter fast path against
   the new defun bodies; update or remove those whose kernel definition
   changed or disappeared. Record the result in the override table below.
5. **Tests.** Community 43.1 suite (154 assertions, 40 reports); RC unit
   tests, `scripts/test-rc-toolchain.sh`, Bifrost corpus; the 8 capcov
   workloads byte-identical to go-1f587de; ASan/UBSan lane in an
   ubuntu:24.04 clang-18 container.
6. **Version and docs.** `(version)` comes from the kernel's `init.kl`.
   README and CHANGELOG name the embedded lineage.
7. **Performance sanity.** b-typed and c-untyped CPU, before and after, two
   interleaved rounds; flag any regression over 10%.

## Inventory

Every place that depended on the kernel, and what changed.

| Area | Before (S43) | Now (43.1) |
|---|---|---|
| Kernel input | `shen/src/kl`: 15 Tarver files (with `backend.kl`, no `dict`/`init`), community `stlib.kl`, local `stlib-init.kl` | `shen/src/kl`: the release's 23 `klambda/*.kl` and `LICENSE.txt`, byte-identical; `shen/src/port/shen-c-boot.kl` |
| Pin and fetch | `shen/releases/s43.1.json` (S43.zip hashes) | `shen/releases/shenoskernel-43.1.json` (default); `scripts/fetch-kernel.sh [--check]` verifies the tarball sha256 and the vendored copies |
| Build plumbing | `release.h` `SHEN_KERNEL_MODULES`; `rc-build --emit-kernel <kl-dir>` | `SHEN_KERNEL_FILES` (release modules, then `port_modules`); `rc-build --emit-kernel <source-root>`; `Makefile.rc`, `CMakeLists.txt` |
| Scripts | `certify-rc.sh`, `compile-shen-rc.sh`, `test-rc-toolchain.sh`, `test-native-tier.py`, `measure-native-tier.py`, `benchmark-rc.py` named `s43.1.json` and built kernel paths from `modules` | use the new manifest and `release_manifest.kernel_files()` |
| Boot | S43 files carried top-level initialisation forms; `stlib-init.kl` called `stlib.initialise` | boot module: `shen.initialise`, extension initialisers (features with `shen/c`, expand-dynamic, programmable pattern matching, namespaces), `stlib.initialise` |
| Native overrides | `src/rc/primitives.c` `core` and `overwrites` tables | see the override table |
| Emitter templates | `src/rc/emit.c`: `assoc`, `shen.pvar?`, `shen.lazyderef` fast paths matched the S42/S43 text including variable names, so nothing in 43.1 matched | matched up to renaming of bound variables, against the 43.1 text |
| Numeric hash | `shen.hashkey` wrapper, shape-certified (`exact_hashkey`) | unchanged; the 43.1 `shen.hashkey` body is identical |
| compile-kl capture (`src/rc/main.c`) | standard macro/datatype callbacks found in `shen.*lambdatable*` | 43.1 keeps them as `shen.lambda-form` properties; synonyms are a closure in `shen.*demodulation-function*`, so the `shen.synonyms-h` call is recorded and replayed |
| stlib | community `stlib.kl` on Tarver's kernel, plus local initialiser | the release's own `stlib.kl`, initialised by the boot module |
| Version | Tarver's `declarations.kl` set `*version*` to "43.1" | `init.kl` sets "43.1" in `shen.initialise-environment` |
| Test suite | Tarver S43 suite, 136 assertions, 36 reports, redirect output | community suite `shen/test/43.1`, 154 assertions, 40 reports, no expected output file; `test_release_manifest.py` generalised |
| Not changed | legacy `src/c` (Boehm) and `tools/kernel-aot-build.c`, `tools/yggdrasil-build.c`: not built by any supported entry point. `scripts/build-phase-profile.py` and `scripts/freeze-rc-benchmark.py` compare against a historical Boehm checkout's kernel and only read `modules` | |

## Override change table

The runtime installs `core` primitives before the kernel and `overwrites`
after it, so a kernel defun replaces a `core` entry and an `overwrites` entry
replaces the kernel defun. Bodies compared after normalising variable names.

| Name | Table | Kernel defun S43 / 43.1 | 43.1 body vs S43 | Action | Result vs the 43.1 kernel |
|---|---|---|---|---|---|
| `boolean?`, `integer?` | overwrites | yes / yes | identical | kept | identical (`test/rc/test_kernel_overrides.py`, 41 inputs) |
| `symbol?`, `variable?` | overwrites | yes / yes | identical | **fixed** | identical after the fix. Before: every non-boolean symbol was a `symbol?` and every symbol starting with A-Z a `variable?`; the kernel analyses the printed name, so `(intern "123")`, `(intern "A b")`, `(intern "")` and non-ASCII names differed (pre-existing since S43) |
| `not` | overwrites | yes / yes | identical | **fixed** | identical after the fix: a non-boolean now reports the kernel's `condition is not a boolean`, not `not requires boolean` |
| `pr` | overwrites | yes / yes | identical | kept | **deliberate deviation, unchanged**: `*hush*` silences only standard output, and a string is written as its bytes in one call (the Shen/Scheme fork does the same) |
| `do`, `function`, `shen.gc` | core | yes / yes | identical | kept | the kernel defun replaces the primitive at boot |
| `value/or`, `<-address/or`, `read-file-as-charlist` | overwrites | no / no | — | kept | port extras; no kernel code calls them |
| `shen.char-stinput?`, `shen.char-stoutput?`, `shen.write-string`, `shen.read-unit-string` | overwrites/core | no / no | — | **removed** | 42.x port hooks that 43.1 replaced with the public `char-stream?`, `read-unit-string`, `write-string` (doc/port-upgrades.md) |
| `char-stream?`, `read-unit-string`, `write-string` | core | no / no | — | kept | 43.1 port interface; `char-stream?` is `false` (byte streams), so `pr` and the reader use the byte paths |
| `shen.hashkey` wrapper | installed after the kernel when the defun has the certified shape | yes / yes | identical | kept | **deliberate deviation, unchanged**: an integral float hashes as the integer, so `(put 1 ...)` is found by `(get 1.0 ...)`, consistent with `=`. In 43.1 this governs `shen.dict` bucket placement (all properties) |
| `assoc` fast path | emitter template | yes / yes | identical up to names | template now alpha-equivalent | identical (guarded differential tests) |
| `shen.pvar?` fast path | emitter template | yes / yes | **changed**: 43.1 traps only the `<-address` read and compares the trapped `shen.not-pvar` with `=` | template updated; runtime now requires canonical `=` for every vector | identical; the old runtime returned `false` for an empty vector when `=` was redefined, the kernel calls `=` |
| `shen.lazyderef` fast path | emitter template | yes / yes | identical up to names | template now alpha-equivalent | identical |

## Gates

Final head `e620cba` (rebased on `origin/main` 6a60df8, which adds the
compile-timeout fallback). Final CLI binary sha256 `50d1311f…8efc`.

| Gate | Where | Result |
|---|---|---|
| Community 43.1 suite (`runme.shen`), CLI | macOS, first boot (6f211dc); CI "Compile-first pinned suite (cold and cached)" at e620cba | 154 passed, 0 failed, 40/40 reports, validator PASS; PASS |
| `scripts/certify-rc.sh` (AOT app, full kernel) | macOS e620cba | PASS, 154/0 (`RC_SUITE_TIMEOUT=2400`: the default 600 s timed out at 119/154 at load ~25) |
| `scripts/certify-rc.sh` | CI ubuntu clang and gcc, e620cba | PASS |
| `scripts/test-rc-toolchain.sh --canonical-receipt` | macOS e620cba; CI ubuntu | PASS; PASS |
| `make test` (RC unit tests) | macOS e620cba; CI | PASS; PASS |
| `scripts/test-rc.sh` (emitted semantics, guarded assoc/Prolog, ownership...) | macOS e620cba | PASS |
| Reader capture, source compile, compile cache, compile fallback, kernel overrides | macOS; CI | PASS; PASS |
| CMake/Ninja build and ctest | CI ubuntu | PASS |
| ASan/UBSan lane (`rc-asan-ubsan` steps) | ubuntu:24.04 clang-18 container, 2a5903f; CI e620cba | PASS (certify 154/0 under ASan+UBSan, build 432 s, suite 284 s); PASS |
| Extension tests (`tests/extensions/runme.shen`) | macOS e620cba | 123 passed, 0 failed |
| Bifrost corpus (pinned def3047) | macOS e620cba; CI ubuntu | 72 PASS, 2 KERNEL_SKIP (was 70 PASS, 2 SKIP, 2 KERNEL_SKIP) |
| `(version)` | e620cba | `43.1` |
| `fetch-kernel.sh --check` against the tarball | macOS | all three vendored trees match |
| CI macOS jobs | CI | not run: no macOS runner was assigned (0 steps, cancelled) on this branch and on `main` |
| Historical S42 manifest (`RC_RELEASE_MANIFEST=shen/releases/s42.json`) | macOS e620cba | builds, `(version)` = 42, S42 suite 134/0 |

Bifrost: the two former SKIPs are `source-form-before-macros` and
`namespace-externals-expression`, which print a capability marker when the
port does not boot source-form handlers or the namespaces extension. S43
had neither; 43.1 boots both and both pass. The two KERNEL_SKIPs
(`version-expr-42.2`, `version-banner-contains`) are restricted to kernel
42.2 in the corpus and are unchanged. The pinned corpus's adapter note
still says shen-c "declares its bundled S43 kernel as 43.1"; that wording
is a follow-up for the bifrost repo.

### Capcov workloads (byte-identical payloads against go-1f587de)

Final binary, private cold caches, `SHEN_RC_COMPILE_TIMEOUT=1800`:

| Workload | Native | Interpret (`SHEN_RC_EXECUTION=interpret`) |
|---|---|---|
| a0-bare, a1-libs-untyped, a2-libs-typed | identical | — |
| c-untyped | identical | identical (perf runs) |
| d-g5 (25 drivers) | identical | identical |
| e-draft, f-modelcheck | identical | — |
| b-typed | not completed: cold native compilation of its ~11 MB generated modules ran past 90 minutes (the 72db8d4 run timed out at 5400 s) | identical |

The same seven native workloads were also identical on 72db8d4 (pre-rebase).
An earlier run on the shared `~/.cache/shen-c` failed three d-g5 drivers
with compile status 124 (timeout while other agents held the cache lock);
those runs are superseded, and on main the fallback now interprets such a
form instead.

### Performance sanity

Base `8c94154` (S43) against `72db8d4` (the migration without main's later
fallback change), two interleaved rounds, `/usr/bin/time -l` on the binary,
user CPU seconds. Host load average 18–27 throughout (shared host).

| Workload | Mode | Base r1 / r2 | 43.1 r1 / r2 | Change (mean) |
|---|---|---|---|---|
| b-typed | interpret | 207.36 / 188.21 | 202.30 / 183.17 | −2.6% |
| c-untyped | interpret | 51.38 / 40.14 | 50.73 / 41.66 | +0.9% |
| c-untyped | native, warm cache | 49.10 / 42.64 | 43.65 / 45.97 | −2.4% |

No CPU regression. Peak RSS of b-typed rose from 1.166/1.170 GB to
1.266/1.293 GB (+8.6% / +10.5%), probably the 20000-bucket property
dictionary and the extra kernel state; c-untyped RSS is unchanged
(83–85 MB). Native c-untyped wall time (309–373 s against 43–50 s CPU) is
scheduler contention on the shared host, for both binaries. Startup
(`eval -e`) user CPU is 0.06–0.09 s for both.

## Progress log

- 2026-10-21: branch `feat/shenoskernel-43.1` from `origin/main` 8c94154;
  plan written.
- 2026-10-21: vendored 43.1 (f9c6d47); alpha-equivalent templates and
  lambda-form lookup (6f211dc). Suite 154/0 on first boot. Draft PR #21.
- 2026-10-21: synonyms replay for compile-kl and fast-path test controls
  (b4005cd); `shen.pvar?` empty-vector fix (37bc164); kernel-exact
  `symbol?`/`variable?`/`not`, removal of 42.x stream hooks, override
  differential test (72db8d4); sanitizer timeouts (2a5903f); README and
  CHANGELOG (29b6b32).
- 2026-10-21: rebased onto main 6a60df8 (compile-timeout fallback); the
  commits above are now e4b311f, 78ab61e, 8399944, 3c62c90, 401d239,
  52af91d, a617256.
- Capcov runs under the shared `~/.cache/shen-c` stalled on its global lock
  (other agents' compiles) and hit the 120 s compile timeout in d-g5. Final
  runs use private `SHEN_RC_NATIVE_CACHE` directories and
  `SHEN_RC_COMPILE_TIMEOUT=1800`.
