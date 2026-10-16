# Canonical arithmetic and comparison entry points

The bounded canonical identity table now covers the eight binary arithmetic and
comparison builtins alongside the six original tiny entry points. Each new body
calls the existing `numeric` implementation with a constant opcode, allowing the
compiler to remove opcode dispatch. No arithmetic, floating-point, overflow, or
error ordering implementation is duplicated or changed.

The same canonical pointer guard, exact two-argument ownership contract, and
ordinary dispatch fallback apply. Partial and replaced closures never acquire
canonical identity. Old owned snapshots after builtin reinstallation remain
callable through the fallback. The fixed context table grows from six to
fourteen roots (48 to 112 bytes on 64-bit hosts), without growing managed values.

## Validation

All 16 runtime executables pass fatal AddressSanitizer and UndefinedBehaviorSanitizer
with default immediate integers and boxed integers. The new numeric differential
compares ordinary application with all eight tiny entry points over 144 ordered
operand pairs, in ordinary and tail position: 2304 cases. Operands include exact
integers, both signed limits, zero, negative one, inexact numbers, negative zero,
NaN, both infinities, and invalid strings. Results compare types, exactness,
integer bits, floating equality/NaN/signed zero, and error messages/order.

Additional cases cover foreign/null inputs, pending errors, foreign functions,
replacements, canonical functions for other operations, partially bound callees,
and old snapshots across reinstallation. Tail-position replacements are verified
as stack-safe requests and driven through an ordinary caller for result comparison.

The generated test now checks all fourteen specialized APIs and 22 differential
results under fatal sanitizers, including arithmetic redefinition during argument
evaluation and a lexical arithmetic shadow. No full kernel build was performed.

## Instruction measurements

Baseline is `3aa882f8a466cd228d59b7262428b9971f65150c`. Both binaries used Apple
clang 21.0.0 on arm64 Darwin, `-O2 -g -std=c17`, no LTO or sanitizer. Each operation
ran 3,000,000 calls twice, reversing before/after and operation order in the
second pass. `bench_canonical_numeric.c` uses exact integer operands 6 and 3.
Raw counters are in `canonical-numeric-measurements.json`.

| Operation | Before instructions/call | Tiny API | Reduction |
|---|---:|---:|---:|
| `+` | 329.14 | 256.61 | 22.04% |
| `-` | 328.82 | 256.17 | 22.09% |
| `*` | 331.06 | 258.08 | 22.05% |
| `/` | 332.94 | 261.18 | 21.56% |
| `>` | 356.22 | 283.90 | 20.30% |
| `<` | 356.43 | 283.97 | 20.33% |
| `>=` | 356.23 | 283.96 | 20.29% |
| `<=` | 355.79 | 283.36 | 20.36% |
| Unmodified generic `hd` control | 277.184 | 274.871 | 0.83% |

Allocation and retain/release counts match for every operation. The generic `hd`
control compiles the identical generic benchmark caller against both runtimes,
excluding caller selection/loop-layout differences. Counts include startup and
teardown, negligible at this denominator. The small control movement is separate
from the target-operation savings. Timing was noisy and is not used to claim an
end-to-end speedup.

Example candidate build and bounded run:

```sh
clang -O2 -g -std=c17 -DRC_BENCH_NUMERIC=1 -Isrc/rc \
  test/rc/bench_canonical_numeric.c src/rc/rc.c src/rc/primitives.c \
  src/rc/eval.c src/rc/read.c -lm -o /tmp/bench-canonical-numeric
/usr/bin/time -l /tmp/bench-canonical-numeric 0 3000000
```
