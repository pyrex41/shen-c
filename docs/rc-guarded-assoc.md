# Guarded pinned assoc intrinsic

The emitter recognizes only the complete original `assoc` body and exact parameter names from `shen/src/kl/sys.kl`. It adds an entry check to that body; it does not replace a function binding. Modified bodies compile normally.

The current `assoc` binding must equal the selected unbound two-argument closure. `=`, `cons?`, `hd`, and `tl` must be the current context canonical primitives. These dependencies have no callbacks, so the guards cannot change during the loop. Aliases and old captured closures retain normal behavior when bindings change. Empty lists, improper tails, or a 4096-entry prefix deoptimize before consuming arguments. Successful results are retained before both owned arguments are consumed.

## Validation

Fatal ASan/UBSan: 20 differential programs plus a C harness installing a partial closure as `assoc`, compared against an otherwise identical definition with renamed parameters that disables recognition. Added nil and redefined-equality nil controls passed after the final nil deoptimization change. Cases cover each dependency redefinition before and during argument evaluation, callee snapshots, malformed tails, custom error handling, structural cyclic vector keys, list aliases and prefix fallback. Independent source review cleared the partial-closure fix.

The public immutable pair API cannot construct a cyclic list spine; bounded-prefix fallback keeps its original execution path. No full-kernel or suite performance measurement has been run for this change.

## Bounded microbenchmark

Apple clang O3 ThinLTO, cached list, actual runtime `rc_apply`, three alternating paired CPU-time samples. Short cases use 200000 calls; 64-entry cases use 10000. Hits are at the final entry. These are microbenchmarks, not suite speedups.

| Entries | Result | hd replaced | Control median s | Guarded median s | Change |
|---:|---|---|---:|---:|---:|
| 0 | miss | no | 0.010436 | 0.011402 | +9.3% |
| 0 | miss | yes | 0.010814 | 0.011757 | +8.7% |
| 1 | miss | no | 0.040848 | 0.014263 | -65.1% |
| 1 | miss | yes | 0.052311 | 0.057662 | +10.2% |
| 1 | hit | no | 0.031161 | 0.013232 | -57.5% |
| 1 | hit | yes | 0.051453 | 0.055175 | +7.2% |
| 2 | miss | no | 0.068051 | 0.015727 | -76.9% |
| 2 | miss | yes | 0.096161 | 0.098374 | +2.3% |
| 2 | hit | no | 0.061645 | 0.015666 | -74.6% |
| 2 | hit | yes | 0.089823 | 0.094194 | +4.9% |
| 64 | miss | no | 0.089786 | 0.005367 | -94.0% |
| 64 | miss | yes | 0.124179 | 0.124811 | +0.5% |
| 64 | hit | no | 0.091356 | 0.004893 | -94.6% |
| 64 | hit | yes | 0.121536 | 0.122542 | +0.8% |

Canonical nonempty lists improve substantially in these controls. Guard failures impose a small fixed overhead; empty-list controls still show a few nanoseconds per call of added entry overhead. Profile lengths are unknown, so full-suite impact remains unqualified.

Raw local evidence is under `evidence/rc/guarded-assoc/`: targeted receipts/logs, micro source, binary hashes and all samples.
