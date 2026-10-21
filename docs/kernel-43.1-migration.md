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
| `symbol?`, `boolean?`, `integer?`, `variable?`, `not` | overwrites | yes / yes | identical | kept | differential check below |
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

(filled in as the work proceeds)

## Progress log

- 2026-10-21: branch `feat/shenoskernel-43.1` from `origin/main` 8c94154;
  plan written.
