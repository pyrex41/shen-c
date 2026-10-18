# Certified identity tail forwarding in the interpreter

Base: `fd1fd1e2`. The original candidate lane produced the focused runtime/emitter
and microbenchmark evidence below. Subsequent full-kernel release qualification
and eight 134-case suite executions passed at `cf8a36fb`; see the
[full-suite checkpoint](rc-suite-identity-tail-20261016.md). Performance remains
OPEN; the microbenchmark improvements below do not establish a suite speedup.

## Certificate and dispatch

A certificate means the exact, effect-free two-parameter function body returns
its second parameter. It belongs to the immutable selected closure, independently
of its name. The compiler proves two distinct symbol parameters and a body
exactly equal to parameter two, sharing the proof with existing generated self-
loop guards. The interpreter validates parameter keys once and proves the body
symbol is parameter two. Original primitive DO has the same exact semantics.
Trusted fixed-arity constructors mark these closures; arbitrary native callbacks
must use ordinary constructors. The byte fits existing header padding: RcValue
remains 128 bytes in default and boxed modes. Every allocation resets the flag;
partials use ordinary construction and conservatively clear it, including a
zero-argument partial. Runtime matching also requires same context, closure type,
original arity two and no bound arguments.

Application resolves and owns its selected function before either argument.
Only a tail-position call with exactly two arguments and that certificate can
forward. Argument one evaluates normally. Argument two evaluates in tail position
and yields an owned value or request; its owner is acquired before the ignored
argument and selected function are released. Redefinition during either argument
cannot replace the selected snapshot. Lexical aliases and computed heads use
the same immutable selected-callee guard. Ordinary calls, partial/overapplication,
and other bodies retain normal dispatch. Successful forwarding performs an
explicit postcleanup safepoint.

This follows the existing compiler/Perceus last-use ownership contract. Ignored
values and unused callee captures may die before a deferred second expression
executes. An otherwise unreachable temporary stream can therefore close earlier
than in the previous recursive interpreter. Reference counts, collection/reuse
statistics and destruction timing can change. There is no promise of identical
close timing for unreferenced streams. Values used by argument two remain rooted
by an owned argument, tail request, closure capture or other valid C owner.
Borrowed FILE pointers still require a live stream owner.

## Preserve operand-context validation

Custom native callbacks can return a foreign-context heap value. Previously DO
evaluated argument two fully before rejecting such a value through normal owned-
call validation. Immediate request validation alone would miss the eventual
foreign result. Forwarding therefore adds a deferred final-result context check
with the exact `invalid owned argument/context` diagnostic.

Tail requests carry a normalized completion guard:

| Guard | Completion operations |
| --- | --- |
| 0 | none |
| 1 | boolean (which also validates context) |
| 2 | owned-argument context |
| 3 | context, then boolean |

Composition preserves the first diagnostic. A boolean check followed by context
needs only boolean; context before boolean retains both. Adjacent pending guards
merge without crossing an overapplication node, so arbitrarily long adjacent
DO/boolean tail chains need one pending guard. Guard changes copy a shared request,
leaving its original C-owned behavior unchanged. Both request constructors reset
the guard. Pending guard metadata increases the private pending node from 16 to
24 bytes on this host; it adds no managed graph edges or permanent context roots.

If argument one is foreign, argument two keeps its original ordinary evaluation,
then the original generic dispatcher rejects the operands. Thus right-side effects
and errors still precede operand validation. Boolean-inside-DO reports its boolean
error first, while DO-inside-boolean reports its earlier context error. Generic
C callback completion behavior outside these existing validation boundaries is
unchanged.

## Correctness evidence

Independent read-only review cleared the final guarded implementation under the
stated last-use timing contract. An earlier unguarded measurement was exploratory
and is superseded by the guarded evidence below.

All 22 registered runtime executables passed fatal ASan/UBSan in default and boxed
modes: 44 executions. This includes exact cycles, ownership/reuse, primitive and
numeric differential cases, object caching, environments, equality, shared tail
requests and boolean continuations. The new fixture covers:

- certificate guards, foreign context, pool reset and partial clearing;
- first-argument redefinition plus collection, aliases and computed heads;
- 100K primitive/interpreted DO recursion and 30K boolean/error continuations;
- live shared streams, escaping closure captures, ignored temporary stream
  release, and cyclic vectors across collection;
- managed RC_TAIL data, shared-request copy/reuse and ignored-request nonexecution;
- exact foreign-left/right diagnostics, right-side effects/error precedence,
  boolean guard order and both sides of overapplication boundaries.

The new runtime test is registered in CMake, Makefile.rc and scripts/test-rc.sh.
The emitted fixture passed 14 expected results in owned and conservative modes,
with default and boxed runtime values: 56 comparisons under fatal ASan/UBSan.
It constructs interpreted ASTs as ordinary cons values, uses compiled exact DO
closures matching the kernel definition, checks emitted certificate constructors,
and exercises 40K interpreted recursion, 20K boolean/trap chains and snapshots.
No original collector or context validation rule is disabled.

## Final guarded microbenchmarks

Apple CLT clang, Darwin arm64, `-std=c17 -O3 -flto=thin`. Exact base and candidate
runtime sources are separately linked. Each case executes 300K logical recursive
steps in batches of 500 (keeping base C recursion bounded). Three serial samples
per executable in AB/BA/AB order; `/usr/bin/time -l` medians:

| Workload | Base instructions | Candidate | Change | Base/candidate CPU |
| --- | ---: | ---: | ---: | ---: |
| Primitive DO | 869,119,773 | 888,103,361 | +2.18% | 0.10 / 0.08 s |
| Interpreted exact identity | 1,173,921,944 | 887,743,077 | -24.38% | 0.12 / 0.08 s |
| Owned-native exact identity | 983,139,255 | 886,040,687 | -9.88% | 0.11 / 0.08 s |
| Redefined effectful DO | 1,303,359,765 | 1,319,767,143 | +1.26% | 0.14 / 0.14 s |
| Partially bound closure | 990,745,845 | 1,001,239,828 | +1.06% | 0.12 / 0.11 s |
| Ordinary tail recursion | 763,331,286 | 773,052,347 | +1.27% | 0.06 / 0.07 s |
| DO outside tail position | 923,500,337 | 933,705,599 | +1.11% | 0.08 / 0.08 s |
| Boolean tail control | 858,351,611 | 870,596,511 | +1.43% | 0.08 / 0.08 s |
| Boolean plus primitive DO | 969,245,261 | 982,142,088 | +1.33% | 0.11 / 0.09 s |

The owned-native fixture uses the same effect-free owned return convention as an
emitted identity callback; actual emitted proof/dispatch is checked separately
by the sanitizer fixture. The candidate certificate constructor is the benchmark's
only conditional source; base uses the ordinary constructor for that same body.

Interpreted identity managed allocations fall 900,601 to 600,601, and retains
2,103,000 to 1,803,000. Owned-native allocations remain 600,601; retains fall by
300K. Primitive DO allocations rise 300,601 to 600,601 as C recursion becomes
managed tail requests; its peak managed objects fall 656 to 158. Interpreted and
owned-native peaks likewise fall from 657/656 to 159/158. Negative controls keep
the same managed counts. Boolean-plus-DO also trades more tail requests for a
smaller peak (660 to 162). CPU times are short and noisy; instructions and
allocation counts establish the scoped tradeoff, not full-program speed.

Reproduce with `test/rc/bench_identity_tail.c`, modes primitive, interpreted,
owned, effectful, partial, plain, ordinary, boolean and boolean-do, argument
300000. Define `SHEN_RC_IDENTITY_TAIL_OPT=1` only for candidate constructor
selection (zero for base). Raw final samples:
`docs/performance/evaluator-identity-tail-measurements.csv`.
