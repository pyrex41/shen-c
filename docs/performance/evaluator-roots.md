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
