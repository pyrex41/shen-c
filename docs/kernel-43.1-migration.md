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

(filled in as the work proceeds)

## Override change table

(filled in as the work proceeds)

## Gates

(filled in as the work proceeds)

## Progress log

- 2026-10-21: branch `feat/shenoskernel-43.1` from `origin/main` 8c94154;
  plan written.
