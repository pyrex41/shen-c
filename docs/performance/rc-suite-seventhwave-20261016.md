# Seventh-wave full-suite checkpoint, 2026-10-16

**Performance target NOT MET / OPEN.** Freeze
`fd1fd1e2ea2c6d311137e022aa8bfa67bd50c09f` passed all eight unchanged S42
executions with 134 passed/0 failed, but RC lost every one of four alternating
release pairs. This preserves and supersedes the sixth-wave checkpoint;
historical receipts remain intact.

| Metric | RC median | Boehm median | Median paired RC/Boehm ratio |
| --- | ---: | ---: | ---: |
| Wall seconds | 13.525 | 12.385 | 1.074775 |
| User CPU seconds | 13.140 | 12.085 | 1.070250 |
| Retired instructions | 120893032471.5 | 107683800776 | 1.122370 |
| Maximum RSS bytes | 30990336 | 82067456 | 0.377506 |

RC remained 7.5% slower wall and 7.0% slower user CPU; paired maximum RSS was 62.2%
lower. Pair ratios are medians of the matched ratios, not ratios of independent
medians. All samples are retained. Host variation still prevents comparing
candidate timings to Boehm timings from a different campaign. No speed win is
claimed from lower RSS or instructions. Sampling is excluded from release timing.

## Correctness scope

Fresh default Release/IPO full-kernel build passed its recorded checks. Focused
fatal ASan/UBSan passed 21 runtime executables in both default and true boxed
modes (42 runs), with source hashes unchanged. This is **not** a full generated
kernel sanitizer qualification. The latest full generated-kernel sanitizer 134/0
remains fifth-wave `8dbc9414`; no seventh-wave Make qualification is claimed.

Release CLI SHA-256:
`a0dad0bfd98533cb94824445aa7b7af0f4d525955092658630564667a2652bd4`.
Boehm CLI SHA-256:
`31e544aef211caca5c150e685986d98cae0ed2f63eac2f0a70f59de3f4463b8e`.

## Receipt mutation boundary

The build receipt changed during timing from SHA-256
`9b4105b5970b2c139d16559d70cb5ffa710bff97a57baec613ab82a71cf50383` to
`467c58eaa27be0a61e371a30419596c4359d0af423af910f3a3813fdac72515a`.
Independent review confirmed all 77 source/current bindings, four actual build
artifacts, each timed CLI identity and all recorded stdout/stderr/stage hashes
match. This supports immutable source/executable evidence, but receipt bytes
were **not immutable**. The build owner reports post-PASS metadata finalization added IPO verification,
assoc occurrence count and the compile-commands hash. About 0.2 seconds of metadata
reads/hashes/copies overlapped timing; the owner attests all build/test subprocesses
completed before GO. Initial receipt bytes were not retained for independent byte
diff. The benchmark remains usable with independently verified source/artifacts,
but neither an immutable build receipt nor a completely activity-free interval
is claimed.

## Evidence and reproduction

[Portable provenance](../evidence/rc-suite-seventhwave-20261016/provenance.json),
[summary](../evidence/rc-suite-seventhwave-20261016/summary.json),
[paired result](../evidence/rc-suite-seventhwave-20261016/perf-seventhwave-balanced/result.json)
and [independent audit](../evidence/rc-suite-seventhwave-20261016/independent-audit.json)
preserve hashes and the mutation caveat. Source evidence remains in `evidence/rc/perf-seventhwave-balanced/`,
`evidence/rc/seventhwave-default-ipo-build/` and
`evidence/rc/seventhwave-focused-asan/`. Preserve source/input/binary hashes,
four alternating pairs and all 134/0 results. Reproduce with recorded compiler,
SDK, Release IPO arguments and `scripts/benchmark-rc.py --config local-config.json
--out new-evidence --suite shen/test/s42 --timeout 400 --output-mib 64`.
No artifacts were installed or pushed at this checkpoint.
