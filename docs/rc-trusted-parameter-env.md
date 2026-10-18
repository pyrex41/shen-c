# Trusted interpreted parameter frames

This candidate removes repeated binding validation when an interpreted closure
constructs its parameter environment. The public `rc_env_frame` API retains all
of its context, parent, array, symbol, value, and overflow checks in their
original order. Both paths use one allocator and initialization loop.

## Private contract

Only `dynamic_call` uses the private `rc_eval_env_frame` entry. `make_function`
already validated the immutable flattened captures: body, parent environment,
and same-context interned parameter symbols. Runtime application validates
argument contexts before callback entry, including arguments retained by partial
closures. The helper checks the active closure's context, native callback,
original arity, and exact capture count before borrowing these captures.

The active runtime call roots the closure through callback return, including
self-redefinition followed by collection. The new environment retains its
parent and argument values through the same construction loop as the public
API; body is borrowed from the active closure. Pool initialization, key-class
bits, collector edges, and graph-slot accounting are unchanged. Both paths
check `count <= SIZE_MAX / 2 / sizeof(RcValue *)` before computing packed storage.
Zero parameters retain the captured parent as before.

The private entry is not a general constructor for caller-supplied captures or
unchecked arguments. It is declared only in `rc_internal.h`; the public API has
no new entry point or weaker validation.

## Focused validation

Independent source review was clear. Fatal AddressSanitizer and
UndefinedBehaviorSanitizer runs passed `test_env`, `test_eval`, `test_cycles`,
`test_apply_fast`, and `test_callee`, with both default immediate integers and
`SHEN_RC_IMMEDIATE_INTEGERS=0` (10 executable runs). LeakSanitizer was disabled
on Darwin.

Coverage includes zero, one, eight, nine, and 32 parameters; invalid and duplicate
parameters; partial and overapplication; escaping scopes; retained old closures;
self-redefinition and collection during an owned callback; and cycles through
captured environments. New checks reject private entry calls with foreign
context, wrong callback, wrong arity, wrong capture count, missing argument
array, or missing body output, and confirm public non-symbol keys are rejected.

## Bounded measurement

Baseline is `3b055330971f1bfb83f6d597db9542564ae0f032`. Both variants used the
same updated `test/rc/bench_flat_params.c` driver, Apple Clang, `-O3 -flto=thin`,
and the Command Line Tools macOS SDK. Each case ran serially in ABBA order,
repeated twice (64 samples total). Values below are medians. CPU seconds come
from `CLOCK_PROCESS_CPUTIME_ID`; instructions come from `/usr/bin/time -l`.

| Case | Iterations | Baseline CPU s | Candidate CPU s | Baseline instructions | Candidate instructions | Managed allocations, both | Capture edges, both |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Call, 2 parameters | 80,000 | 0.0066825 | 0.0069165 | 98,628,409 | 95,173,655.5 | 80,000 | 0 |
| Call, 8 parameters | 80,000 | 0.0113975 | 0.0104355 | 147,889,883.5 | 133,270,711.5 | 80,000 | 0 |
| Call, 9 parameters | 80,000 | 0.0195290 | 0.0194280 | 236,789,293.5 | 222,281,563 | 160,000 | 0 |
| Call, 32 parameters | 80,000 | 0.0470350 | 0.0399785 | 482,814,488.5 | 432,362,049 | 160,000 | 0 |
| Create lambda, 1 parameter | 40,000 | 0.0047255 | 0.0047080 | 68,306,379.5 | 68,014,973.5 | 80,000 | 120,000 |
| Create defun, 16 parameters | 40,000 | 0.0209995 | 0.0222215 | 237,997,729 | 239,567,617 | 40,000 | 720,000 |
| Public ENV, 2 bindings | 80,000 | 0.0049940 | 0.0048795 | 73,770,122 | 73,867,616.5 | 80,000 | 0 |
| Public ENV, 32 bindings | 80,000 | 0.0249495 | 0.0232750 | 260,687,787 | 260,732,274.5 | 80,000 | 0 |

Interpreted call instructions decreased by 3.5%, 9.9%, 6.1%, and 10.4% at
arities 2, 8, 9, and 32. Creation and public-constructor instruction controls
were within 0.7%. Managed allocations, capture edges, and peak object counts
were identical for each paired case. CPU results are mixed at these short
durations; no full-suite speed claim follows from this microbenchmark.

Local evidence is retained under `evidence/rc/trusted-env/`: `builds.json`
records exact commands, baseline commit, source and driver hashes, and binary
hashes; `micro.json` and `run_micro.py` retain all samples and runner; individual
`.out`/`.err` files preserve raw results. `asan.json`, `boxed.json`, and per-test
logs preserve focused validation. No kernel build or full-suite run was made
for this candidate.
