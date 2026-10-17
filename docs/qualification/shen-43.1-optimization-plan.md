# Measured evaluator optimization

Start: `c34045b`; product baseline `8625781`. Isolated worktree/branch:
`perf/shen-43.1-prepared-eval`. Preserve both earlier worktrees and evidence.

1. Candidate A1 borrowed immutable native captures through the active closure.
   Fatal sanitizer controls and the full suite passed, but only 0.1% of whole
   suite ownership operations disappeared. Three matched pairs showed no CPU
   win. Reverted; raw evidence is under `evidence/rc/opt-20261017/candidate-a1`
   and `timing-a1`. The saved emitter experiment is not production code.
2. Candidate B prepares interpreted function syntax and lexical coordinates
   once, retaining all dynamic function lookups before argument effects. Keep
   unsupported syntax on the original evaluator, bound optional preparation,
   and guard shared AST caches by the exact parameter sequence. Cache graph
   edges must preserve exact cycle detection and pair reuse invariants.
3. Run focused lazy validation, shadowing, capture/alias/cycle/redefinition,
   error and tail controls under fatal ASan/UBSan, then the unchanged complete
   43.1 suite. Bind source, generated C, executable, flags and dependencies
   before matched timing against baseline. Reject CPU or memory regressions.
4. Record ownership reductions and sampled profiles separately from unprofiled
   alternating pairs. Continue measured compiler/runtime work if the target
   remains unmet; keep diagnostics and exact cycles enabled.
5. Final candidate: two batches of five RC/Boehm pairs, whole generated-kernel
   sanitizers, default/boxed runtime, emitted controls, CLI/source/metadata,
   Make/CMake/Nix qualification. Keep performance OPEN until every agreed
   performance gate passes. Linux/remote CI remains a separate platform gate.

Measured continuation: B3 passes the exploratory memory gate; C1 native conditions, D1 guarded Prolog borrows and E1 owned-exit summaries each win matched pairs. E2 tests privately rooted dispatch. See `../performance/rc-prepared-evaluator-20261017.md` for exact evidence boundaries.

E2 and F1 improve matched CPU; G1 was reverted after 0/3 wins despite fewer instructions. Freeze F1 for the ten-pair gate and remaining CLI/build/platform qualification.

Frozen F1 now passes ten paired numerical gates (0.8811/0.8360 CPU, 10 wins, RSS +3.49%), complete correctness and all four CI lanes. Final report: `../performance/rc-43.1-final-20261017.md`. Strict quiet-host provenance remains UNKNOWN; no further product optimization is needed to meet the recorded numerical target.
