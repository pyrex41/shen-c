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
The b-typed counter run is still running: its cold native compile under the
counter build is slow.

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
| 2fe55c6 | musttail | PASS | pending | | | |

## Cold compile cost on main (compile-first, PR #14)

Cold b-typed on main 8c94154 took 6432 s wall and 4140 s user. compile_s was
4469, it emitted 105 MB of C, and peak RSS was 3.3 GB (load ~30–60). With the
default 120 s SHEN_RC_COMPILE_TIMEOUT, the warm-up runs of b-typed and d-g5
under that load produced wrong payloads. All warm-ups therefore use
SHEN_RC_COMPILE_TIMEOUT=1200, and the timed rows assert compiles=0.
