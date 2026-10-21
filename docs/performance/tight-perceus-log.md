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

On b-typed, pairs die mostly in cascades, when a whole `deref`ed term is
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

## Timing: load-time compile off (SHEN_RC_EXECUTION=interpret)

Loaded user code runs on the interpreter, with
SHEN_RC_NATIVE_CACHE=/nonexistent so that no compile can succeed. The kernel
(type checker, Prolog, `hash`, `deref`) is native in every row. Each row is the
mean of 2 interleaved rounds, run on 2026-10-21. In this mode CPU and wall time
agree.

| workload | build | mode | instr (G) | CPU s | peak RSS MB | payload ok | load |
|---|---|---|---|---|---|---|---|
| b-typed | main | interpret | 3555.9 | 136.8 | 1592 | yes (2 runs) | 35.5/10.8 |
| b-typed | pr12 | interpret | 2952.5 | 124.7 | 1400 | yes (2 runs) | 28.1/5.3 |
| b-typed | s12 | interpret | 3094.4 | 109.7 | 450 | yes (2 runs) | 27.4/4.7 |
| b-typed | s5 | interpret | 3047.4 | 110.2 | 455 | yes (2 runs) | 17.9/4.7 |
| b-typed | s3 | interpret | 2939.7 | 113.1 | 457 | yes (2 runs) | 27.2/4.5 |
| b-typed | scheme | - | 133.3 | 6.5 | 400 | yes (2 runs) | 25.9/4.4 |
| c-untyped | main | interpret | 988.0 | 45.0 | 86 | yes (2 runs) | 35.5/25.7 |
| c-untyped | pr12 | interpret | 911.1 | 37.7 | 75 | yes (2 runs) | 31.7/22.7 |
| c-untyped | s12 | interpret | 918.5 | 35.1 | 32 | yes (2 runs) | 28.6/25.6 |
| c-untyped | s5 | interpret | 900.4 | 34.0 | 32 | yes (2 runs) | 30.8/23.1 |
| c-untyped | s3 | interpret | 872.7 | 33.9 | 32 | yes (2 runs) | 28.4/22.2 |
| c-untyped | scheme | - | 24.0 | 1.5 | 123 | yes (2 runs) | 29.4/25.0 |
| d-g5 | main | interpret | 557.1 | 21.4 | 78 | yes (2 runs) | 29.4/25.0 |
| d-g5 | pr12 | interpret | 476.1 | 19.5 | 71 | yes (2 runs) | 27.0/19.6 |
| d-g5 | s12 | interpret | 506.3 | 17.7 | 35 | yes (2 runs) | 29.5/15.9 |
| d-g5 | s5 | interpret | 426.4 | 14.0 | 35 | yes (2 runs) | 29.7/13.2 |
| d-g5 | s3 | interpret | 406.0 | 13.9 | 35 | yes (2 runs) | 38.0/11.2 |
| d-g5 | scheme | - | 179.4 | 12.2 | 461 | yes (2 runs) | 37.1/11.9 |

### Overall change, main to s3

Interpret mode:

| Workload | Instructions | CPU | Peak RSS |
|---|---|---|---|
| b-typed | −17% | −17% | −71% |
| c-untyped | −12% | −25% | −63% |
| d-g5 | −27% | −35% | −55% |

The warm native cache shows the same pattern.

shen-scheme on the same host and in the same rounds:
- b-typed: 6.5 s CPU and 133G instructions, about 17× less CPU and 22× fewer
  instructions than s3.
- c-untyped: 1.5 s and 24G, about 22× less CPU and 36× fewer instructions.
- d-g5: 12.2 s and 179G. On d-g5, s3 uses 1.14× scheme's CPU and 2.3× its
  instructions, and 13× less RSS.

### Change by stage (interpret mode, CPU)

**#12** (immortal symbols, counters only in diagnostic builds, closure-free
named calls)
- −9% CPU on b-typed, −16% on c-untyped, −9% on d-g5.
- −17% instructions on b-typed.

**Source-rooted collector plus slab cells (s12)**
- −12% CPU on b-typed, −7% on c-untyped, −9% on d-g5.
- RSS falls about 3× on b-typed (1.4 GB to 450 MB).
- Instructions rise 5–6%: the slab allocator and the visited-bit walks
  execute more instructions per operation, but touch much less memory.

**musttail (s5)**
- CPU about 0% on b-typed, −3% on c-untyped, −21% on d-g5.
- d-g5 and a2-libs-typed have the most non-self kernel tail calls.

**Inline fast paths (s3)**
- −3.5% instructions on b-typed, −3% on c-untyped, −5% on d-g5.
- CPU is within noise: +3% on b-typed, about 0% elsewhere.

## Verdict

Precise RC with Perceus-style reuse cannot make shen-c competitive with
shen-scheme on b-typed or c-untyped.

**Reuse has no headroom.**
- Reuse hits 0.06–0.08% of cons sites. The candidates are shared: `deref`
  copies terms that are still live.
- At most 6.6% (b-typed) to 52% (c-untyped) of pair allocations follow a pair
  free. The free list already recycles that memory.

**Removing the plain RC overhead was worth 12–35% CPU and 55–71% RSS.**
That overhead was:
- the full-heap collector, which scanned 148M objects and freed 0;
- the global list;
- 112-byte pairs;
- heap tail requests;
- out-of-line retain/release/hd/tl.

**The remaining 17–22× gap is not memory management.**
- c-untyped is the kernel's `hash` (37% of allocations are its strings).
  shen-scheme overrides it natively.
- b-typed is the kernel's `deref`, which copies whole terms: 1.45G pair
  allocations, plus 7.2G retains and 9.0G releases.
- shen-scheme runs the same `deref` KL, but with Chez's bump allocation, no
  refcount traffic and cp0 inlining.
- What is left for RC is the ownership traffic itself: about 4 retains and
  5 releases per allocation. Only borrowed-parameter inference in the emitter
  (Perceus proper) would remove it. That is a rewrite of emit.c's owned-slot
  calling convention, and it was not attempted.
- The native `deref`/`hash` overrides (excluded from this branch on purpose)
  are the next lever.
