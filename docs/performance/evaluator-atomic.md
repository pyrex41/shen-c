# Inline atomic interpreter arguments

Base: `3b055330971f1bfb83f6d597db9542564ae0f032`. This isolated candidate keeps
the original owned function lookup, snapshot and cleanup. It does not include
the separately preserved lazy-callee candidate (`0cfbd3e`) or lazy-plus-inline
follow-on (`1e902e8`). No full-kernel build or full-language-suite run was made.

## Change and correctness

Application checks the source argument type once. Pair arguments enter the
existing recursive evaluator. Other arguments use a shared inline helper with
the same NULL diagnostic, lexical symbol lookup, and retain of the lexical value
or literal. Both preexisting atomic paths in the evaluator use this same helper.
Every lookup/context check and argument order remains intact. Reading an atomic
source expression invokes no callbacks; its lexical value can still be any
managed type. Function lifetime, redefinition snapshots, traps and tail requests
retain their existing ownership behavior.

The stack argv array no longer initializes all eight slots. A complete call
assigns all argc entries before dispatch; failure cleanup reads only successfully
assigned entries. Heap calloc, overflow/error behavior and cleanup are unchanged.

Independent source review cleared the diff. Fatal ASan/UBSan tests passed for
evaluator, managed environments, boolean-tail continuations, and exact apply,
with default and boxed numbers (`SHEN_RC_IMMEDIATE_INTEGERS=0`): eight executables.
New evaluator cases cover first/later argument redefinition, lexical symbol aliases
and computed heads, a native callback replacing its only binding then collecting
before reading old captures, pending-tail ownership, boolean guards, and argument
errors before/after populated stack slots. Existing tests exercise heap arguments,
undefined-function-before-effects ordering, escaped closures and cycle cleanup.

## Balanced instruction and CPU measurements

Apple CLT clang, Darwin arm64, `-std=c17 -O3 -flto=thin`. Three separately linked
executables: exact base; inline-only candidate; lazy-plus-inline. Three serial
runs each in forward/reverse/rotated order; medians from `/usr/bin/time -l`.

| Workload | Base instructions | Inline only | Change | Lazy + inline change | Base/inline CPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| Atomic primitive, 5M calls | 2,547,237,263 | 2,466,191,858 | -3.18% | -5.14% | 0.22 / 0.21 s |
| Effectful argument, 5M calls | 6,249,220,608 | 6,329,797,753 | +1.29% | +3.53% | 0.54 / 0.53 s |
| Lexical head, 5M calls | 5,248,865,437 | 5,190,082,244 | -1.12% | +0.30% | 0.45 / 0.43 s |
| Computed head, 5M calls | 3,387,289,942 | 3,391,524,395 | +0.13% | +2.20% | 0.29 / 0.29 s |
| Native user callback, 5M calls | 3,356,585,962 | 3,277,686,275 | -2.35% | -6.06% | 0.28 / 0.28 s |
| Upper-case interpreted loop, 300K calls | 9,936,877,617 | 9,800,342,792 | -1.37% | -2.56% | 1.00 / 0.96 s |
| Lower-case interpreted loop, 300K calls | 13,211,034,563 | 13,077,058,669 | -1.01% | -1.90% | 1.36 / 1.35 s |

Inline-only retains/releases/allocations match base in every case. Both interpreted
loops report 30,900,634 retains, 36,300,507 releases and 5,400,374 allocations.
Lazy-plus-inline removes 10,200,003 retain/release pairs from each interpreted
loop but carries larger effectful/computed control regressions. These are scoped
measurements; the remaining +1.29% effectful instruction regression and noisy CPU
timing remain explicit. Integrated performance is unmeasured.

Reproduce with `test/rc/bench_eval_callee.c` modes atomic, effect, lexical, computed
and user (5,000,000 iterations), plus `test/rc/bench_eval.c` eval-upper/eval-lower
(300,000 iterations), linking separately against the three source revisions.
Raw runs: `docs/performance/evaluator-atomic-measurements.csv`.
