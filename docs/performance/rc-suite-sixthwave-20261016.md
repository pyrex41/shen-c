# Sixth-wave full-suite checkpoint, 2026-10-16 — target NOT MET

Freeze `3b055330971f1bfb83f6d597db9542564ae0f032` combines five bounded changes.
Integrated release and focused runtime qualification now passed; paired
wall/CPU results remain slower and the performance target stays OPEN. Individual microbenchmarks below do not change
that gate. Existing published checkpoint evidence stays intact.

## Integrated changes and tradeoffs

| Change | Preserved semantics / resource bound | Measured controls and limits |
| --- | --- | --- |
| ENV key classes (`574f7d6`) | Immutable inherited OR of uppercase/lowercase/other symbol classes rejects only impossible lookups; positive classes retain exact identity search and all shadows. Context/value validation unchanged. | Uppercase 16-let interpreted loop instructions −20.04%; lowercase loop +3.85%. Same-class lookup control +0.04%. Metadata remains within closure-sized union. |
| Borrow internal evaluator roots (`180c116`) | Public input root retained once; active closure roots body/keys; only local let environments owned locally. Results/captures acquire ownership. Error, redefinition, collection, order and tail guards retained. | Beyond filter, interpreted loop instructions −7.83% uppercase / −5.95% lowercase; combined versus base −26.44% / −2.33%. Retains/releases roughly halved; allocations unchanged. |
| Trial/work storage union (`c7a0cfe`) | Trial subtraction completes before root discovery; discovery consumes only the current trial; child marking waits until all trial reads finish. Exact collector and worklists unchanged. | Object 136→128 requested bytes; Darwin allocator usable class 160→128. Cache bound 1,024 blocks gives 131072 logical bytes. Instructions nearly flat; timing mixed, including slower cached pair churn. Footprint gain, not demonstrated speed gain. |
| Flat interpreted parameter captures (`a151557`) | `[body, environment, keys...]` strong captures validated once; private span guarded by context/native/arity/count. Active closure roots span across redefinition/collection; partial/excess calls retain dispatch semantics. | Repeated calls improve: arity 32 wall 63.021→53.518ms. Creation regresses: arity 16 wall 18.599→24.200ms, instructions 181.714→237.769M (~31% increase). Extra high-arity key captures/temporary array; suite tradeoff unknown. |
| Pinned guarded assoc (`3b05533`) | Exact original AST/parameter match, current selected unbound two-arg closure and canonical `=`, `cons?`, `hd`, `tl` guards. Aliases/redefinitions, malformed/nil input and >4096-prefix paths deoptimize; successful result retained before owned inputs consumed. | Canonical nonempty controls improve 57.5–94.6% CPU. Empty and replaced-dependency controls regress 0.5–10.2%. Profile distribution/full-suite impact unknown. No function binding replaced. |

The trial/work change makes the final object 128 bytes; ENV class metadata fits
inside its union. Do not repeat the ENV lane's earlier 136-byte total as the
integrated final layout. Physical cache counters describe actual requested bytes,
with allocator size classes reported separately. No unbounded retention or
collection disabling is credited.

## Integrated results — correctness PASS, performance NOT MET

Fresh default Release/IPO complete-kernel build passed 20 runtime executables,
13 reader cases, dependency audits and standalone CLI checks. Generated source
contains one guarded assoc helper. Focused fatal ASan/UBSan passed all 20 runtime
executables in both default and true boxed modes (40 runs), with sources unchanged.
This sixth-wave record **does not qualify a full generated-kernel sanitizer run
or default Make build**. Their earlier successes belong to `8dbc9414`, not this freeze.

Four alternating serial release pairs produced eight 134/0 executions, unchanged
source/binary/suite/input. RC lost the first three pairs and won the fourth.
The faster-than-Boehm target remains **NOT MET / OPEN**.

| Metric | RC median | Boehm median | Median paired RC/Boehm ratio |
| --- | ---: | ---: | ---: |
| Wall seconds | 15.105 | 15.155 | 1.058275 |
| User CPU seconds | 14.070 | 13.815 | 1.048450 |
| Retired instructions | 123806130462.5 | 104572653007 | 1.184094 |
| Maximum RSS bytes | 30220288 | 99500032 | 0.304601 |

The paired wall result is 5.83% slower; paired CPU is 4.84% slower. Dividing the
independent wall medians 15.105/15.155 misleadingly suggests a small advantage:
the two medians are selected from different runs, losing their time pairing.
Keep the recorded pair ratios 1.0471,1.1294,1.0695,0.9360 and all four samples;
do not select the favorable fourth pair. Lower RSS and fewer instructions than
earlier RC candidates do not establish the elapsed-time target. Host variation
still prevents cross-campaign elapsed-time comparisons.

Release/profile executable SHA-256:
`aa8ee0ced2225a2b0c04b070e12092baa937879a822d856f957b6ef4fe87a3ce`.
Boehm executable SHA-256:
`31e544aef211caca5c150e685986d98cae0ed2f63eac2f0a70f59de3f4463b8e`.

A separate exact-binary sampled diagnostic passed 134/0 and confirmed unchanged
binary/suite. `evaluate` and `apply_request` remain leaders; native assoc is no
longer among the leading costs. Sampling is diagnostic only and excluded from
all benchmark timing. Selected portable receipts and their original/portable SHA-256 hashes are retained
in [provenance](../evidence/rc-suite-sixthwave-20261016/provenance.json).

## Next candidates — pending

Seventh-wave candidates target atomic argument evaluation, borrowed request
argument spans, and parameter-frame construction. The initial lazy-callee
experiment remains outside the main branch because its measured gains were small
and several controls regressed. No full-suite improvement is claimed for these
candidates. Require a new immutable artifact, focused ownership/cycle checks and
repeated same-input paired measurements. No installation or publication claimed.

## Portable evidence and reproduction

- [Paired release result](../evidence/rc-suite-sixthwave-20261016/perf-sixthwave-balanced/result.json)
- [Default IPO build](../evidence/rc-suite-sixthwave-20261016/sixthwave-default-ipo-build/receipt.json)
- [Focused sanitizer qualification](../evidence/rc-suite-sixthwave-20261016/sixthwave-focused-asan/receipt.json)
- [Exact-binary profile](../evidence/rc-suite-sixthwave-20261016/perf-sixthwave-profile/result.json)
- [Profile interpretation](../evidence/rc-suite-sixthwave-20261016/perf-sixthwave-profile/decisions.md)

Use recorded compiler/SDK/build arguments and source freeze from the build
receipt. Replace portable placeholders with local checkout and artifact paths
in the copied configuration. Run release measurements separately from sanitizer
checks and profiling:

```sh
python3 scripts/benchmark-rc.py --config local-config.json --out new-evidence \
  --suite shen/test/s42 --timeout 400 --output-mib 64
```

Retain four alternating pairs, all suite/input and artifact hashes, and all 134/0
results. Source receipts remain untouched; provenance binds their raw hashes to
sanitized copies. No artifacts were installed or pushed at this checkpoint.
