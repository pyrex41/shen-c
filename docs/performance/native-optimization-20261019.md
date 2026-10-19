# Native execution and compiler cost checkpoint, 2026-10-19

Status: correctness qualification is recorded below; repeated whole-suite
performance acceptance remains **OPEN**. These experiments compare two native
RC checkpoints, not RC versus Boehm. The earlier [loaded execution controls](compiled-execution-20261019.md)
remain a separate comparison with the diagnostic interpreter.

## Remove ownership work only where the compiler proves the owner survives

The loaded typechecker profile points to graph traversal, release work and tail
requests. Its simple repeated typecheck performs 3,787 retains and 4,147 releases.
The bounded candidate lets eight condition predicates borrow lexical operands
that remain live after the condition. Exact installed primitives use a checked
borrowed path; replacements use checked application and independent snapshots.
Effectful operands, uncertain bindings and last-use owners keep the consuming
path. Last-use owners must disappear before the existing safepoint so the
optimization preserves the roots visible to cycle collection.

The first prototype borrowed all bound operands. Its 15,000-typecheck control
reduced retains from 56,805,000 to 52,005,000 (8.45%) and releases from 62,205,000
to 57,405,000 (7.72%). A fresh single-pair counter control on the integrated live-after rule
reproduced the same reductions with zero timed compiler CPU or interpreter
entries; it overlapped qualification and receives no CPU timing credit. The prototype's three
whole-suite pairs all passed 136/0 without compilation or interpretation. With
a pruned native image, median paired retired instructions fell about 1.15%;
user-CPU ratios were 0.847, 1.254 and 0.979. That spread does not establish a
reliable CPU improvement.

An earlier pack retained 509 shapes from several compiler versions. Its peak
RSS rose about 17%, so that preparation is rejected for performance acceptance.
Recording used keys and rebuilding a plain 363-shape pack brought median
paired RSS to about 1.003 of the baseline. Usage logging is diagnostic only;
timed images have no logging. Images remain bounded to 512 shapes and 32 MiB
of canonical source. Pack writers serialize with a bounded lock wait and
compiler timeout. Source-bundle reuse verifies canonical hashes and recompiles
for the current ABI; current full-source equality is still required at lookup.

A separate refreshed three-second loaded typecheck profile captured 1,855 main
thread samples. Its largest collapsed leaves include release-edge work (85),
request dispatch (78), edge traversal (78), collection (77), owned tail-request
creation (74), argument consumption (58), release (58) and owned equality
conditions (50). These are exclusive sampled leaves, not additive inclusive
stack times. The 300,000-call driver checks every result and records zero
compiler-child CPU and interpreter entries. The default inference limit ended
the first long probe early; the profiling driver now explicitly sets a billion
inferences before the timed loop. A short symbolication timeout is also retained
as rejected profiling evidence. Exact cycle reclamation remains enabled.

This supports a next compiler experiment: ownership summaries for certified
call boundaries that avoid repeated parameter ownership and tail-request work,
with checked generic paths for redefinition, partial application and escaping
closures. It does not justify disabling cycle collection or reviving the
unbounded interpreted self-loop experiment.

Portable receipts and logs are in [the evidence directory](../evidence/native-optimization-20261019/).
They retain all samples, including the failed and rejected preparations.

## Bound C compiler work

Apple clang's AArch64 optimizer spent all 651 sampled worker stacks in
`MachineRegisterInfo::hasOneNonDBGUse`, beneath conditional-branch peephole
optimization. The original standard-library initializer was roughly 166,715
lines in one C function. A full-debug ASan/UBSan build was stopped after more
than 17 minutes; reducing debug information alone also exceeded its 240-second
budget. Those unfinished builds are not successful measurements.

Large straight-line AOT bodies now split into noinline C helpers sharing the
original owned frame and argument scratch space. Calls, snapshots, cleanup,
allocations and collection boundaries retain their existing ordering. Dynamic
modules, loops, C locals, labels, borrowed callee storage and unrecognized
control flow keep their original lowering. The regression compares optimized
and conservative output across redefinition, explicit collection, errors and
strings containing C-like text.

This integrates main's independent constant-table emitter (913acf5), which
keeps large constant trees compact while replaying observable cons/protect
calls. Generic splitting still bounds large nonconstant user bodies. Packed
modules isolate the new table typedefs and helper names; two distinct modules
with tables are compiled together and checked for native pack hits.

| Build observation | Generated C to object | Generated C to CLI |
| --- | ---: | ---: |
| Chunking before constant tables, release O3/thin LTO | 29.90 s | 125.04 s |
| Chunking before constant tables, full-debug fatal sanitizer | 204.18 s | 205.85 s |
| Integrated constant tables and chunks, release O3/thin LTO | 16.32 s | 91.20 s |
| Integrated constant tables and chunks, full-debug fatal sanitizer | 105.72 s | 106.21 s |

These are file-completion timestamp intervals on a shared Darwin arm64 host,
not compiler user CPU or a controlled causal comparison. Link inputs, source
hashes, toolchain and logs are archived. LTO linking is now a visible part of
release build time. Apple's incremental LTO cache is a possible next experiment;
it has not been enabled or measured.

## Reproduce and continue

```sh
make -f Makefile.rc CC=clang RC_LTO=1 all bin/rc/bench_execution
make -f Makefile.rc CC=clang RC_SANITIZE=1 \
  RC_CPPFLAGS=-DSHEN_RC_VERIFY_OWNERSHIP=1 \
  RC_BUILD=obj/qualified-san RC_BIN=bin/qualified-san RC_TOOL_BIN=bin/qualified-san all test
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  CFLAGS='-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -DSHEN_RC_VERIFY_OWNERSHIP=1' \
  RC_BIN=bin/qualified-san scripts/test-rc.sh --no-build bin/qualified-san/rc-build
python3 scripts/test-compile-cache.py bin/rc/test_native_compile
python3 scripts/test-compile-first.py bin/shen-rc \
  --cache evidence/current-cache --configs interpret,native-cold
python3 scripts/bundle-native-cache.py --cache evidence/current-cache
python3 scripts/test-compile-first.py bin/shen-rc \
  --cache evidence/current-cache --configs interpret,native-warm
python3 scripts/measure-execution.py bin/rc/bench_execution \
  --cache evidence/current-cache --rounds 3
```

Keep compilation, source loading and already-loaded execution separate. Do not
sum nested Shen timing reports. Freeze the plain image and binaries before
paired timing. Final acceptance still requires the current 16-module Boehm
comparator and two quiet-host five-pair batches, <=0.90 median user-CPU ratio
in each batch, at least eight wins, <=3% median wall regression and the agreed
RC baseline/Boehm RSS bounds. Earlier release receipts do not qualify this
backend. Refresh the loaded execution profile after the final live-after rule;
then choose the largest measured remaining cost among graph traversal,
parameter ownership and tail dispatch.

## Tail correctness follow-up (55bf4a9)

The complete suite at ea1ae24 passed, but the broader emitted sanitizer controls
then found a native stack overflow in `(and true (do ... (f ...)))`. The saved
`do` identity allowed the recursive operand to be forwarded, but its ordinary
application still drove that operand through another C call. The preceding
receipts and profile above describe that earlier source, not the follow-up.

The emitter now creates a tail request only if every saved `do` callee in the
forwarding chain is a certified second-argument identity. It forwards the result
through those identities and leaves arbitrary replacements on the eager checked
path. Boolean completion guards still validate the eventual result. This is a
compiled control-flow correction; it does not revive the deferred interpreted
self-loop experiment or change the collector.

The regression checks 17 outcomes with both ownership lowerings and both native
and diagnostic interpreter execution, including 40,000 self calls, 20,000 boolean
calls, a nonboolean final result, 30,000 mutually recursive calls, nested `do`,
callee snapshots, mutation, collection, lexical/computed calls and partial
application. Native executions must report zero interpreter entries. Fatal
ASan/UBSan controls pass in both immediate and boxed modes.

Final follow-up receipts, exact build inputs and focused controls are recorded
separately in [the tail qualification evidence](../evidence/native-tail-qualification-20261019/README.md).
The preceding timing and counter observations do not automatically qualify this
changed source for performance. Whole-suite Boehm acceptance remains OPEN.
