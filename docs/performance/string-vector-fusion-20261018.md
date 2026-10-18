# String and vector fusion exploration, 2026-10-18

**Long string scans have a removable copying problem. Mutable vectors need a
different approach. Neither prototype establishes a new complete-suite win.**
Work is isolated on `perf/string-vector-fusion`, based on `802e724`.
The pinned 43.1 kernel, Shen sources and 136-test suite are unchanged.

| Checkpoint | Implementation | Decision |
|---|---|---|
| `0753780` | Diagnostic counters, source probes, semantic controls | Measurement baseline |
| `ae24940` (A) | Allocate `cn` directly in its final buffer | Keep as a reviewable primitive improvement; suite timing is flat |
| `770c39b` (B) | Bounded immutable suffix views | Keep **off by default**, enabled only by `SHEN_RC_STRING_SLICES=1` |

This is exploration after v0.3.0, separate from its
[accepted RC/Boehm qualification](rc-43.1-final-20261017.md). These comparisons
are between RC implementations. They do not repeat the release acceptance gate.

## What upstream actually does

In [sys.shen](../../shen/src/shen/sys.shen), `@s` is `cn`. String patterns use
`shen.+string?`, `hdstr` and `tlstr`; a recursive scan repeatedly copies the
remaining suffix. The `process-@s` macro also expands a multi-character literal
prefix into one-character constructors, creating opportunities for a builder.
Concrete lowered forms are retained in the evidence as `lowered-forms.txt.gz`.

`@v` allocates a larger vector and copies the old elements. `tlv` allocates a
shorter vector and copies its tail. Its current source additionally evaluates
an unused `NewVector` initializer before allocating the vector it returns.
Dropping that initializer requires a guard: `vector` can be redefined with
observable effects. Pattern matching also adds small vector operations.

Vectors have fresh-copy behavior that must survive optimization. This control
returns **`[20 99 20 77]`**:

```shen
(let V (vector 2)
     A (vector-> V 1 10)
     B (vector-> V 2 20)
     T (tlv V)
     R (@v (hdv V) T)
     C (vector-> V 2 99)
     Snapshot (<-vector R 2)
     D (vector-> R 2 77)
     [Snapshot (<-vector V 2) (<-vector T 1) (<-vector R 2)])
```

Returning `V` for the reconstruction, or sharing writable storage for `T`,
changes this result. Other controls preserve missing vector elements, string
aliases, embedded NUL bytes, dynamic function redefinition and error ordering.

## Two measured prototypes

A removes `cn`'s temporary allocation and second payload copy. It still creates
the same result string. It keeps ordinary callee lookup, argument evaluation,
ownership and failure paths. This is primitive copy elimination; it does not
fuse a chain of constructors or remove its intermediate string handles.

B makes a `tlstr` result share its immutable backing string when the suffix is
at least 64 bytes and at least one quarter of the backing string's length.
Otherwise it copies. Each view points directly to an owning string, avoiding
chains of view owners. The policy bounds the backing retained by an escaped
view to four times that view's length; independently live aliases still retain
their own storage. Suffixes preserve the existing NUL-terminated borrowed C
text contract, including length-aware operations on embedded NULs.

The owner is a real RC graph edge, included in traversal, cycle subtraction,
release and destruction. The prototype reuses String-only metadata fields and
keeps `sizeof(RcValue)` at 128 bytes. **Every suffix still allocates a string
handle.** This is Bruno's immutable suffix-sharing idea, with bounded retention,
rather than a compiler cursor or allocation-free traversal.

Diagnostic single-scan counts after four complete warmup calls:

| Input bytes | Baseline/A suffix bytes copied | B suffix bytes copied | B views |
|---:|---:|---:|---:|
| 128 | 8,128 | 2,016 | 64 |
| 512 | 130,816 | 2,143 | 447 |
| 2,048 | 2,096,128 | 2,654 | 1,982 |
| 8,192 | 33,550,336 | **4,701** | 8,125 |

The copy policy changes a shrinking walk from quadratic payload copying to
geometric copies plus the bounded short tail. It does not remove evaluator,
call-dispatch, retain/release or string-handle work.

Vector scanning is unchanged by both candidates:

| Input elements | Vectors allocated during scan | Cumulative slots allocated |
|---:|---:|---:|
| 16 | 47 | 287 |
| 64 | 191 | 4,223 |
| 256 | 767 | 66,047 |
| 1,024 | 3,071 | 1,050,623 |

These are cumulative allocation counters, not peak/live storage. This probe
allocates `3n−1` vectors and `n²+2n−1` slots, exposing the repeated-copy cost.

## Timing and suite relevance

Apple Clang 21, Darwin arm64, `-O3 -g -std=c17 -flto=thin`; B adds its opt-in
flag. All release variants link the same unchanged generated kernel/main
objects. Separately regenerated, fully instrumented kernels are checked below.
The exact source, generated C, binary, dependency, suite and input hashes are
bound by the published receipts. Baseline runtime sources are from `0753780`;
the probe driver uses the four-warmup version bound by its source hash.

Each microbenchmark uses five alternating pairs, independent output oracles,
and uninstrumented binaries. Its internal process-CPU interval includes user
plus system CPU, result checking and release; it excludes setup, source loading,
input allocation and warmup. Probe definitions are **interpreted/prepared KL
running on a native kernel**, matching ordinary loaded-source execution. The
separate emitted native control below covers that path too.

| Probe | A / baseline median paired CPU | B / A median paired CPU |
|---|---:|---:|
| `cn`, 16-byte operands | 0.803 | 0.951 |
| `cn`, 256-byte operands | 0.685 | 1.076 |
| `cn`, 4,096-byte operands | 0.740 | 1.172 |
| String scan, 2,048 bytes | 0.999 | 0.898 |
| String reconstruction, 256 bytes | 0.949 | 1.016 |
| String construction, 256 bytes | 0.902 | 0.995 |
| Vector scan, 256 elements | 0.997 | 1.030 |
| Vector construction, 256 elements | 1.029 | 0.996 |

Other Shen and .NET jobs were observed, especially during B measurements.
Unchanged controls vary substantially. These are retained exploratory samples,
**not quiet-host estimates or a claim that B speeds up unrelated operations**.
No build, profiler or diagnostic run from this session overlapped timing.

Each whole-suite batch also uses five alternating pairs with all ten runs
passing every intermediate report, final **136/0**, exact redirect output and
unchanged artifact/input checks:

| Median paired complete-process metric | A / baseline | B / A |
|---|---:|---:|
| User CPU | **0.9975** | **1.0024** |
| System CPU | 1.0000 | 1.0000 |
| Wall time | 1.0000 | 1.0690 |
| Peak RSS | 0.9933 | 1.0031 |
| Retired instructions | 0.9997 | 1.0003 |
| Cycles | 0.9951 | 1.0090 |

Separate distribution medians for A/baseline were 3.92/3.93 seconds user CPU,
4.01/4.02 seconds wall, and .02/.02 seconds system CPU. In the B/A batch they
were 4.57/4.51 user, 5.27/4.93 wall and .05/.05 system. Do not compare absolute
times across the two host windows. B's wall result would fail a 3% regression
limit, even though these samples do not qualify a quiet-host gate.

Separate diagnostic suite runs explain the limited effect. A removes
**2,030,698 temporary-copy bytes across 136,107 concatenations**. B shares only
**2,056 of 414,988 tails (0.495%)**, avoiding 388,183 suffix-copy bytes. Most
suite tails are short. All variants allocate 2,282,015 vectors and 5,128,075
slots; many are small Prolog cells, so those totals cannot be attributed to
`@v`/`tlv`. Printed floating-point timing strings vary in length, so total
string-copy counters need not be byte-identical across complete suite runs.

## Correctness and limits

| Check | Result |
|---|---|
| A runtime, fatal ASan/UBSan, default and boxed | 25/25 each |
| B views on, fatal ASan/UBSan, default and boxed | 26/26 each |
| Current source, views off, fatal ASan/UBSan | 26/26 |
| Full regenerated kernel, A, fatal ASan/UBSan + emitted ownership verification | 136/0; compiler/dependency audits pass |
| Full regenerated kernel, B views on, same instrumentation | 136/0; compiler/dependency audits pass |
| CMake, B views on, fatal ASan/UBSan | Both new tests pass; other CMake tests not rerun here |
| Native emitted suffix escape + vector/closure cycle | Two cycle objects reclaimed with 255-byte suffix rooted; exact bytes/NUL pass |
| Existing emitted controls, B views on, fatal ASan/UBSan + ownership verification | Semantics 87/87; ownership 20 differential cases; callee 18 differential cases; primitive and Prolog controls pass |
| New candidate Linux/Nix/remote CI qualification | UNKNOWN |

Tests cover aliases, borrowed/owned calls, partial application, errors, pooled
reuse, backing retention, closure escape and unreachable mixed cycles. The
native collector control uses `rc_collect` from its C host. An initial Shen
fixture used an undefined `gc`; its correction used `shen.gc`, but upstream
redefines that name as a two-argument Prolog helper, so a zero-argument call
returns a partial application. Both attempts and the final host-hook check
are retained. Native string printing also required a quoted-output oracle,
unlike the CLI's bare string display. No runtime defect was found in these
control corrections.

Earlier one-warmup probes crossed a preparation threshold in one small vector
case; they remain under `initial-warmup`. Final counts and timings use four
warmups. No samples from the final matched batches were discarded.

## Next candidate

**Start with fresh construction fusion.** For `(@v A (@v B Tail))`, prove that
the intermediate vectors are fresh and do not escape, allocate the final
vector once and copy `Tail` once. Keep the final storage independent from
`Tail`. Guard the actual kernel/helper bindings and vector header/storage
invariants; preserve missing elements, argument effects and error order.
Measure increasing chain lengths before spending effort on general analysis.

For strings, flatten a certified `@s`/`cn` construction chain into segments,
compute its final size and copy each segment once. Literal-prefix expansion is
a useful first shape. Preserve the original callee snapshots and argument/error
order; function names alone cannot justify a transformation. Unknown bindings,
effectful expressions or uncertain string types need the ordinary path.

Both candidates need prepared-evaluator support as well as native emission;
an emitter-only change would miss the loaded-source probes and much suite work.
They should eliminate intermediate handles, which the suffix prototype leaves.

Vector suffix cursors require a proof of no intervening mutation and no escaping
view, or a materialized copy at the boundary. Unique vector reuse could instead
use ownership plus a runtime uniqueness check, with a copying fallback. Merely
reallocating an exact-size buffer at each prepend remains quadratic: headroom,
geometric capacity or whole-chain preallocation would be needed. That adds
layout and cycle-accounting work and is deferred until a measured candidate
justifies it. Writable escaping tail views are not a drop-in optimization.

These proposals connect to [Perceus](https://www.microsoft.com/en-us/research/publication/perceus-garbage-free-reference-counting-with-reuse/)
through precise ownership and reuse at certified boundaries. [FP²](https://www.microsoft.com/en-us/research/publication/fp2-fully-in-place-functional-programming/)
provides a stronger checked in-place calculus, implemented in Koka. This branch
adds neither that checker nor its allocation/stack guarantees, and introduces
no portable P-lambda intermediate language.

## Evidence and reproduction

[Assessment](../evidence/string-vector-fusion-20261018/fusion-20261018/assessment.json),
[portable publication index](../evidence/string-vector-fusion-20261018/publication.json),
and [reproduction commands](../evidence/string-vector-fusion-20261018/fusion-20261018/reproduce.sh)
bind the raw outputs, source/binary freezes and qualification receipts. The
publication includes failed control attempts. Original binaries and generated
build trees remain local under `evidence/rc/fusion-20261018/`; publication is a
bounded, path-normalized text copy with both original and published hashes.

Run `python3 evidence/rc/fusion-20261018/assess.py` against the raw tree to
recompute comparisons and verify output hashes. Benchmarks reject diagnostic
binaries, changed dependencies/inputs, incomplete suites and incorrect outputs.
Use fresh output directories. The reproduction file records release builds,
separate diagnostic builds, fatal sanitizer checks and matched commands.

Decision: A is a small measured primitive improvement; complete-suite gain is
unproven. B demonstrates bounded suffix sharing and remains opt-in. General
string/vector fusion and platform qualification remain **OPEN**.
