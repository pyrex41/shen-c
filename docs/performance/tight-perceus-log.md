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
