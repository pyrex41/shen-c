# Kernel: ShenOSKernel 43.4

The default kernel moves from the official 43.2 release to Shen-Language/shen-sources
release **shen-43.4** (tag object `40fd3358`, commit `f34b307b490880eedcfe1f5e5d3cc24ca3d2602e`).
`(version)` is `"43.4"`. Asset `ShenOSKernel-43.4.tar.gz` has sha256
`15e7972e6bf0cd65d37c2eba85750b8839d2dbadda41027eae0c88e5017a2756`.

- Layout as for 43.2: `shen/src/kl`, `shen/src/shen`, `shen/test/43.4` hold 43.4;
  43.2 moved to `shen/releases/43.2/{kl,shen}` (suite stays in `shen/test/43.2`) and is
  selectable with `RC_RELEASE_MANIFEST=shen/releases/shenoskernel-43.2.json`.
- `scripts/fetch-kernel.sh --version 43.4|43.2|43.1-ec16a9f|43.1 --check` verifies each.
- The kernel and `stlib.kl` are regenerated together by `make` (`rc-build --emit-kernel`);
  nothing generated is checked in. No new primitive was needed.
- The suite is still 154 assertions in 40 reports; 43.4 adds `regressions/` and
  `extensions/expand-dynamic/` files, which the manifest's suite hashes cover.
- No port code wraps or records `declare`, so the 43.3 move of stdlib signatures to
  `shen.declare-with-signature` needs no change; `sig-check` passes.
- Native overrides (`assoc`, `shen.pvar?`, `shen.lazyderef`, `shen.hashkey`, `symbol?`,
  `variable?`) are matched up to bound-variable renaming; `test_kernel_overrides.py`
  passes against 43.4.

Gates (macOS arm64): certify 154 passed / 0 failed (needed `RC_SUITE_TIMEOUT=1500`
under heavy host load; the first run was killed at the default 600 s); `make test`,
guarded assoc, guarded Prolog, reader capture, `test_kernel_overrides.py`,
`test_release_manifest.py`: PASS. Pinned Bifrost corpus: the 43.2 version cases fail with
`43.4`, as does nothing else beyond the known divergences.
