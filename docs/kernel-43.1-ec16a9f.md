# Kernel: upstream ec16a9f

The default kernel moves from the official ShenOSKernel 43.1 release (#21)
to an **unmodified build of Shen-Language/shen-sources commit ec16a9f**.
That commit is tizoc's merge of
[#117](https://github.com/Shen-Language/shen-sources/pull/117), the
type-checker fix, merged unchanged. Upstream has not cut a release
containing it and does not commit `klambda/`, so the rendered build is
published as pyrex41/shen-sources release `upstream-ec16a9f`. The release
tag points at upstream commit `ec16a9fa4fc2e87e2aa336fb1ae30e8ac2266394`
itself, and no tracked file is edited. `(version)` is `"43.1"`, as on
upstream master.

- `ShenOSKernel-43.1-ec16a9f.tar.gz` has sha256
  `060066a8f84d9a2f7d41cb3b37b986a8d4653279752b739b5063cd648d9baacd`; its
  `VERSION` file says `43.1-ec16a9f`.
- `scripts/fetch-kernel.sh --check` verifies the vendored copy;
  `--version 43.1` checks the official 43.1 copy.
- This replaces the earlier fork label 43.1.2. The kernel code is identical:
  every `.kl` file matches 43.1.2 byte for byte except `init.kl`, which
  differs only in the version string.

## Layout

| Kernel | Manifest | KLambda | Sources | Suite |
|---|---|---|---|---|
| upstream ec16a9f (default) | `shen/releases/shenoskernel-43.1-ec16a9f.json` | `shen/src/kl` | `shen/src/shen` | `shen/test/43.1` |
| official 43.1 | `shen/releases/shenoskernel-43.1.json` | `shen/releases/43.1/kl` | not vendored | `shen/test/43.1` |

Both kernels ship byte-identical `tests/`, so they share one suite
directory: 154 assertions in 40 reports. Both use the same port boot
module, `shen/src/port/shen-c-boot.kl`. Both manifests have
`kernel_version` `43.1`, because that is what `(version)` reports; the
default manifest also records `kernel_build: upstream-ec16a9f`. To build
official 43.1, use
`make RC_RELEASE_MANIFEST=shen/releases/shenoskernel-43.1.json`.

## What changes in the kernel

Compared with official 43.1, 13 KLambda files differ in bytes. After
normalising generated variable numbers, only two defuns in `t-star.kl`
change, and two are added:

- `shen.by-hypothesis` and `shen.search-user-datatypes` now return `false`
  at once for an empty hypothesis or datatype list.
- Otherwise they deref the subject once and delegate to the new
  `shen.by-hypothesis-h` and `shen.search-user-datatypes-h`. In 43.1 the
  subject was dereferenced again for every hypothesis.

## Templates and fast paths

shen-c's kernel-shaped optimisations are the `assoc`, `shen.pvar?` and
`shen.lazyderef` templates and the numeric `shen.hashkey` wrapper. None of
those definitions changes in ec16a9f. The generated kernel still installs
each fast path once, and `test/rc/test_kernel_overrides.py`,
`test_prolog_fast.sh` and `test_guarded_assoc.sh` pass.

The new `-h` predicates are ordinary Prolog-compiled defuns. They match no
template, which is correct: no template describes them. They still use the
fast paths at runtime, because they call `shen.lazyderef`, whose native
definition carries the guarded fast path. To check the emitter, each old and
new predicate was emitted on its own:

| Definition | Self tail call compiled to a loop | Tail calls |
|---|---|---|
| 43.1 `shen.by-hypothesis` | yes | 3 |
| ec16a9f `shen.by-hypothesis-h` | yes | 3 |
| 43.1 `shen.search-user-datatypes` | yes | 1 |
| ec16a9f `shen.search-user-datatypes-h` | yes | 1 |
| ec16a9f `shen.by-hypothesis` (wrapper) | n/a; tail-calls the `-h` predicate | 1 |

So the recursion over long hypothesis lists still runs in constant C stack.

## Gates

Code at 9b0129f; later commits change only CI and docs. The binary's sha256
is `d0fb92bd…e501`.

| Gate | Where | Result |
|---|---|---|
| `fetch-kernel.sh --check` (43.1.2, and `--version 43.1`) | macOS | every vendored tree matches its tarball |
| `test_release_manifest.py` (validates every manifest: s42, 43.1, 43.1.2) | macOS, CI | PASS |
| Community suite, CLI | macOS | 154 passed, 0 failed, 40/40 reports, validator PASS |
| `certify-rc.sh` (AOT app) | macOS (`RC_SUITE_TIMEOUT=2400`); CI ubuntu clang/gcc and macOS | 154/0 |
| `test-rc-toolchain.sh --canonical-receipt` | macOS; CI | PASS |
| `make test`, `scripts/test-rc.sh` (including the guarded assoc and Prolog fast-path tests) | macOS; CI | PASS |
| Reader capture, source compile, compile cache, compile fallback | macOS; CI | PASS |
| `test/rc/test_kernel_overrides.py` | macOS; CI | 5 defuns x 43 inputs identical to the 43.1.2 kernel |
| ASan/UBSan lane | ubuntu:24.04 clang-18 container at 9b0129f; CI `rc-asan-ubsan` | PASS; certify 154/0 under ASan+UBSan |
| Extension tests (`tests/extensions/runme.shen`) | macOS | 123/0 |
| Bifrost (pinned def3047) | macOS; CI ubuntu and macOS | 71 PASS, 1 FAIL, 2 KERNEL_SKIP. The one FAIL is `version-expr-43.1`, whose golden is "43.1" while 43.1.2 reports "43.1.2". `bifrost.yml` asserts that this is the only failure and that it has exactly this cause. Updating the corpus is a follow-up for the bifrost repo |
| `(version)` | macOS | `43.1.2` |

CI at db9703c is green on every job, including rc-macos.

### Capcov workloads (byte-identical payloads against go-1f587de)

Run on private cold caches with `SHEN_RC_COMPILE_TIMEOUT=1800`:

| Workload | Native | Interpret |
|---|---|---|
| a0-bare, a1-libs-untyped, a2-libs-typed, e-draft, f-modelcheck | identical | — |
| c-untyped | identical | identical |
| d-g5 (25 drivers) | identical | identical |
| b-typed | not run: cold native compilation is impractical (known limitation from #21) | identical |

Every perf run below also produced the reference payload.

## Performance

43.1 is main `03bc00b` (binary `91479c44…6c1a`); 43.1.2 is this branch.
Both builds are identical except for the kernel. The measurements are:

- `/usr/bin/time -l` run directly on the binary, recording instructions
  retired, which are insensitive to load;
- three interleaved rounds, with round 1 a discarded warm-up;
- native runs from warm private caches seeded from the same compiled
  modules;
- d-g5 summed over its 25 drivers;
- `mb-defs` and `mb-terms`, the empty-case microbenchmarks from
  `scratchpad/guard117/mb` (`mb-defs` typechecks `defs.shen` 400 times;
  `mb-terms` calls `shen.typecheck` 120000 times).

User CPU is shown for reference; it varies with load.

| Workload | Mode | 43.1 instructions (r2 / r3) | 43.1.2 instructions (r2 / r3) | Change | 43.1 user s | 43.1.2 user s |
|---|---|---|---|---|---|---|
| b-typed | interpret | 3577.2G / 3577.4G | 1110.8G / 1110.6G | -69.0% | 150.1 / 145.8 | 47.0 / 46.8 |
| c-untyped | interpret | 1000.8G / 1001.1G | 1000.9G / 1000.7G | -0.0% | 38.7 / 40.5 | 37.7 / 36.8 |
| c-untyped | native | 958.7G / 957.5G | 958.4G / 957.8G | +0.0% | 41.5 / 41.4 | 40.8 / 40.8 |
| d-g5 | interpret | 600.8G / 600.8G | 598.4G / 598.3G | -0.4% | 29.5 / 28.0 | 28.7 / 24.0 |
| d-g5 | native | 750.1G / 748.8G | 747.2G / 746.6G | -0.3% | 33.4 / 28.1 | 30.1 / 28.4 |
| mb-defs | interpret | 374.5G / 374.4G | 373.2G / 373.3G | -0.3% | 15.4 / 15.0 | 15.2 / 17.1 |
| mb-defs | native | 410.0G / 409.9G | 408.6G / 408.6G | -0.3% | 17.7 / 19.1 | 17.5 / 16.6 |
| mb-terms | interpret | 1301.1G / 1301.0G | 1275.6G / 1275.2G | -2.0% | 48.7 / 46.9 | 51.1 / 43.5 |
| mb-terms | native | 1301.1G / 1301.1G | 1275.9G / 1275.5G | -2.0% | 48.8 / 50.1 | 49.8 / 54.4 |

b-typed, whose typechecking is dominated by the hypothesis search that
#117 fixes, retires 69% fewer instructions. The empty-case microbenchmark
improves by 2.0%. c-untyped is unchanged, as expected for untyped code.
No workload regresses. b-typed's peak RSS is the same within noise (1.58 GB
against 1.59 GB).
