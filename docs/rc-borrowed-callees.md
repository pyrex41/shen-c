# Borrowed callees for atomic leaf calls

Based on `a8c37e5a904789f7c77285c67b070c7fb7f5537f`, independently of the
unintegrated mixed-argument borrowing experiment.

A named leaf-primitive call with only atomic arguments resolves its current
function binding before evaluating the arguments, preserving undefined-function
errors and callee snapshot order. Atomic literals and direct lexical reads can
allocate but never execute user callbacks or run a safepoint. The symbol binding
therefore roots the selected closure until dispatch. Exact built-in helpers are
pure; their generic apply/tail fallbacks acquire their own callee reference before
running callbacks. A replacement callback may redefine its own function binding
and collect without invalidating its active self value.

Computed heads, computed arguments, module direct-body calls, cons/reuse calls,
and `do` spines retain owned callee snapshots. `tl` is eligible only outside the
entire active cons-reuse scope, so projection-reuse helpers remain owned.

Because each borrowed call has only atomic arguments, borrowed callee lifetimes
within one activation never overlap. All logical borrowed slots share one local
scalar, with no extra frame allocation or slab storage. Computed-argument
eligibility would invalidate this proof.

Borrowed callee slots never enter the owned frame array. Error cleanup, drops,
and explicit self-recursion clear borrowed pointers without releasing them.
No new managed root or collector edge is introduced. The borrowed getter has the
same undefined-function error as the owned getter; its caller must prevent
binding changes until a dispatch API protects the callee.

## Validation

Independent source review cleared the lifetime proof and implementation.
Fatal ASan/UBSan passed the direct runtime test, including a callback whose only
initial owner is its symbol binding, which replaces itself, collects, then reads
its original captured value. Eighteen fatal sanitizer emitted differential cases pass. Coverage includes literal and
lexical arguments, branches, traps, lexical shadowing, computed arguments/heads,
undefined functions, redefinition during arguments, replacement callbacks,
self-recursion, projection reuse, and an 80-parameter heap frame. Existing emitted
semantics passed 87/87 with an independent Boehm import audit.

Allocation-failure injection was not performed. Argument-allocation error paths
use the same cleanup path as caught primitive failures; borrowed callees are never
released by that cleanup. Future changes making atom construction invoke callbacks
or safepoints must revisit this optimization's eligibility proof.

No full-kernel build or language-suite benchmark ran in this lane.

## Hardware microbenchmark

Darwin arm64; Apple CLT clang, `-O3 -flto`. Empty kernel and one native
self-recursive scanner. Three serial runs each in alternating order, hardware
instructions from `/usr/bin/time -l`. The base executable uses the actual base
emitter and runtime; the middle control uses the base emitter with the candidate
runtime; the candidate uses both new components. Medians:

| Workload | Base emitter/runtime | Base emitter, new runtime | Candidate |
| --- | ---: | ---: | ---: |
| Six `(cons? X)` checks, 2M iterations | 5,741,901,801 | 5,744,259,418 | 4,730,180,348 |
| Six `(cons? (cons 1 ()))` checks, 1M iterations | 5,375,184,732 | 5,379,012,889 | 5,254,948,438 |

The atomic-argument workload executes 17.62% fewer instructions and removes
16,000,002 retain/release pairs. The computed-argument control executes 2.24%
fewer instructions: predicate calls remain owned, while the numeric loop test
and decrement have atomic arguments and borrow their callee snapshots. Counts
for computed predicate argument construction remain unchanged. All programs
return 42. CPU and wall time varied with scheduling and CPU frequency; hardware
counts establish only these scoped gains, not a full-program improvement.
