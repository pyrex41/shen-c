# Flattened interpreted parameter captures

Interpreted closures capture `[body, environment, key0, ..., keyN-1]`. Parameter
validation and flattening happen once during closure creation. Calls pass the
contiguous key span directly to the immutable environment constructor, removing
the parameter list walk, temporary key copy, and the per-call temporary array
allocation above eight arguments.

The borrowed span is private to the runtime/evaluator implementation. Its
accessor validates the context, exact native callback, original arity, and exact
capture count before returning storage. Runtime dispatch owns the active closure
until callback return, so redefinition and collection cannot invalidate its body,
environment, or keys. Captures remain ordinary strong collector edges. Partial
closures copy the same captures and receive the complete original argument list.
No AST metadata or mutation invalidation is introduced.

Fatal ASan/UBSan checks pass in both immediate and boxed configurations for the
environment, evaluator, cycle, and exact-call tests. Extended environment checks
cover zero, one, eight, nine, and 32 parameters; invalid/duplicate parameter
diagnostics; partial and excess applications; escaping scopes; collection after
self redefinition through an owned-argument call with a borrowed callee; retained
old closures; and repeated packed-frame capture cycles. Private span guards also
reject foreign contexts, wrong callback/arity/count, and overflowing arity.

## Bounded measurements and creation tradeoff

The baseline is `574f7d6`. Both executables use Apple Clang, O3, and ThinLTO.
Each case ran in baseline/candidate/candidate/baseline order, repeated twice.
Values below are the four-sample medians for each executable. Loop wall and CPU
time exclude parsing/bootstrap; hardware instructions cover the complete process.
The raw process CPU user/sys counters are also retained, with their coarse
centisecond precision. These short controls do not qualify suite performance.

| Case | Iterations | Wall ms, before → after | CPU ms, before → after | Instructions M, before → after | Managed allocations, both | Capture edges, before → after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Call, arity 2 | 80,000 | 7.967 → 7.874 | 7.7505 → 7.5140 | 102.914 → 99.751 | 80,000 | 0 → 0 |
| Call, arity 8 | 80,000 | 14.474 → 13.764 | 13.6495 → 12.4875 | 157.198 → 147.620 | 80,000 | 0 → 0 |
| Call, arity 9 | 80,000 | 25.504 → 23.6355 | 23.9480 → 21.5740 | 264.967 → 237.760 | 160,000 | 0 → 0 |
| Call, arity 32 | 80,000 | 63.021 → 53.518 | 56.3905 → 49.0990 | 533.722 → 483.691 | 160,000 | 0 → 0 |
| Create, lambda arity 1 | 40,000 | 4.9565 → 5.4485 | 4.9065 → 5.2205 | 66.771 → 67.485 | 80,000 | 120,000 → 120,000 |
| Create, defun arity 16 | 40,000 | 18.5985 → 24.1995 | 16.9205 → 22.5575 | 181.714 → 237.769 | 40,000 | 120,000 → 720,000 |

High-arity creation regresses: every key becomes an ordinary strong capture, and
creation above eight parameters uses an additional temporary capture array.
The arity-16 creation control retires about 31% more instructions and takes about
30% more wall time. The single-parameter creation control also has a small
instruction increase. Repeated calls retire fewer instructions; whether this
tradeoff improves the complete suite remains unmeasured.

Reproduction driver: `test/rc/bench_flat_params.c`. Local evidence is retained
under `evidence/rc/flat-params`: exact build commands, source/binary hashes,
`micro-timed.json`, all 48 stdout/stderr pairs, sanitizer logs, and the serial
runner. LeakSanitizer is disabled on Darwin. No complete kernel was built here.
