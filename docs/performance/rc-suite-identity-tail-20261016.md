# Identity-tail full-suite checkpoint, 2026-10-16

**Performance target NOT MET / OPEN.** Source freeze
`cf8a36fb8a0dc3be72e44e3d6f7598c443d61a33` passed all eight unchanged S42
executions (134 passed, 0 failed each) in four alternating serial pairs against
the preserved Boehm CLI. The candidate is now included in `feat/perceus-rc`.

| Metric | RC median | Boehm median | Median paired RC/Boehm ratio |
| --- | ---: | ---: | ---: |
| Wall seconds | 19.230 | 18.185 | 1.008508 |
| User CPU seconds | 17.555 | 16.630 | 1.041059 |
| Retired instructions | 121197417847.5 | 107705160595 | 1.125760 |
| Maximum RSS bytes | 30310400 | 81379328 | 0.371238 |

RC was 0.9% slower in paired wall time and 4.1% slower in paired user CPU;
paired peak memory was 62.9% lower. Paired ratios are medians of the four matched
ratios, not ratios of independent medians. All samples are retained. RC won two
wall-time pairs and one CPU pair. Unrelated verification processes and desktop
services contended for CPU; process snapshots remain in local evidence. This
batch establishes neither parity nor a speedup. Do not compare its raw seconds
with earlier campaigns or attribute changes between campaigns to this patch.
Retired instructions remained about 12.6% above the preserved Boehm executable.
The original Boehm build flags are unknown; these compare whole implementations.

## Qualification and identities

Fresh default Release ThinLTO full-kernel build passed, including 22/22 CTest
runtime checks, no-Boehm artifact audits, reader capture and three empty-directory
checks. Generated source contains the expected compiled identity certificate.
The candidate lane separately passed 44 fatal ASan/UBSan runtime executions
(default/boxed) and 56 emitted comparisons. This is not a full generated-kernel
sanitizer qualification; the last such suite belongs to fifth-wave `8dbc9414`.
Fresh default Make, Linux/Nix and CI qualification are not established here.

- RC CLI SHA-256: `757374bbd86c5aa334f7f353d6f671420d4ca1b2ac94ea26eb8f34574b17d009`.
- Boehm CLI SHA-256: `31e544aef211caca5c150e685986d98cae0ed2f63eac2f0a70f59de3f4463b8e`.
- Raw result SHA-256: `5f73ced4c6a92ffd22206848b35bf8d565fb2c880e4053c0a7118fd1a0301e60`.
- Frozen build receipt SHA-256: `86955123f0dde1929d991e8a09ecb1c6606e18a0a4292d8cffe95d17e5a7710d`.

Source, artifacts, dependencies, stdin, harness and the frozen build receipt
remained unchanged during timing. No collector or statistics were disabled.
The [implementation record](evaluator-identity-tail.md) describes context guards,
last-use destruction timing, microbenchmark gains and control regressions.
The separate self-loop experiment was not integrated because it retained input
lists and raised peak RSS 87.7% in its list control.

## Evidence and reproduction

[Portable provenance](../evidence/rc-identity-tail-20261016/provenance.json),
[summary](../evidence/rc-identity-tail-20261016/identity-tail-summary.json),
[paired result](../evidence/rc-identity-tail-20261016/perf-identity-tail-balanced/result.json),
[build receipt](../evidence/rc-identity-tail-20261016/identity-tail-default-ipo-build/receipt.json)
and [frozen benchmark receipt](../evidence/rc-identity-tail-20261016/identity-tail-benchmark-frozen.json)
preserve raw and portable hashes. Paths in publication copies are placeholders;
raw logs and process snapshots remain in the candidate worktree's `evidence/rc/`.

Reproduce the recorded Apple Clang/SDK default Release ThinLTO build, freeze its
receipts before timing, and use the recorded configuration with
`scripts/benchmark-rc.py --config local-config.json --out new-evidence
--suite shen/test/s42 --timeout 400 --output-mib 64`. Run four alternating serial
pairs with the same stdin and unchanged suite. Set Boehm's library path inside
`/usr/bin/time -l /usr/bin/env DYLD_LIBRARY_PATH=...` on macOS. Preserve all runs
and host-contention notes. Earlier [seventh-wave evidence](rc-suite-seventhwave-20261016.md)
remains historical. No installed binaries were replaced for this publication.
