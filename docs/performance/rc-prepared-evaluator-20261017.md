# Shen 43.1 evaluator and ownership optimization

The [final measured checkpoint](rc-43.1-final-20261017.md) passes the numerical
performance gates and all recorded correctness/platform checks. Strict quiet-host
provenance remains UNKNOWN, so that part of the finish line remains OPEN.
The 43.1 correctness checkpoint `8625781` and its S42 predecessor
remain intact. Work is isolated on `perf/shen-43.1-prepared-eval`, starting from
the comparative profiling checkpoint `c34045b`.

## Measured decisions

Every row below has three alternating unprofiled pairs, with fresh writable
copies of the unchanged pinned suite. Every execution passed 136/0, including
all 36 report checkpoints and the exact `test.txt` redirect output. CPU changes
are median **paired** ratios against the stated reference. These exploratory
measurements do not substitute for the final two batches of five Boehm pairs.

| Candidate | Reference | CPU change | Instructions | Decision |
|---|---|---:|---:|---|
| A1 native capture borrows | RC checkpoint | +0.43% | -0.07% | Reverted: no CPU win; ownership reduction only 0.1% |
| B1 eager prepared evaluator | RC checkpoint | -8.86% | -4.07% | Replaced: RSS +9.4% failed memory gate |
| B2 compact code, lazy heap environments | RC checkpoint | -10.13% | -5.66% | Replaced: RSS +7.4% failed memory gate |
| B3 prepare repeatedly called bodies | RC checkpoint | -10.06% | -5.57% | Retained: maximum RSS 32,342,016 bytes |
| C1 C booleans in native conditions | B3 | -2.40% | -3.37% | Retained: maximum RSS 31,916,032 bytes |
| D1 guarded Prolog borrows | C1 | -2.48% | -3.99% | Retained: maximum RSS 31,916,032 bytes |
| E1 successful owned-exit summaries | D1 | -1.53% | -1.17% | Retained |
| E2 privately rooted call boundaries | E1 | -3.63% | -3.46% | Retained |
| F1 generation-bound dependency cache | E2 | -1.33% | -1.30% | Retained; 2/3 CPU wins |
| G1 direct pvar C condition | F1 | +0.54% | -0.85% | Reverted; 0/3 CPU wins |

F1's exploratory three-way comparison with Boehm has a median paired user CPU
ratio of 0.8892. One pair is 0.9082, and the first Boehm sample has higher
instruction/collection work; every sample is retained. These exploratory pairs preceded the now-passing twenty-run numerical qualification. Maximum observed F1 RSS is 32,047,104 bytes, below the
32,531,251-byte acceptance ceiling and below Boehm. The final frozen pairs and remaining platform checks now pass; strict host quietness remains unqualified.

These changes compare complete implementations. They do not isolate the causal
cost of reference counting versus Boehm.

## Prepared evaluator and lifetime proof

Optional preparation begins after repeated execution of an immutable function
body. A cached plan is guarded by the exact parameter sequence; a shared AST
with different binders cannot reuse the wrong lexical coordinates. Preparation
is bounded to 4,096 nodes/auxiliary entries/literals and depth 128. Unsupported
or oversized roots keep the original evaluator. Failed shapes retry only at
exponentially spaced execution counts. Captured lowercase binders conservatively
keep the generic evaluator, preserving special-form shadowing.

A plan owns a code string, parameter keys and original AST descendants through
ordinary managed closure edges. It never owns its AST root. Pair cache edges
are counted by the exact collector and cleared by unique cons reuse, tail
projection and pooled allocation. They add no vector path or immutable backedge
that would invalidate cycle-source detection. RcValue remains 128 bytes and the
existing object cache stays bounded.

Prepared calls borrow parameters from the runtime-owned argument span and keep
let values in owned C slots. They materialize managed frames only when generic
evaluation or an escaping closure needs them. Every callee is selected before
argument effects. Boolean/context tail guards, partial/overapplication, aliases,
redefinition and uncertain syntax keep their checked paths.

A recursive call can promote a closure while an older invocation is still
walking its syntax. Cold calls retain that historical syntax until return. An
initial adaptive version omitted this root; ASan caught the failure in non-tail
recursion. The corrected control explicitly crosses the preparation threshold
inside a live recursive stack. No failing variant is retained as product code.

## Native conditions and borrowed Prolog walks

Native if/cond conditions use C booleans for literal booleans, short-circuit
syntax and certified canonical predicates. Replaced functions still run through
checked apply. Non-boolean completion preserves the distinct condition versus
boolean-RHS diagnostics, including nested and/or, partial and overapplication.

The sampled Prolog costs include `shen.lazyderef` and `shen.pvar?`; the historical
Boehm backend already implements these operations in C. RC recognizes their
complete pinned 43.1 ASTs, including parameter names. Altered bodies/names fall
back. Runtime guards certify each current primitive dependency and the selected
compiled pvar body. The lazy walk also checks its current recursive binding.
Only after those guards can vector entries be borrowed without callbacks.
Malformed indices, uncertain shapes and walks beyond a bounded 4,096-step prefix
resume the original body with original owners. Successful results acquire their
owner before the arguments are consumed. Exact cycles remain enabled.

## Successful owned exits

Optimized expression emission consumes its intermediate owners and, with an
empty continuation, releases dead lexical bindings. After moving its final
result, a successful generated body has no frame owners. Its arguments have
already moved into that frame. Normal returns can skip both empty cleanup scans;
error and recur edges keep complete cleanup. Metadata literal helpers keep their
conservative cleanup too.

`SHEN_RC_VERIFY_OWNERSHIP=1` checks the frame summary at every successful generated
return. Qualification combines this check with fatal ASan/UBSan and the complete
native kernel/suite. CI sanitizer commands enable it. A private call boundary also uses the caller's strong callee owner and its
private owned scratch array directly for exact non-tail callbacks. Partial,
overapplication and uncertain contexts retain checked dispatch. Borrowed
callbacks keep argument roots through return; owned callbacks can consume them.
Public borrowed/owned callback APIs retain their independent snapshots and
argument-consumption contracts.

## Evidence and remaining gates

Raw experiments are under `evidence/rc/opt-20261017/`; each frozen candidate binds
source, generated C or qualified kernel objects, runtime library, executable,
flags and dependencies. Failed experiments and harness attempts remain there.
The [portable publication](../evidence/rc-optimization-20261017/publication.json)
binds original and published hashes separately; its [candidate analysis](../evidence/rc-optimization-20261017/opt-20261017/candidate-analysis.json)
retains every matched ratio. No binaries or object files are published.

The final sources match frozen F1. Fatal default/boxed runtime and emitted
checks, full generated-kernel sanitizers with ownership assertions, fresh Make
source/CLI/metadata checks, 24/24 CMake tests, Darwin Nix installation and its
complete suite, and all four Linux/macOS/sanitizer CI jobs pass. Exact final
receipts and reproduction commands are in the final measured checkpoint.
The new controls
cover shared-body cache guards, lazily materialized environments, 400 generated
comparisons against the original interpreter, recursive promotion, cache reuse,
cycles, 35 native-condition results and 36 guarded-Prolog cases. The dependency controls warm the guard cache before redefinition, so they exercise invalidation. A per-context single-entry cache stores three permanent symbol keys and a guard mask. Every function publication invalidates it, and saturation disables cache hits.

An early A1 control expected the wrong redefinition order; its corrected oracle
is retained separately. A running shell test driver was edited while adding a
new runtime control, causing a late parser failure; the driver passes syntax
validation and is rerun unchanged. That interrupted run is not a qualification
PASS. Baseline and every candidate's timing samples remain intact.

The final numerical acceptance runs satisfy ten frozen alternating RC/Boehm pairs, CPU ratio
<=0.90 in each batch, eight CPU wins, wall ratio <=1.03, maximum RSS <=1.05 of the
30,982,144-byte pre-optimization RC baseline, and RC below Boehm in every pair.
Profiling stays separate. Defender and desktop activity are present, so host
quietness remains unestablished. Darwin, Linux/Nix and remote CI results must be
reported separately.
