# Borrowed native request argument spans

`apply_request` passes the request's existing argument span directly to an
unbound borrowed-native callback. It previously copied the arguments into an
eight-slot stack array or, above eight arguments, a separate malloc array.
Owned-native callbacks and bound/partial closures keep their existing paths.

## Ownership invariant

The executor owns the request until callback return. Its function and argument
edges therefore remain roots during collection and function redefinition. A
symbol target also has the existing owned `resolved` reference through callback
return. The public borrowed callback type makes argument slots const; no edge
is stolen or changed. Shared requests remain executable repeatedly.

Before callback dispatch, overapplication has already retained the suffix in
its pending request. The callback receives only the original closure arity,
even when its array contains a larger backing span. Zero-argument callbacks
may receive NULL, as allowed by the existing argc-zero interface. The span's
lifetime ends at callback return, as with the prior stack/malloc slot array.

Target resolution, overflow validation, boolean continuations, errors,
callback result ownership and safepoint placement are unchanged. Runtime
allocation, reference, collection and cache counters retain their semantics;
the removed temporary malloc array was unmanaged bookkeeping.

## Focused validation

Base: `3b055330971f1bfb83f6d597db9542564ae0f032`. Fatal AddressSanitizer and
UndefinedBehaviorSanitizer pass in default immediate-integer and explicit
`SHEN_RC_IMMEDIATE_INTEGERS=0` builds for six runtime executables:
`test_apply_fast`, `test_bool_tail`, `test_cycles`, `test_collection`,
`test_primitive_fast`, and `test_callee` (12 executable runs).

The expanded existing apply test covers arities 0, 1, 2, 8, 9 and 32, repeated
shared requests, cyclic arguments with no owner beyond request edges, full
callback collection, a symbol callback replacing its own function/global
roots, overapplication, boolean continuations, and callback errors that also
return an owned result. Existing bound and owned callback tests remain active.
Independent source and test review found no blocker. No full kernel or language
suite was built in this lane.

## Bounded measurements

Both binaries use Apple clang 21, arm64 Darwin, `-O3 -flto=thin -std=c17`, normal
statistics/cache/collection, and no sanitizer. Baseline is the source commit
above; candidate changes only the request dispatch branch. Two passes reverse
both case and binary order. Each process makes one million callback calls;
`/usr/bin/time -l` supplies retired instructions for the entire process.

The fresh cases create and execute an ordinary request on every call. Shared
cases repeatedly execute an existing captured request, isolating request
dispatch. Bound and owned controls use a shared request and nine supplied
arguments; the bound callback has one additional pre-bound argument.

| Request mode | Arguments | Instruction change |
|---|---:|---:|
| Fresh | 1 | -3.13% |
| Fresh | 2 | -3.58% |
| Fresh | 8 | -3.55% |
| Fresh | 9 | -16.62% |
| Fresh | 32 | -11.83% |
| Shared | 1 | -6.11% |
| Shared | 2 | -7.56% |
| Shared | 8 | -12.81% |
| Shared | 9 | -45.10% |
| Shared | 32 | -53.19% |
| Bound control | 9 | +1.36% |
| Owned control | 9 | +0.67% |

Allocation/free, retain/release, and collection counts match exactly before and
after in every case. Short elapsed/CPU times are noisy; these results establish
a bounded instruction reduction for borrowed request calls, with small control
costs. Full language-suite benefit remains a separate gate. Raw runs, source
hashes, compiler identity and sanitizer exit codes are in
`borrowed-native-spans-measurements.json`.

Example release micro build/run (use the same flags for both revisions):

```sh
clang -O3 -flto=thin -std=c17 -Isrc/rc \
  test/rc/bench_borrowed_native_span.c src/rc/rc.c src/rc/primitives.c \
  src/rc/eval.c src/rc/read.c -lm -o /tmp/bench-native-span
/usr/bin/time -l /tmp/bench-native-span fresh 9 1000000
```
