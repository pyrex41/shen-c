# Cheaper `=` (Track A2)

## Change

- **Inline fast path** (`src/rc/rc_layout.h`, used by generated code). The
  path already covered two immediates, symbols or []. It now also decides
  when one operand is a symbol or [] belonging to this context and the other
  is not a pending tail request.
  - Such a value is equal only to itself: other symbols have other names,
    and no other type compares equal to a symbol or [].
  - So `(= X sym)` with a list, string or number `X` no longer reaches
    `rc_equal`.
  - Operands from another context, and pending tail requests, still take the
    checked path.
- **`graph_equal`** (`src/rc/rc.c`, reached when a compared structure may
  contain vectors). It deduplicates its first 32 pairs with a linear scan,
  and allocates its hash table only when the worklist grows past them. Every
  call used to zero a 512-byte table on the stack.
- **Tests.** `test/rc/test_condition_emit.sh` adds 14 cases of `=` against
  symbols and [], including strings with the same text, numbers and
  booleans, in optimised and conservative modes.
- `test_equal` already covers vector cycles and growth past 32 pairs.

## Measurements

Instructions retired, from `/usr/bin/time -l`, comparing main 2cd9741 with
this branch (5d10439).

- **Runs.** Two interleaved rounds with load-time compile off
  (`SHEN_RC_EXECUTION=interpret`, `SHEN_RC_NATIVE_CACHE=off`). The table
  shows their mean.
- **Workloads.** `big` is cvs's `btyped-x10`.
- **Outputs.** Every run produced the reference payload or output.

| Workload | main 2cd9741 instructions | A2 instructions | Change | main 2cd9741 peak RSS | A2 peak RSS |
|---|---|---|---|---|---|
| suite | 98.18G | 96.45G | -1.8% | 20 MB | 21 MB |
| mb-terms | 208.39G | 203.67G | -2.3% | 11 MB | 11 MB |
| mb-defs | 256.14G | 250.32G | -2.3% | 24 MB | 23 MB |
| a2-libs-typed | 111.73G | 104.24G | -6.7% | 29 MB | 29 MB |
| b-typed | 688.91G | 679.82G | -1.3% | 454 MB | 455 MB |
| c-untyped | 161.60G | 159.92G | -1.0% | 32 MB | 32 MB |
| d-g5 | 383.13G | 366.56G | -4.3% | 39 MB | 39 MB |
| big | 6902.89G | 6810.24G | -1.3% | 502 MB | 518 MB |

The generated kernel C is unchanged (the fast path is in a header). The
single-job clang compile time of `kernel.o` was 71.5 s user, against
56.1 s at A1. Load average was 23 to 30 during both runs, and the source is
the same size, so the difference is load, not code.
