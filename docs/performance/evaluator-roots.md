# Evaluator lexical filter and root ownership

Base: `8dbc9414a402a148dee0e5c0bde6fd087c7e9fce`. No full-kernel build or
full-language-suite benchmark ran in this lane.

## Immutable environment key classes

Each environment carries the OR of its parent and newly bound symbol classes:
ASCII uppercase initial, ASCII lowercase initial, or other. Empty names,
punctuation, and non-ASCII names all use the other class. An absent class proves
lexical lookup cannot succeed; a present class uses the existing identity walk.
This preserves lowercase special-form/function shadows and all binder spellings.

All environment context, key, value, and size validation remains in place. Every
new or pooled frame initializes its class byte; zero-binding construction retains
the original parent. The extra byte increases the env member from 48 to 56 bytes,
within the existing 56-byte closure union. The static env<=closure assertion and
object-cache test confirm the complete value block remains 136 bytes.

Independent source review cleared this change. Fatal ASan/UBSan passes for the
managed-environment and evaluator tests in default and boxed modes. Added cases
cover A/Z/a/z, punctuation, empty and UTF-8 names, inherited classes, shadowing,
512-deep frames, pooled-frame reset, and lowercase if/+ shadows.

### Focused instruction measurements

Apple CLT clang on Darwin arm64, `-std=c17 -O3 -flto=thin`; actual base versus
candidate runtime. Three serial runs each in alternating order; medians from
`/usr/bin/time -l`:

| Workload | Base instructions | Filter instructions | Change |
| --- | ---: | ---: | ---: |
| 128 uppercase frames, 2M lowercase negative lookups | 4,659,274,623 | 14,631,524 | -99.69% |
| 128 lowercase frames, 2M same-class negative lookups | 4,656,526,838 | 4,658,350,231 | +0.04% |
| Interpreted 16-let loop, uppercase binders, 30K calls | 1,373,686,863 | 1,098,343,859 | -20.04% |
| Same interpreted loop, all lowercase binders | 1,374,181,696 | 1,427,038,554 | +3.85% |

The lowercase interpreted control pays classification and metadata overhead
without rejecting its function-head lookup walks. Retains (6,180,639), releases
(6,720,510), and managed allocations (540,374) remain identical in both interpreted
cases. These gains and regressions are scoped measurements, not full-program
results.

Reproduce with `test/rc/bench_eval.c`, linking separately against each runtime;
arguments are `lookup-upper 2000000`, `lookup-lower 2000000`, `eval-upper 30000`,
and `eval-lower 30000`.

## Borrow internal evaluation roots

The public `rc_eval` boundary validates context exactly as before and retains a
pair input once through the full call. Atomic inputs keep their existing owned
result path. Dynamic calls keep their body strongly rooted through the active
closure and own their parameter environment until evaluation returns.

Internal recursive evaluation borrows the same immutable AST graph and incoming
environment. Every loop transition selects a descendant of that graph. Legal pair
reuse cannot mutate an AST pair while its rooted parent or active closure still
owns it. Vectors remain shared values; no mutable data is copied or memoized.

A let-created environment is locally owned, tracked by one flag. Its constructor
acquires the parent edge before the previous local owner is released. Incoming
borrowed environments are never released here. Atomic results, quotes, and
returned closure captures acquire ownership before local environment cleanup.
Function snapshots, argument order, special-form shadows, tail guards, error
messages, and public context checks remain unchanged.

Independent source review cleared this ownership proof and actual implementation.
Fatal ASan/UBSan evaluator and managed-environment suites passed in default and
boxed modes, including existing cycle reclamation and repeated error cleanup.
New tests remove the sole external AST owner inside a collecting callback, on
both successful and trapped paths; replace the executing dynamic function then
collect and continue its old body; and apply an escaped let closure after the
public AST root has been released.

### Incremental instruction measurements

The same balanced three-run method compares actual base, filter-only, and filter
plus internal borrowing executables:

| Interpreted 16-let loop, 30K calls | Base | Filter only | Filter + borrowing |
| --- | ---: | ---: | ---: |
| Uppercase binders | 1,374,807,007 | 1,097,178,573 | 1,011,295,590 |
| Lowercase binders | 1,371,585,504 | 1,424,312,213 | 1,339,600,810 |

Internal borrowing reduces instructions by a further 7.83% and 5.95% respectively.
Combined reductions against the base are 26.44% and 2.33%. Retains fall from
6,180,639 to 3,090,633 and releases from 6,720,510 to 3,630,504; managed allocations
remain 540,374 in both cases. These focused results do not establish integrated
language-suite performance.
