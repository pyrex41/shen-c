# Compiled-path ownership overhead (issue #7)

This branch takes the four candidates from
[issue #7](https://github.com/pyrex41/shen-c/issues/7) in order, one commit per
item. Each commit passes the 136/0 Shen 43.1 suite, the runtime tests, the
shell differential tests and the sanitizer lanes listed below.

**Ownership operations in one complete suite run fall by 56%, from 1.18 billion
to 0.51 billion. Peak RSS falls by about 13%.** The timings were taken on a
loaded host, so the CPU and wall-time figures are UNKNOWN. They need to be
requalified on a quiet host before anyone claims a speedup.

Base: `a2c4d7e` (origin/main, Shen 43.1, v0.3.0+), built with Apple clang 21
(`make CC=clang`, `-O3 -g -flto=thin`) on Darwin arm64.

## Typed ownership counts

The counts come from `scripts/build-rc-counter-control.py --typed`, run once per
checkpoint through `scripts/benchmark-rc.py`. Each run used a fresh writable
suite copy and passed 136/0. Each cell is retain plus release operations that
actually changed a count. Since item 1, immortal values are not counted.

| Type | base | 1 symbols | 2 counters | 3 closures | 4 header |
|---|---:|---:|---:|---:|---:|
| number | 2,504,325 | 2,504,325 | 2,504,325 | 2,504,325 | 2,504,325 |
| string | 6,426,632 | 6,426,632 | 6,426,632 | 6,426,632 | 6,426,632 |
| symbol | 242,776,081 | 0 | 0 | 0 | 0 |
| pair | 239,123,036 | 239,123,036 | 239,123,036 | 239,123,036 | 239,123,036 |
| vector | 144,093,323 | 144,093,323 | 144,093,323 | 144,093,323 | 144,093,323 |
| closure | 464,216,608 | 464,216,608 | 464,216,608 | 43,359,721 | 43,359,721 |
| stream | 152,005 | 152,005 | 152,005 | 152,005 | 152,005 |
| tail request | 41,151,799 | 41,151,799 | 41,151,799 | 41,151,799 | 41,151,799 |
| environment | 36,692,542 | 36,692,542 | 36,692,542 | 36,692,542 | 36,692,542 |
| **total** | **1,177,136,351** | 934,360,270 | 934,360,270 | 513,503,383 | **513,503,383** |
| boolean subset | 89,246,450 | 0 | 0 | 0 | 0 |

The base totals differ from the 1.52 billion in the
[comparative profile](rc-comparative-profile-20261017.md). That profile measured
an earlier checkpoint, before the prepared evaluator landed. Every comparison
here is against the frozen `a2c4d7e` build.

A diagnostic experiment for item 3 left the primitive installer unpinned and
pinned only the generated top-level definitions. It counted 242,662,118 closure
operations, so pinned builtins account for about 199 million of the 421 million
removed.

## Items

### 1. Immortal interned symbols

The intern table already roots every symbol until context teardown, so a
symbol's count never decided its lifetime. Symbols now get the immortal
sentinel when they are interned, the same `SIZE_MAX` that nil already uses.
`rc_retain` and `rc_release` skip immortal values with one test, before any
statistics write. The emitter loads symbol literals straight from the module
table, with no retain.

The collector is unchanged. An immortal object's trial count can never reach
zero, so it is always a root. Teardown still frees symbols through the live
list. `rc_refcount` reports `SIZE_MAX` for symbols, as `rc.h` already promised
for immortal values.

### 2. Counters only in diagnostic builds

`retain_calls` and `release_calls` are compiled only when
`SHEN_RC_STATS_COUNTERS=1`. In Make that is `RC_STATS_COUNTERS=1`, and in CMake
`-DSHEN_RC_STATS_COUNTERS=ON`. Release builds report both fields as zero, and
`rc_stats_counts_ownership()` tells callers which kind of build they have.

The tests that assert exact deltas consult that function, so they still check
something in the release lane. `make counters` (new, and added as a CI step) and
`make sanitize` run every runtime test against a counting runtime.
`test_ownership.sh` builds its measured applications with the define.

### 3. Named calls without closure counting

`rc_closure_static` makes an unbound closure immortal when all of its captures
are immortal. It applies only where a context builds the closure at most once:

- generated top-level definitions in optimized mode. A top-level body runs once
  per bootstrap and has no name to recurse through.
- the primitive installer. Opcode captures are immediate, so boxed builds and
  the clock primitive keep counted closures.
- the numeric hashkey wrapper and `eval-kl`.

Definitions nested in function bodies, interpreted functions, lambdas, and
partial or bound closures keep the counted path. Immortal closures are shared
through a per-context set keyed by callbacks, arity, identity certificate and
captures. Reinstalling the same builtins therefore returns the existing objects,
and storage stays bounded by the number of distinct code and capture
combinations.

**Function cell.** The issue proposed a separate per-symbol cell holding the
native entry, owned entry and arity. Once the bound closure is immortal, the
symbol's existing function slot already behaves like that cell. Those three
fields are immutable fields of an object that lives until teardown, so a named
call's retain and release become one-test no-ops. The `self` passed to a body
also stays valid after a callback redefines the name, which the self-loop check
`t[fn]==self` and the guarded assoc and Prolog paths depend on. A raw
code-pointer cell would still need a `self` that cannot be freed or have its
address reused. A duplicate cached entry would save at most one dependent load
per call, and every `rc_function_set` would have to keep it in step. It is left
as a measured follow-up rather than a second source of truth.

**Observable changes.** `rc_refcount` reports `SIZE_MAX` for these closures. A
displaced top-level definition stays allocated until teardown. Reinstalling
identical builtins keeps the same canonical closure, so
`test_generic_builtin_tag` checks displacement only where builtins are mortal
(boxed builds).

New tests:
- `test_callee` covers immortality, sharing, mortal and immortal captures,
  partial applications, and a body that rebinds its own name, collects, then
  uses `self` and tail-calls the name. It passes under ASan.
- `test_ownership.sh` covers compiled top-level definitions that are replaced
  while their old body still runs.

### 4. Header 128 → 112 bytes (safe subset)

Before:

| Field | Bytes |
|---|---:|
| `type` (enum) and padding | 8 |
| `refs` | 8 |
| `bytes` | 8 |
| `trial`/`work` | 8 |
| `visit_epoch` | 8 |
| five flag bytes and padding | 8 |
| `ctx` | 8 |
| `prev`/`next` | 16 |
| payload union (closure is the widest) | 56 |
| **total** | **128** |

After: `refs` is 32 bits and shares one word with the `type` byte, the hot
`marked`/`reaches_vector` bytes, and a byte holding the closure-only
`builtin_slot` (7 bits) and `returns_second` (1 bit). `cycle_source` applies
only to vectors, so it moved into the vector payload. The union does not grow.

The 32-bit immortal sentinel is `UINT32_MAX`. A counted object that reaches
2^32-1 owners saturates into an immortal: it is freed at teardown and never
early. The collector seeds immortal trial counts with `SIZE_MAX`, so the number
of incoming edges cannot exhaust them. Both macOS's 16-byte quanta and glibc's
chunk rounding turn 112 bytes into an allocation one size class smaller.

A first variant packed every flag as a bitfield. It measured +1.1% instructions
and +1.7% cycles against item 3, from read-modify-write traffic on
`reaches_vector` and `marked`, and was rejected. The committed byte-flag variant
is the `item4b` binary below.

**What further shrinking needs.** These numbers are design arithmetic only;
none of it is implemented or measured.
- Dropping `prev`/`next` (16 bytes) and the per-object `trial`/`work` and
  `visit_epoch` fields (16 bytes) requires the candidate-based collector the
  issue describes. Trial deletion would start from cycle-source vectors, and
  bounded-walk epochs would move to a side table. That would give about
  80 bytes.
- Deriving `ctx` from the allocation arena (8 bytes) and `bytes` from the type
  and payload lengths (8 bytes) would give about 64 bytes.
- Storing pair, vector and closure slots inline would go further, towards
  shen-llvm's 24-byte `LlObj` header.

The collector rewrite is out of scope for this PR.

## Timing (loaded host, CPU and wall UNKNOWN)

Each table shows alternating repetitions through `scripts/benchmark-rc.py`, with
the order reversed every repetition. Every run passed 136/0 in a fresh suite
copy. Other agents shared the 10-core host throughout, with 1-minute load
averages between 9.8 and 17.4. The ratios are medians of per-repetition
ratios.

Run A: five repetitions, load 10.94 → 11.63.

| Build | user s | instructions vs base | peak RSS vs base |
|---|---:|---:|---:|
| base | 4.300 | 1.0000 | 1.0000 (35,045,376 B) |
| 1 symbols | 4.330 | .9456 | 1.0014 |
| 2 counters | 4.200 | .9118 | .9584 |
| 3 closures | 4.420 | .8985 | .9561 |

Run B: six repetitions, load 12.55 → 10.43.

| Build | user s | user vs base | instructions vs base | peak RSS vs base |
|---|---:|---:|---:|---:|
| base | 3.910 | 1.0000 | 1.0000 | 1.0000 (35,086,336 B) |
| 3 closures | 3.740 | .9588 (6/6 wins) | .8983 | .9489 |
| 4, bitfield variant (rejected) | 3.855 | .9782 | .9079 | .8725 |

Run C: six repetitions, load 9.94 → 9.77, compared with item 3.

| Build | user vs item 3 | instructions | cycles | peak RSS |
|---|---:|---:|---:|---:|
| 4, bitfield variant (rejected) | 1.0176 | 1.0106 | — | .9137 |
| 4, byte flags (committed) | .9908 | 1.0024 | .9988 | .9153 |

Run D: the final branch head against base, eight repetitions, load
9.35 → 7.60.

| Metric | head / base, median of per-repetition ratios | range |
|---|---:|---|
| user CPU | .9743 (8/8 wins) | .935 to .987 |
| wall | .9758 | .871 to .987 |
| cycles | .9751 | .955 to .987 |
| instructions | .9004 | .900 to .901 |
| peak RSS | .8708 | .869 to .873 |

The largest peak RSS was 30,523,392 B on the head and 35,110,912 B on base.

Instruction counts and peak RSS are much more repeatable than CPU across
repetitions: the instruction spread is under 0.1% and the RSS spread under 1%.
User and wall time varied by several percent between repetitions because of
load. The parent session will requalify CPU on a quiet host using the
ten-pair protocol in [rc-43.1-final-20261017.md](rc-43.1-final-20261017.md).

## Gates run for every item

Each item ran these locally on Darwin arm64 with Apple clang 21:

```sh
make CC=clang -j2 && make CC=clang test
make CC=clang counters                       # items 2 to 4
scripts/test-rc.sh --no-build bin/rc-build
test/rc/test_reader_capture.sh ./bin/shen-rc
CC=clang test/rc/test_source_compile.sh
CC=clang test/rc/test_source_metadata.sh
CC=clang scripts/certify-rc.sh               # 136 passed, 0 failed, 36 reports
make CC=clang sanitize                       # fatal ASan/UBSan runtime tests
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make -f Makefile.rc CC=clang RC_SANITIZE=1 RC_CPPFLAGS=-DSHEN_RC_IMMEDIATE_INTEGERS=0 RC_BUILD=obj/boxed RC_BIN=bin/boxed test
CC=clang CFLAGS='-O1 -g -DSHEN_RC_VERIFY_OWNERSHIP=1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 scripts/certify-rc.sh
make CC=clang acyclic                        # item 4
```

All passed. At the final head (`ac31fd8` before this note was amended in)
these also passed: `scripts/certify-rc.sh` (receipt revision `ac31fd8`, 136/0),
`scripts/test-rc-toolchain.sh --canonical-receipt` on that receipt, and CMake
Release plus `-DSHEN_RC_STATS_COUNTERS=ON` builds with `ctest` (25/25 each).
`rc.c` also syntax-checks for i386 and armv7 targets. Nix builds and Linux
gcc/clang were not run locally; Linux/macOS CI on the PR is authoritative for
them.
