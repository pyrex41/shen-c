# Shen 43.1 comparative CPU profile

**Profiling complete; performance target OPEN.** Product runtime, compiler,
kernel, suite and public statistics contract are unchanged from `8625781`.
Worktree: `perf/shen-43.1-profile`.

Next production experiment: compiler ownership/effect summaries targeting
**temporary closure and symbol owners during source processing**. Preserve
generic paths for callbacks, escape, partial/overapplication, redefinition and
uncertain ownership. Do not revive the rejected interpreted callee-borrow
experiment unchanged. Even a zero-capture generated body can consult `self`
later for a self-loop after a callback has replaced its binding.

## Exclusive CPU budget

Three fresh accounting processes per backend ran the complete pinned suite.
These means are additive **exclusive process CPU seconds**, subtracting child
scopes. Boundaries are selected compiled non-tail calls to `read-file`,
`shen.shen->kl`, `shen.typecheck`, and the complete `eval-kl` evaluator entry.
Other work remains unclassified; these are not exhaustive function categories.

| Boundary | RC mean | Boehm mean | RC minus Boehm |
|---|---:|---:|---:|
| `read-file`, including parsing and macro processing | .739 | .441 | +.298 |
| Shen to KLambda translation | .400 | .185 | +.214 |
| Typechecking | 2.497 | 2.588 | -.091 |
| Evaluation, excluding nested classified work | .198 | .290 | -.092 |
| Other: setup, type preparation, output, teardown, etc. | 1.054 | .994 | +.059 |
| Total | 4.887 | 4.499 | +.388 |

This points to source reading/processing and translation as the main excess
work. **It does not establish a production typechecker win.** Matched three-pair
calibration measured median accounting/release CPU ratios of **1.00862 RC** and
**1.05060 Boehm**. Boehm ratios were .99758, 1.05060, 1.05085; instructions and RSS
also varied (about 74 MB versus 65 MB for accounting processes). The reason for
that collection/memory variation was not isolated. Do not uniformly scale
individual phases or treat their differences as an exact causal decomposition
of the release timing gap.

Earlier unchanged-binary load intervals independently put L near parity
(2.343/2.304 seconds) and C-minus behind (.744/.596). New exclusive detail:

| Load | Backend | Read/process | Translate | Typecheck | Other + evaluate |
|---|---|---:|---:|---:|---:|
| C-minus | RC | .177 | .146 | .231 | .193 |
| C-minus | Boehm | .093 | .067 | .276 | .183 |
| L interpreter | RC | .079 | .071 | 2.090 | .114 |
| L interpreter | Boehm | .051 | .033 | 2.140 | .104 |

All 24 primary diagnostic runs pass 136/0, regenerate `test.txt` as `hello`, and
print the same ten typechecking-inference counts, ending at 1,634,358. Vendor
inputs remain pristine. Each accounting run conserves CPU across its exclusive
scopes and ends with only its process root open before finalization.

## Stack evidence

Each backend has three complete-process profiles and three for each selected
load. Focus runs stop at load entry, attach the sampler, execute the load, and
stop at load exit; they then resume and complete the suite. Paused/profiler
stacks are removed from these denominators:

| Scope | RC non-paused samples | Boehm non-paused samples |
|---|---:|---:|
| Complete process | 2,427 | 2,172 |
| C-minus load | 370 | 324 |
| L interpreter load | 1,124 | 1,079 |

These are **wall-clock stack samples**, including blocking syscalls, not exact
CPU percentages. Recursive inclusive stacks are never summed. The analyzer
reconstructs self counts from the complete call tree, retaining leaves below
`sample`'s five-hit flat-summary cutoff. Generated symbols map to named Shen
functions; anonymous closures can remain unnamed. LTO can hide ownership work
within a caller's self samples.

RC's largest individual entries are `application` (342) and `evaluate` (199):
541/2,427 whole-process samples. `apply_small`, `apply_request`,
`rc_tailcall_owned_args` and `consume_arguments` add 338. C-minus repeats those
costs alongside `shen.deref`, `shen.map-h`, cons/equality primitives and object
cleanup. L application/evaluation contribute 297/1,124 non-paused samples.

Boehm includes `GC_mark_from` (347), `GC_push_contents_hdr` (146),
`GC_reclaim_generic` (154), `GC_malloc_kind` (121), `eval_kl_object` (265),
`shen_apply` (175), and `apply_exact` (170). These compare complete implementations;
they do not causally isolate collector cost. RC exact cycle collection stays
enabled. This evidence favors application/ownership work over collector redesign.

## Bounded counter experiment

A source snapshot removes exactly the two `retain_calls`/`release_calls`
increments, preserving actual reference counts, graph edges, collection budgets,
reuse limits and exact cycle collection. Product source/defaults stay unchanged.
Five alternating checkpoint/control pairs all pass 136/0:

| Metric | Median paired control/checkpoint ratio |
|---|---:|
| User CPU | **.97408** |
| Wall | .97468 |
| Instructions | .96532 |
| Peak RSS | .94406 |

CPU wins: 5/5. CPU ratios: .98031, .92276, .96336, .98478, .97408.
The first seven runtime tests passed fatal ASan/UBSan, then unchanged
`test_immediates` correctly rejected the missing public statistic. This is an
intentional diagnostic-contract failure, **not full runtime qualification**.
The control is rejected as a production candidate. Its modest benefit is far
below the roughly 18% reduction needed from the recorded 4.82-second checkpoint
to reach 90% of Boehm's recorded 4.39 seconds. Code generation/layout changes can
contribute to the timing/RSS effect; this is not an isolation of two machine
instructions' dynamic cost.

A separate stats-preserving typed-counter executable passes 136/0 and records
1,523,334,682 ownership operations:

| Type | Retain + release operations | Share |
|---|---:|---:|
| Closure | 567,607,113 | 37.26% |
| Symbol | 436,976,330 | 28.69% |
| Pair | 254,233,715 | 16.69% |
| Vector | 158,969,051 | 10.44% |
| Environment | 54,098,135 | 3.55% |
| Tail request | 41,238,992 | 2.71% |

Boolean symbols are a subset: 259,045,179 operations (17.0% of all operations).
These are operation counts, not CPU fractions.

## Next measured optimization

1. Attribute closure/symbol ownership to callee snapshots, captures, argument
   staging and parameter frames during reading/translation, in diagnostic builds.
2. Implement one bounded compiler summary certifying that an owner stays live
   and a borrow cannot be invalidated by redefinition, callbacks or escape.
   A pure-looking argument list alone is insufficient. Confirm the predicted
   ownership reduction before claiming a CPU gain.
3. Use C-minus as the regression case and L as the large near-parity control.
   Retain aliasing, escape, partial/overapplication, self replacement, error
   order, mutation/cycles and bounded tail-memory controls. Then run the unchanged
   suite and matched unprofiled checkpoint/candidate measurements.
4. If this stalls, prepare immutable interpreted call shapes and lexical slots
   once, preserving dynamic callee snapshots. Measure plan storage against the
   5% RSS gate; avoid retaining a second unbounded AST cache.

Matching Boehm's reading/translation cost alone would not reach the 10%-faster
finish line. Common application/dispatch work also needs improvement. No new
production optimization or final acceptance win is claimed here.

## Reproduction and evidence

Raw local root: `evidence/rc/profile-20261017`. Portable compressed copies are
indexed in [publication.json](../evidence/rc-profile-20261017/publication.json),
with separate original/published hashes. Combined report:
[analysis.json](../evidence/rc-profile-20261017/analysis.json). The primary
executed harness is preserved separately from subsequent guard improvements.
A final one-repetition smoke checks the current driver on all eight backend/mode
combinations. Binaries and object files are not published.

```sh
python3 test/rc/test_phase_profile.py
python3 scripts/build-phase-profile.py \
  --rc-generated ../shen-c-43.1/obj/rc/kernel.c \
  --boehm-root ../shen-c-boehm-43.1 \
  --gc-include /nix/store/q8mzql20dwjdc6pd9bn49wya477daxsz-boehm-gc-8.2.12-dev/include \
  --gc-lib /nix/store/r9bdx29an70k9jg6jj2h1ccnpjqdbwzg-boehm-gc-8.2.12/lib/libgc.1.dylib \
  --out evidence/rc/profile-new/build
python3 scripts/compare-phase-profile.py \
  --build evidence/rc/profile-new/build --boehm-home ../shen-c-boehm-43.1 \
  --out evidence/rc/profile-new/comparison --repetitions 3
python3 scripts/build-rc-counter-control.py --checkpoint ../shen-c-43.1 \
  --out evidence/rc/profile-new/counter-control
python3 scripts/benchmark-rc.py \
  --config evidence/rc/profile-new/counter-control/benchmark.json \
  --out evidence/rc/profile-new/counter-timing
python3 scripts/build-rc-counter-control.py --checkpoint ../shen-c-43.1 --typed \
  --out evidence/rc/profile-new/type-counts
```

The typed binary emits `RC_TYPE_COUNTS` on stderr; run it in a fresh suite copy
through the manifest validator. Calibration configuration and all samples are
published, including failed/superseded attempts. Use the benchmark runner for
calibration too. Do not profile acceptance timing runs.

Superseded attempts: missing historical `SHEN_C_HOME`; overhead from testing
every Boehm ABI call's name; incorrect tagged-string load labels, rejected by
the focus-completion guard. One-millisecond expanded reports were 443–551 MiB;
they remain local and are published compressed. Five-millisecond reports peak
around 131 MiB. The old polling limit could overrun at sampler exit; the final
driver enforces 192 MiB during and after report generation. Fatal ASan/UBSan
helper controls cover nested accounting/exception unwinding; other controls
reject inconsistent recursive trees and failed matched runs.

Unrelated Defender, desktop and background analysis activity is captured in
process snapshots. Quiet-host acceptance remains unestablished. Linux, Nix and
remote CI qualification of these Darwin-only tools is not claimed. The product
checkpoint's existing platform limits remain unchanged.
