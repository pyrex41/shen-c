# Initialize reused objects by type

The bounded object cache now resets lifetime metadata and each constructor's
required defaults instead of clearing all 136 bytes. The fresh allocation path
still uses `calloc`. Reused objects retain full ASan poisoning until allocation.

The allocator resets `prev`, `visit_epoch`, `marked`, `reaches_vector` and
`cycle_source`. Number, symbol, closure and tail constructors initialize their
previously implicit defaults. Collector `trial` is assigned for every object
before edge subtraction. Every worklist insertion assigns `work` before reading
it. All other payload fields are assigned before they are read or a callback or
collection can run.

`rc_stats()` derives cached bytes from object count. Cache capacity, peak reserved
bytes, logical allocation/ownership statistics and the public API are unchanged.

## Measurements

Apple arm64, Apple CLT clang, `-O3 -DNDEBUG -std=c17`; baseline commit `7933147`.
Four serial repetitions per variant, alternating variant order, gave these
median elapsed times. Pair churn uses 40 million iterations; mixed churn uses
10 million boxed-number/pair/tail iterations. These are allocator microbenchmarks,
not full Shen suite results.

| Variant | Pair churn | Mixed churn |
| --- | ---: | ---: |
| Baseline | 1.074438 s | 0.771918 s |
| Selective initialization only | 1.068924 s | 0.716455 s |
| Selective initialization plus derived cache bytes | 0.976536 s | 0.667807 s |

The retained combined change reduced median time by 9.1% and 13.5%. An earlier
shorter six-repetition comparison found counter derivation alone regressed pair
churn; that change is retained only with the measured combined implementation.
The pair-only benefit of initialization alone was within noise.

The standalone benchmark is `test/rc/bench_object_init.c`; link it with `src/rc/rc.c`
and `-lm`, then run `pair 40000000` or `mixed 10000000`. Local raw observations are
in `evidence/rc/selective-init/microbench-long.json`.

## Correctness checks

All 12 runtime tests passed with fatal ASan and UBSan, with immediate integers both
enabled and disabled. Both cached-use-after-free negative controls produced
ASan `use-after-poison`. Additional pool regressions cycle boxed integers into
fractional numbers, tails into symbols, partial closures into fresh closures,
and guarded tails into unguarded tails; both configurations passed. Existing
pool bounds, cycle, ownership, boolean-tail and fallback checks also passed.

Local source hashes, binary hashes and logs are under
`evidence/rc/selective-init/runtime/` and `recycled-defaults.json`.
