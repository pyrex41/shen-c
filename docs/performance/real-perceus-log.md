# Real Perceus: measurements

Base 8e2327e; after: this branch. Instructions, CPU and peak footprint come from a
release build per commit with `/usr/bin/time -l`, warm private native caches, minimum
of 2 interleaved rounds, every timed row `compiles=0`. Counts come from separate
`RC_STATS_COUNTERS=1` builds, which route every operation through the runtime, so their
instruction counts are not comparable. The host was at load about 20; CPU is noisy and
instructions are the primary metric. Machine: Darwin arm64.

Releases are always retains plus frees, so only retains are listed. Reuse lowers the
number of allocations, so a retains-per-allocation ratio can rise while retains fall:
read the absolute columns too. nrev is the example: 0.01 retains per allocation
before reuse is added, 1.18 after, with the same 3.9M retains.

| workload (native) | R/alloc before | R/alloc after | retains before | retains after | allocs before | allocs after | reuse before | reuse after | ok |
|---|---|---|---|---|---|---|---|---|---|
| startup | 5.99 | 2.76 | 2.2M | 0.9M | 0.4M | 0.3M | 46 | 36,893 | True |
| fib | 86.45 | 2.76 | 32.1M | 0.9M | 0.4M | 0.3M | 54 | 37,457 | True |
| loop | 545.29 | 2.76 | 202.2M | 0.9M | 0.4M | 0.3M | 53 | 37,439 | True |
| nrev | 4.98 | 1.18 | 1.88G | 3.9M | 377.6M | 3.3M | 63 | 374,288,656 | True |
| hof | 5.99 | 1.03 | 122.3M | 20.9M | 20.4M | 20.4M | 63 | 39,195 | True |
| vec | 530.55 | 119.62 | 202.3M | 40.9M | 0.4M | 0.3M | 57 | 39,044 | True |
| str | 5.89 | 0.63 | 29.9M | 3.2M | 5.1M | 5.0M | 55 | 38,457 | True |
| prolog | 4.72 | 3.27 | 1.23G | 851.7M | 260.3M | 260.3M | 90 | 53,688 | True |
| typed | 4.47 | 2.09 | 29.6M | 9.8M | 6.6M | 4.7M | 3,686 | 1,945,553 | True |
| compile | 5.28 | 2.64 | 54.2M | 23.2M | 10.3M | 8.8M | 10,555 | 1,494,208 | True |
| kernel | 3.79 | 2.39 | 178.1M | 101.4M | 47.0M | 42.4M | 16,201 | 4,552,055 | 154/0 |
| extensions | 6.99 | 3.23 | 12.3M | 4.9M | 1.8M | 1.5M | 1,555 | 248,614 | 123/0 |
| steam | 3.73 | 2.54 | 224.5M | 144.4M | 60.2M | 56.8M | 8,115 | 3,391,342 | True |
| yql | 3.93 | 2.51 | 425.1M | 268.6M | 108.3M | 106.9M | 3,893 | 1,340,685 | True |

interpreted loaded code
| kernel | 3.25 | 2.46 | 191.0M | 133.7M | 58.8M | 54.3M | 11,837 | 4,547,702 | 154/0 |
| extensions | 6.99 | 3.28 | 12.4M | 5.0M | 1.8M | 1.5M | 1,515 | 248,298 | 123/0 |
| nrev | 6.98 | 6.97 | 2.63G | 2.63G | 377.6M | 377.6M | 63 | 38,656 | True |
| typed | 4.40 | 2.11 | 29.9M | 10.2M | 6.8M | 4.9M | 3,686 | 1,945,553 | True |

| workload (native, release, warm) | instr before | after | change | CPU s before | after | footprint MB before | after | compiles |
|---|---|---|---|---|---|---|---|---|
| startup | 0.8G | 0.7G | -14.2% | 0.04 | 0.03 | 5 | 5 | 0/0 n=2/2 |
| fib | 22.9G | 18.4G | -19.9% | 1.00 | 0.83 | 5 | 5 | 0/0 n=2/2 |
| loop | 119.3G | 89.9G | -24.6% | 5.49 | 3.96 | 5 | 5 | 0/0 n=2/2 |
| nrev | 398.6G | 276.5G | -30.6% | 21.19 | 15.30 | 5 | 5 | 0/0 n=2/2 |
| hof | 33.7G | 29.5G | -12.5% | 1.60 | 1.31 | 12 | 12 | 0/0 n=2/2 |
| vec | 72.2G | 55.8G | -22.8% | 2.96 | 2.38 | 7 | 7 | 0/0 n=2/2 |
| str | 9.6G | 8.1G | -16.2% | 0.46 | 0.38 | 6 | 6 | 0/0 n=2/2 |
| prolog | 350.5G | 331.2G | -5.5% | 17.83 | 17.39 | 7 | 6 | 0/0 n=2/2 |
| typed | 15.0G | 13.1G | -12.4% | 0.86 | 0.74 | 10 | 10 | 0/0 n=2/2 |
| compile | 29.8G | 26.1G | -12.5% | 1.30 | 1.30 | 18 | 18 | 0/0 n=2/2 |
| kernel | 91.7G | 84.4G | -8.0% | 6.19 | 5.72 | 85 | 82 | 0/0 n=2/2 |
| extensions | 8.9G | 8.2G | -8.1% | 0.84 | 0.73 | 22 | 22 | 0/0 n=2/2 |
| steam | 94.4G | 87.3G | -7.5% | 5.81 | 5.18 | 42 | 38 | 0/0 n=2/2 |
| yql | 143.4G | 133.0G | -7.2% | 7.86 | 7.75 | 28 | 26 | 0/0 n=2/2 |

## What each stage bought (native, retains per allocation, counter build)

| Stage | commit | nrev | hof | str | typed | prolog | kernel suite, interpreted |
|---|---|---|---|---|---|---|---|
| base | 8e2327e | 4.98 | 5.99 | 5.89 | 4.47 | 4.72 | 3.25 |
| primitive and callback operands | 92a726a | 2.99 | 3.99 | 1.92 | 2.72 | 4.48 | 2.73 |
| closure environments | 6c0125a | not run | 3.99 | not run | 2.70 | 3.80 | 2.72 |
| callee bindings | fe88467 | 1.99 | 2.52 | 1.76 | 2.69 | 3.73 | 2.72 |
| fusion, drop specialisation | a1bcb36 | 0.01 | 1.52 | 1.16 | 1.67 | 3.27 | 2.33 |
| reuse | c92816f | 1.19 | 1.52 | 1.17 | 2.47 | 3.27 | 2.55 |
| late moves, single projections | b251e8b | 1.18 | 1.03 | 0.63 | 2.09 | 3.27 | 2.46 |

## Why reuse hit 63 of 376M on nrev

The old analysis took the last bound lexical that was dead after the cons. In
`(cons (hd V1) (app (tl V1) V2))` that is V2, which is already moved into `app` by the
time the cons runs, so the candidate was always NULL. Pairing the cons with the pair it
projects gives 374.3M hits of 376M. Reuse is not rare on functional list code whose input
is unique. It stays rare where input is shared (hof) or allocation is not pairs (prolog).

## What is left

Measured by return-address attribution of retains on the final code, in percent:

| Source | typed | prolog | hof | kernel suite, interpreted |
|---|---|---|---|---|
| hd/tl of a shared pair | 41 | 2 | 51 | 11 |
| frame dup: tail arguments and let aliases | 34 | 46 | 1 | 15 |
| closure capture dup | 5 | 36 | 48 | 7 |
| arg dup for an owning callee (parameter borrowing ceiling) | 14 | 1 | 1 | 5 |
| inside the interpreter | 0 | 0 | 0 | 49 |
