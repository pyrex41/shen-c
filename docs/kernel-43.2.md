# Kernel: ShenOSKernel 43.2

The default kernel moves from upstream `ec16a9f` (release 43.1 plus the
type-checker fix, `(version)` `43.1`) to the official
Shen-Language/shen-sources release **shen-43.2** (tag object `7b7563e5`,
commit `e6947f507ebc`). `(version)` is `"43.2"`.

- Asset `ShenOSKernel-43.2.tar.gz`, sha256
  `23c71508f0de24785e1f127ea4fab440081904d81c463090bc642ee840417f32`; its
  `VERSION` file says `43.2`.
- `scripts/fetch-kernel.sh --check` verifies the vendored copy;
  `--version 43.1-ec16a9f` and `--version 43.1` check the older copies.

## Layout

| Kernel | Manifest | KLambda | Sources | Suite |
|---|---|---|---|---|
| 43.2 (historical; default until 43.4) | `shen/releases/shenoskernel-43.2.json` | `shen/releases/43.2/kl` | `shen/releases/43.2/shen` | `shen/test/43.2` |
| upstream ec16a9f | `shen/releases/shenoskernel-43.1-ec16a9f.json` | `shen/releases/43.1-ec16a9f/kl` | `shen/releases/43.1-ec16a9f/shen` | `shen/test/43.1` |
| official 43.1 | `shen/releases/shenoskernel-43.1.json` | `shen/releases/43.1/kl` | not vendored | `shen/test/43.1` |

The 43.2 suite differs from 43.1's by one added file,
`benchmark-coverage.shen`, which `kerneltests.shen` does not run; the
canonical suite is still 154 assertions in 40 reports, so it has its own
directory. All kernels use the port boot module
`shen/src/port/shen-c-boot.kl`. To build an older kernel:
`make RC_RELEASE_MANIFEST=shen/releases/shenoskernel-43.1-ec16a9f.json`.

## What changes in the kernel

Against ec16a9f, `init.kl` (version), `declarations.shen` (version) and
`extension-type-annotations.kl` differ; the other klambda files match
byte for byte. The `shen.by-hypothesis` / `shen.search-user-datatypes` guards
and their `-h` helpers are already in ec16a9f, so shen-c's templates
(`assoc`, `shen.pvar?`, `shen.lazyderef`, numeric `shen.hashkey`) and their
tests are unaffected. Nothing in `src/` overrides those two functions.
The generated kernel is rebuilt from the vendored KLambda by
`bin/rc-build --emit-kernel` (`obj/rc/kernel.c`); nothing generated is
checked in.

## Gates (macOS arm64, Apple clang 21, base 8e2327e)

| Gate | Result |
|---|---|
| `fetch-kernel.sh --check` (43.2 tarball) | every vendored tree matches |
| `test/rc/test_release_manifest.py` (s42, 43.1-ec16a9f, 43.1, 43.2) | PASS |
| `make certify` (canonical suite, AOT app) | 154 passed, 0 failed, 40/40 reports |
| `scripts/test-rc-toolchain.sh --canonical-receipt` | PASS |
| `make test`; `test_kernel_overrides.py`; `test_guarded_assoc.sh`; `test_prolog_fast.sh`; `test_reader_capture.sh` | PASS |
| `bin/shen-c eval -e '(version)'` | `43.2` |

Binary size: base 8e2327e `bin/shen-c` 9,975,128 bytes; 43.2 9,974,424
bytes (-704). Generated `kernel.c`: 14,393,179 to 14,392,496 bytes (-683).

The pinned Bifrost corpus still expects `43.1` in `version-expr-43.1`, so
`.github/workflows/bifrost.yml` allows that one case to fail with `43.2`
until the corpus learns the 43.2 kernel.
