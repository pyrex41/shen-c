# Tight Perceus RC: progress log

Branch `perf/tight-perceus`, from main 8c94154 with PR #12
(`perf/issue-7-ownership-overhead`, 0b086f9) merged as d3d0460.

Question: can precise reference counting with reuse (Perceus) make shen-c
competitive with shen-scheme (Chez, generational GC)? Native `deref`/`hash`
overrides are deliberately absent, so only memory management changes.

## Merge of #12

Conflicts in `CHANGELOG.md` and `src/rc/emit.c`. The merge keeps main's
compile-first emitter and adds #12's run-once immortal definitions
(`rc_closure_static`). Dynamic expression modules (`expression_entry`) never
take that path: their definitions capture the literal vector, and cached code
may be run again.

## Measurement method

- Harness: `tools/perceus-bench/run.py`. It runs `/usr/bin/time -l BIN script
  driver.shen` with cwd set to the driver directory, which is how bench_c2.py
  runs it, and kills the run after 900 s. It records instructions retired,
  user+sys CPU, peak RSS and the 1-minute load average. It compares payload()
  (copied from bench_c2.py) with the go-1f587de `time-c2` reference.
- Main compiles loaded code to C at load time (PR #14). Each binary gets its own
  `SHEN_RC_NATIVE_CACHE`, keyed by the binary's sha256. A warm-up pass, which is
  not timed, fills that cache. Every timed row has `compiles=0`, so clang time is
  excluded. In-process emission (`emit_s`) and dlopen (`load_s`) are still
  counted, and the RC_NATIVE line records them.
- Cold compile cost on main 8c94154: the c-untyped warm-up (256 compiles) took
  about 10 minutes wall at load ~40.

## Stage 0: instrumentation (counter builds only)

`SHEN_RC_STATS_COUNTERS=1` builds report one `RC_PERCEUS {...}` JSON line at
context teardown when `SHEN_RC_PERCEUS_STATS` is set. It contains:
- allocations, root deaths and cascade deaths for each type;
- releases that take a count to zero;
- pair allocations made after a pair free with no pair allocation in between
  (the reuse-token ceiling);
- for full collections: objects scanned, live cycle sources, and how many
  vector-reaching objects can be reached from those sources (the work a
  source-rooted collector would do).

### Stage 0 results (c-untyped; warm pass, counter build of d3d0460)

| Metric | Value |
|---|---|
| allocations | 730M: strings 268M (kernel `hash`/`explode`), pairs 269M, RC_TAIL 188M, closures 4.8M |
| pair reuse hits / attempts | 222K / 267M (0.08%) |
| pair allocations after a pair free (reuse-token ceiling) | 139M / 269M (52%) |
| pair deaths: owner's drop / cascade | 263.8M / 5.3M |
| retains / releases / releases to zero | 2.08G / 2.81G / 730M (26%) |
| full collections / objects scanned / freed | 206 / 62.9M / 0 |
| objects reachable from live sources | 94K (0.15% of scanned) |

So the free list already recycles about half of all pair memory, and in-place
reuse almost never applies, because the candidates are shared. The headroom
lies in ownership traffic (about 2.9 retains and 3.9 releases per
allocation), tail requests (26% of allocations) and the full-heap collector.
### Stage 0 results (b-typed; counter build of d3d0460, loaded code interpreted)

The native-mode counter run spent more than 8 h in cold compiles and was
stopped. This run used SHEN_RC_EXECUTION=interpret, so b-typed's loaded code
was interpreted; the kernel is native in both modes. The 45M env frames come
from the interpreter.

| Metric | Value |
|---|---|
| allocations | 1.81G: pairs 1.45G (80%), RC_TAIL 191M, strings 101M, env 45M, closures 22M |
| pair reuse hits / attempts | 876K / 1.44G (0.06%) |
| pair allocations after a pair free | 95M / 1.45G (6.6%) |
| pair deaths: owner's drop / cascade | 141M / 1.31G |
| retains / releases / releases to zero | 7.21G / 9.02G / 1.81G (20%) |
| full collections / objects scanned / freed | 188 / 148M / 0 |
| objects reachable from live sources | 303K (0.2% of scanned) |

On b-typed, pairs die mostly in cascades, when a whole ed term is
dropped. In-place reuse has almost nothing to work with: 0.06% today and a
6.6% ceiling.

## Order of the stages

The global list is what the full-heap collector, the epoch reset and teardown
walk, so it can only go after the collector stops walking the heap. Stage 4
(the collector) therefore landed first, then stages 1 and 2 together (slab
cells), then stage 5 (musttail), then stage 3.

## Commits and gates

Gates:
- `tools/perceus-bench/gates-mac.sh`: build, `make test`, `certify-rc.sh` (the
  136-test suite), `test-rc-toolchain.sh` with that receipt, and the Bifrost
  corpus at def3047.
- `gates-asan-linux.sh`: `make sanitize` and the ASan/UBSan certify with
  VERIFY_OWNERSHIP, run in an ubuntu:24.04 clang-18 container. ASan hangs at
  init on this Darwin 27 host, even for a trivial program, with both Apple and
  nix clang.
- Timeouts are raised (RC_SUITE_TIMEOUT and RC_BUILD_TIMEOUT 7200, and 14400
  under ASan), because the host load reaches 60 or more.

| Commit | What | make test | certify | toolchain | Bifrost | ASan certify |
|---|---|---|---|---|---|---|
| d3d0460 | main + #12 | PASS | 136/136 | PASS | 74 rows: 70 PASS, 2 capability skips, 2 kernel skips | 136/136 |
| 1ad223f | source-rooted collector | PASS | covered by 0977e46 | | | |
| 0977e46 | slab cells, 32 B pairs | PASS | 136/136 | PASS | 70 PASS, 2 capability skips, 2 kernel skips | 136/136 |
| 2fe55c6 | musttail | PASS | 136/136 | PASS | 70 PASS, 2 capability skips, 2 kernel skips | 136/136 |
| d53c20b | inline fast paths (stage 3) | PASS | 136/136 | PASS | 70 PASS, 2 capability skips, 2 kernel skips | 136/136 |

## Cold compile cost on main (compile-first, PR #14)

Cold b-typed on main 8c94154 took 6432 s wall and 4140 s user. compile_s was
4469, it emitted 105 MB of C, and peak RSS was 3.3 GB (load ~30–60). With the
default 120 s SHEN_RC_COMPILE_TIMEOUT, the warm-up runs of b-typed and d-g5
under that load produced wrong payloads. All warm-ups therefore use
SHEN_RC_COMPILE_TIMEOUT=1200, and the timed rows assert compiles=0.

## Timing: warm native cache (load-time compile on, compiles=0 asserted)

Run 2026-10-20/21. Each row is the mean of 2 interleaved rounds; CPU is
user+sys. The host was shared, so instructions retired is the stable metric.
Wall time is not reported: module loading blocks for minutes in `open()` on
this host (load_s up to about 300 s), which inflates every native row equally.
The builds are:
- `main`: 8c94154;
- `pr12`: d3d0460;
- `s12`: 0977e46 (source-rooted collector plus slab cells);
- `s5`: 2fe55c6 (plus musttail);
- `s3`: d53c20b (plus inline fast paths);
- `scheme`: shen-scheme d037ae6, on the same host in the same rounds.

| workload | build | mode | instr (G) | CPU s | peak RSS MB | payload ok | load |
|---|---|---|---|---|---|---|---|
| b-typed | main | native (warm cache) | 3635.7 | 154.8 | 1677 | yes (2 runs) | 44.4/2.4 |
| b-typed | pr12 | native (warm cache) | 3031.4 | 141.1 | 1464 | yes (2 runs) | 59.5/38.1 |
| b-typed | s12 | native (warm cache) | 3178.3 | 134.4 | 566 | yes (2 runs) | 18.5/30.4 |
| b-typed | s5 | native (warm cache) | 3129.8 | 133.6 | 567 | yes (2 runs) | 17.0/30.0 |
| b-typed | s3 | native (warm cache) | 3012.1 | 126.0 | 504 | yes (2 runs) | 11.3/28.1 |
| b-typed | scheme | - | 133.2 | 6.4 | 396 | yes (2 runs) | 7.1/26.5 |
| c-untyped | main | native (warm cache) | 943.4 | 38.9 | 97 | yes (2 runs) | 49.8/9.1 |
| c-untyped | pr12 | native (warm cache) | 866.2 | 36.5 | 93 | yes (2 runs) | 34.0/12.6 |
| c-untyped | s12 | native (warm cache) | 879.2 | 32.7 | 71 | yes (2 runs) | 34.7/5.3 |
| c-untyped | s5 | native (warm cache) | 861.3 | 34.1 | 67 | yes (2 runs) | 40.4/4.2 |
| c-untyped | s3 | native (warm cache) | 828.2 | 30.9 | 73 | yes (2 runs) | 28.8/4.4 |
| c-untyped | scheme | - | 23.9 | 1.4 | 123 | yes (2 runs) | 54.3/3.4 |
| d-g5 | main | native (warm cache) | 705.1 | 30.5 | 139 | yes (2 runs) | 54.6/3.4 |
| d-g5 | pr12 | native (warm cache) | 624.2 | 27.9 | 128 | yes (2 runs) | 33.2/4.7 |
| d-g5 | s12 | native (warm cache) | 654.9 | 26.7 | 98 | yes (2 runs) | 46.5/4.7 |
| d-g5 | s5 | native (warm cache) | 575.3 | 24.7 | 94 | yes (2 runs) | 44.9/5.2 |
| d-g5 | s3 | native (warm cache) | 555.2 | 25.7 | 99 | yes (2 runs) | 46.7/2.4 |
| d-g5 | scheme | - | 179.7 | 13.1 | 461 | yes (2 runs) | 41.9/2.0 |
