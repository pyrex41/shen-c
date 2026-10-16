# Boehm-free Shen-C implementation

Status: implementation is integrated and locally qualified on Darwin arm64.
The experimental checkpoint `97aa8e1` passed the bundled S42 suite but still
depended on Boehm in the compiler, used conservative ownership, and lacked a
standalone Shen executable. The continuation replaces those components and
adds a native Shen source compilation driver. Exact executed results are kept
in [the validation record](rc-validation.md).

Completed: independent compiler host, standalone embedded-kernel CLI, native
Shen source driver, local ownership insertion, free-variable capture, owned
calls, selective pair reuse, and supported build migration away from Boehm.
The final unchanged S42 suite passed 134/0 with fatal ASan/UBSan. Fresh Make
and CMake qualification, standalone frontend sanitizer checks, native source
fixtures, and dependency audits passed. Nix build/install, Linux execution,
and remote CI remain unqualified; the validation record states those limits.

The current user request authorizes an implementation beyond the earlier
documentation-only rung-2 proposal. The goal is full Shen semantics with an
explicit reference-counted C runtime and exact cycle reclamation. This is
automatic memory management; it is not a claim of no garbage collection.

## Architecture

- A separate `src/rc` runtime owns its values, heap edges, globals, function
  bindings, closures, and mutable vectors. Public arguments are borrowed;
  value results are owned. Errors are explicit, not nonlocal C jumps.
- Reference counts reclaim acyclic garbage; an exact cycle pass reclaims
  unreachable cyclic graphs without scanning the C stack.
- A new native emitter lowers KL functions to C and inserts ownership
  operations. Runtime `eval-kl` uses a separate RC evaluator when invoked.
  Native compilation must not become a wrapper around source evaluation.
- The compiler host uses the independent RC reader and AST. Compiler, CLI,
  and generated applications link only the RC runtime and ordinary C dependencies.
- Historical interpreter/AOT source remains a comparison reference; supported
  build paths have no Boehm dependency. Compatibility requires full suite evidence.

## Team ownership

| Lane | Owned paths |
| --- | --- |
| Runtime | `src/rc/rc.h`, `src/rc/rc.c`, heap unit tests |
| Compiler | `src/rc/emit.*`, `tools/rc-build.c` |
| Primitives | `src/rc/primitives.*`, primitive tests |
| Dynamic evaluation | `src/rc/eval.*`, `src/rc/read.*`, evaluator tests |
| Independent validation | cycle tests, validation scripts and inventory |
| Coordinator | build integration, documentation, integration fixes, full checks |

All lanes use the isolated `feat/perceus-rc` worktree based on `cc07d10`.
They coordinate the runtime header before relying on its ABI. Heavy integration
builds and full suites are serialized by the coordinator.

## Execution and acceptance

1. Agree runtime ownership and native-call ABI; implement the heap and cycles.
2. Implement primitives and dynamic evaluation with explicit cleanup.
3. Implement direct C emission, closures, exception paths, and tail calls.
4. Integrate builder and run focused ASan/UBSan ownership tests.
5. Run generated fixtures and full bundled S42 tests against the same inputs.
6. Inspect linked artifacts for Boehm symbols/dependencies and source for
   genuine native definitions. Review the final diff and record exact results.

Acceptance includes self/two-node/mixed cycles, live aliases, mutation,
global replacement, closure escape, partial/overapplication, deep destruction,
self/mutual tail calls, error unwinding, dynamic definitions and eval, I/O,
and full kernel execution. Allocation statistics must demonstrate reclamation
during execution as well as context teardown.

Precise last-use insertion, reuse optimization, performance, and full-language
coverage are separate claims. Scoped ownership alone is not full Perceus.
Unknown or failed gates remain explicit. No publishing is requested.

## Continuation after 97aa8e1

1. Replace the Boehm/libshenc compiler frontend with the RC reader and AST.
   Build and audit both compiler and emitted applications without Boehm.
2. Insert last-use moves/drops, capture only free variables, and introduce
   owned call boundaries so unique values can remain unique across calls.
3. Reuse unique cons storage in actual emitted programs; measure allocations
   and preserve aliases, errors, partial calls, and cycle safety.
4. Produce a standalone `shen-rc` executable with the actual compiled kernel,
   source loading, evaluation, and REPL; replace the default CLI.
5. Run focused ownership/reuse/CLI checks, canonical unchanged S42 suite,
   sanitizers, and dependency audits against the new source snapshot.

The user explicitly confirmed that cycle reclamation is acceptable and asked
to remove Boehm completely. Retain unrestricted Shen semantics and exact cycle
reclamation by default. An optional acyclic-only build is supplementary, not
a replacement for the default semantics. Published
Perceus does not establish collector-free reclamation of arbitrary mutable
cycles; do not claim that local liveness or reuse removes this requirement.

Compiler owns the AST/frontend migration first, then hands the native emitter
to the ownership lane. Runtime owns reuse and opt-in owned call ABI. Evaluator
owns the CLI. Coordinator owns builds, integration, and full qualification;
validation and review independently check executable evidence and semantics.

## Performance continuation after 4157892

The sanitizer suite result is not a release performance result. The initial
O3 CLI run passes 134/134 in 99.978 seconds including sampling overhead;
performance remains open. Preserve Boehm-free operation and exact cycle
reclamation while completing these steps:

1. Measure preserved Boehm and RC release CLIs on identical suite sources,
   input, and invocation; record binary hashes and separate wall/CPU time.
2. Profile allocation, call dispatch, cycle collection, and dynamic evaluation.
3. Remove redundant ordinary-call allocations with ownership-preserving fast
   paths, independently review, and run focused sanitizer regressions.
4. Compare unchanged release binaries serially, then qualify improvements
   against the full suite and dependency audit. Report remaining regression.

The first bounded performance pass is implemented and validated: small exact
calls, inline tail arguments, and empty-slot release guards. Current full-suite
release runs are 57.09–70.88 seconds versus preserved Boehm 12.54–15.43 seconds,
all 134/134. Performance parity remains OPEN. See `rc-validation.md` for current
receipts and historical sanitizer boundaries. The new artifact was profiled
separately; next work should reduce remaining dispatch, symbol lookup, and allocation costs;
do not disable cycle reclamation or substitute a smaller suite.

## Aggressive performance campaign after c0b4309

User acceptance target: outperform the preserved Boehm CLI on the unchanged
full 134-test S42 suite, with Boehm completely absent and exact cycle
reclamation preserved. A partial speedup does not meet this target.

Parallel isolated candidate lanes: compiled symbol tables; guarded primitive
calls; self-tail-call loops; frame-slot reuse; bounded object reuse with ASan
poisoning; exact cycle-collection work reduction. Build lane evaluates LTO
without changing arithmetic semantics. Independent review gates integration.

Coordinator integrates reviewed commits, resolves overlapping runtime/emitter
changes, and qualifies combined source. Benchmark lane runs a single serial
queue with immutable binary/suite/input identities and balanced repetitions,
recording wall time, CPU time, retired instructions, and memory. No concurrent
compilation during final timing. Reject semantic shortcuts, suite edits,
disabled collection, hidden unbounded caches, or performance claims from
sanitizer runs. Keep correctness and performance gates separate.

### Resumed second round, 2026-10-16

An authentication refresh failure interrupted the coordinator and four lanes;
their isolated worktrees and completed artifacts were preserved. The seven
first-round changes at `7933147` pass the unchanged 134-test suite. A fresh
serial exploratory comparison measured O3 at 54.60 s wall / 50.30 s user CPU,
ThinLTO at 41.75 s / 38.53 s, and preserved Boehm at 18.29 s / 15.43 s.
These are single runs, not a repeated performance qualification. ThinLTO is
still 2.50 times slower by user CPU. Raw identities and stage timings are in
`evidence/rc/perf-combined-three-20261016T141748Z/result.json`.

Second-round work targets equality, interpreted argument and lexical lookup,
compact managed environments, primitive dispatch, tail recursion hidden by
`do`, redundant generated cleanup, and selective initialization of pooled
objects. Each change requires independent review and focused correctness
evidence before integration, followed by a fresh combined build and suite.
Native list overwrites that bypass helper redefinition are deferred and must
not count toward the unchanged-semantics performance target.

### Near-parity checkpoint, 2026-10-16

At `e28bee9`, a fresh default-IPO release build passed 17 runtime tests,
13 reader cases, three dependency audits, and three standalone CLI checks.
The fourth serial exploratory pair passed the unchanged suite 134/0 for
both implementations: RC 21.17 s wall / 19.72 s user CPU, preserved Boehm
21.65 s / 19.28 s. This single wall-time advantage does not establish the
performance target: RC still used 2.3% more user CPU and host scheduling
differed. Evidence: `evidence/rc/perf-fourthwave-20261016/result.json`.

The combined focused fatal ASan/UBSan run passed all 17 runtime binaries in
default and boxed modes, 87 emitted semantics cases, and 20 ownership cases.
A structural cleanup assertion caught lost NULL facts after borrowed-callee
statements; `79c490f` recognizes only those three exact statements and restores
the original assertion unchanged, with an additional retained-helper control.
The raw failed run remains preserved with its correction.

Remaining bounded candidates are generic canonical builtin dispatch and cons
classification/reuse. They remain isolated until reviewed and measured. Then
freeze the combined source, build a fresh release CLI and fatal sanitizer CLI,
run the unchanged full suite, and run four alternating serial release pairs
with all team compilation stopped. Require an actual repeated improvement,
then refresh the validation record, install the qualified local artifacts,
and retain the precise evidence and platform limits. No publishing requested.

### Repeated full-suite checkpoint, 2026-10-16

The combined `8dbc9414` candidate passed fresh full ASan/UBSan 134/0,
19 runtime tests in default/boxed modes, nine emitted scripts, and default Make
134/0 plus native-source proof. Four alternating release pairs all favored
Boehm: RC median 20.425 s wall / 19.530 s CPU versus 16.460 s / 15.615 s,
with median paired ratios 1.2643 and 1.2592. **Faster-than-Boehm target NOT MET;
performance remains OPEN.** Earlier single-pair near-parity is not a win.

See [the checkpoint](performance/rc-suite-20261016.md). Continue profiling major
costs, preserve unrestricted exact cycle semantics and bounded storage, and
qualify each next immutable candidate before repeated matched measurements.
No artifacts were installed or pushed at this checkpoint. Historical evidence stays.
