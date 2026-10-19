# Already-loaded native execution, 2026-10-19

Status: diagnostic controls PASS; whole-suite Boehm acceptance remains OPEN.

Each control boots the same 16-module 43.1 kernel, sets up its workload, runs
one complete warm-up call and resolves the function again after lazy publication.
The timed C loop calls that function directly. It includes result checking and
releases, and excludes boot, source loading, C compilation and first-call setup.
Every timed native window recorded zero compiler-child CPU and zero interpreter
entries. This measures repeated fixed workloads, not all Shen programs.

Three rounds alternate native/interpreter order. Native execution and the
explicit diagnostic interpreter share one Apple clang arm64 -O3/thin-LTO host
binary; dynamic native modules use -O1. These are comparisons with the same
implementation's interpreter, not Boehm, Scheme or Common Lisp.

| Workload | Native / interpreter CPU | Speedup |
| --- | ---: | ---: |
| tail-count | 0.361 | 2.77x |
| list-build | 0.615 | 1.63x |
| queens | 0.518 | 1.93x |
| prolog | 0.723 | 1.38x |
| typecheck | 0.986 | 1.01x |

Ratios are medians of paired user-CPU ratios. N-Queens uses the pinned suite's
six-queen program and checks four solutions; Prolog loads the pinned naive
reverse program and reverses twelve integers. Typechecking checks the type of
`(+ 1 (* 2 3))` through the real kernel. Tail-count and list-build are controls
with fixed inputs and independently checked results. Native and interpreter
execute identical case inputs and repetition counts.

The calibration run `execution-20261019T192831Z` overlapped a profiler and is
explicitly excluded. Accepted execution evidence is
`execution-20261019T192940Z`, whose source and binary hashes remained unchanged.
The archived [portable receipt](../evidence/compiled-execution-20261019/result.json)
contains every retained measurement and original log hashes; compressed logs
are alongside it. This shared host was also running a sanitizer qualification;
repeated quiet-host whole-suite acceptance remains a separate gate.

## Profile and next bounded candidate

A separate three-second sample of repeated loaded native typechecking obtained
2,508 worker samples. The main thread waits for the worker and is excluded from
CPU attribution. The largest individual leaf entries were graph-edge traversal
(113), release-edge work (111), tail-request dispatch (108), owned tail-call
creation (108), `shen.system-S-h` (98), release (88) and collection (83). This
is a distribution of sampled stack leaves, not additive inclusive stack times.
The driver sets maxinferences high enough for the complete repeated workload;
an earlier short failed profile is retained locally and provides no timing.

The repeated benchmark records 3,787 retains and 4,147 releases per simple
native typecheck. The kernel typechecker is compiled in both modes, explaining
why compiling user definitions alone changes this control very little.

Next hypothesis: condition predicates can borrow operands already rooted in
lexical slots. Restrict this to callback-free operand lookup, keep original
bindings live through the call, and preserve the function snapshot. Certified
builtins avoid temporary ownership; redefined predicates use checked borrowed
apply. Keep effectful operands and uncertain cases on the existing owned path.
Measure reference-count reduction, affected condition/redefinition controls
and the complete unchanged suite before retaining the candidate.

## Reproduce

```sh
make -f Makefile.rc CC=clang RC_LTO=1 bin/rc/bench_execution
python3 scripts/measure-execution.py bin/rc/bench_execution --cache evidence/native-cache --rounds 3
```

The benchmark prepares a fresh writable pinned suite for every sample. Its
`RC_EXEC_BEGIN` / `RC_EXEC_END` markers delimit the timed window; `RC_NATIVE`
diagnostics cover the entire process, including excluded setup compilation.
Profiles must run separately from the timing batch.
