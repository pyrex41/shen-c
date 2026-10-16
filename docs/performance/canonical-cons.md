# Canonical cons and reuse guards

Base: `e28bee990468334166765db8c1cf56bd0ea07d4e`. This standalone change
adds CONS at canonical slot 14, keeping all existing slot indices stable.

The tiny consuming constructor uses the same checked canonical-call guard as
other tiny APIs, then transfers its two owned arguments into `rc_cons_owned`.
The common guard is required to preserve the generic dispatcher's exact error
messages for null and foreign operands. Constructor failure consumes both owned
arguments; the common completion preserves pending errors and safepoints.

Cons and tail reuse helpers check canonical pointer identity before the existing
native/arity/capture/opcode classification. The old classification remains intact
for snapshots retained across reinstallation and equivalent original primitive
closures. Constructors/projectors still validate operands, enforce alias rules,
and govern errors and reuse statistics. No extra safepoint or completion step is
added to reuse paths. DO and borrowed-callee eligibility remain unchanged.

## Validation

Independent source review cleared the refined implementation. Fatal ASan/UBSan
passed in default and boxed-integer modes. Tests cover consuming construction,
unique/shared/alias reuse, foreign functions/operands/candidates, null operands
and output slots, replaced and partial functions, old snapshots after reinstall,
pending errors, runtime tail requests stored as pair data, and cyclic vector
reachability. Differential invalid-operand cases compare exact diagnostic text
and consumed slots against generic dispatch.

Emitted optimized/conservative programs passed 24 differential results and assert
all 15 tiny APIs are emitted, including cons snapshots selected before argument
redefinition. Existing ownership differential cases passed 20/20 with reuse
counts and alias behavior preserved. The runtime test is registered in CMake,
Makefile.rc, and scripts/test-rc.sh.

No full-kernel build or full-language-suite benchmark was run in this lane.

## Hardware microbenchmarks

Darwin arm64, Apple CLT clang, `-std=c17 -O3 -flto=thin`, two million iterations
per case. Actual base runtime versus candidate runtime; baseline consuming cons
uses the original generic owned entry point. Three serial runs each in alternating
order; hardware instructions from `/usr/bin/time -l`, medians:

| Case | Base instructions | Candidate instructions | Change |
| --- | ---: | ---: | ---: |
| Owned construction | 853,111,506 | 648,830,714 | -23.94% |
| Unique pair reuse | 436,265,993 | 387,988,587 | -11.07% |
| Shared candidate, fresh allocation | 811,021,389 | 763,056,986 | -5.91% |
| Tail transfer and reconstruction | 536,307,518 | 488,219,104 | -8.97% |
| Old snapshot reuse after reinstall | 435,990,657 | 447,236,809 | +2.58% |

The last control retains its original slow classification and pays an extra
failed canonical check; semantics and reuse counters match. The canonical CONS
root adds one setup retain, with no recurring ownership-count change. These are
focused instruction measurements, not established whole-program gains.

Reproduction: compile `test/rc/bench_canonical_cons.c` against the baseline with
`-DRC_CONS_BASELINE`, or against this runtime without that define. Run each binary
with `owned`, `unique`, `aliased`, `tail`, and `old`, followed by iteration count.

## Combining the generic tag candidate

The separate generic tag dispatcher must append a CONS validated body and table
entry, update its slot-count assertion, and add a valid cons input to its tag test
name/value arrays. The count becomes 15. Its validated cons body can transfer
slots directly to `rc_cons_owned`; public tiny cons still requires the common
checked-call guard and generic slow fallback to preserve diagnostic behavior.
