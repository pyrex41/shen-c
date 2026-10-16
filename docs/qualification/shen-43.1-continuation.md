# Shen 43.1 qualification and performance continuation

Source of intent: user plan, 2026-10-16. Historical S42 checkpoint at 0b74f9d
is preserved. Work is isolated on feat/shen-43.1-qualification; historical
Boehm comparator is a detached worktree at 97aa8e1.

## Written implementation plan

1. Vendor byte-exact official S43 archive subsets, bind a release manifest to
   kernel, source, boot order, suite and redirect output. Confirm the suite count
   structurally. Preserve S42 suite and historical measurements.
2. Upgrade CLI/compiler/install/Make/CMake/CI together. Make certification and
   benchmarking manifest-aware, reject intermediate failures, use writable copies.
3. Rebuild both backends on the same Darwin arm64 compiler/SDK and kernel. Qualify
   runtime, emitted controls, frontend/metadata and complete generated kernel under
   fatal sanitizers. Record Linux/Nix/CI independently.
4. Freeze a pre-optimization RC baseline and measure paired complete suites.
   Profile separately; implement bounded candidates with controls and matched
   evidence. After two insufficient runtime rounds, consider compiler ownership.
5. Freeze the final candidate; retain two batches of five alternating pairs.
   Require paired CPU medians <= .90 in each batch, >=8 CPU wins, wall <=1.03,
   RSS <=1.05 of RC baseline and below Boehm. Keep OPEN until every gate passes.

## Checkpoints

- 43.1 correctness: IN PROGRESS.
- Measured optimization: OPEN.
- Final qualification: OPEN. Linux and remote CI not yet run.

Reproduction commands, immutable evidence and rejected candidates will be added
as each gate is reached. No historical timing counts as current qualification.

## Checkpoint 1: 43.1 local correctness baseline

Official archive SHA-256 matches the requested
`195537eb3480363313678594897d4d8cf6399b484afab0d68cf180e725c52d40`.
`shen/releases/s43.1/upstream.json` inventories 96 preserved upstream files;
`backend.kl` is retained but excluded from the upstream 14-module boot order.
The source structure proves 136 assertions in 36 reports. S42 kernel and suite
remain selectable through `shen/releases/s42.json`.

Both current RC and historical Boehm 97aa8e1 boot 43.1 and pass 136/0. Boehm
needed only the kernel replacement, no C compatibility patch. Apple Clang release
flags are `-O3 -g -flto=thin`; Boehm also retains its required
`-fno-optimize-sibling-calls -fsigned-char`. Boehm GC is Nix 8.2.12, not an unknown
historical executable. Both use the same installed Apple SDK. This compares full
implementations, not the isolated cost of their memory managers.

RC passed 22 default release tests, 44 fatal ASan/UBSan runtime tests across
default/boxed modes, emitted ownership/alias/reuse/closure/mutation/cycles/error/
tail/redefinition controls, 13 reader controls, source metadata and source
compilation, and CMake Release with 22/22 CTest checks. A complete generated
kernel+suite was instrumented with fatal ASan/UBSan and passed 136/0. Source
compilation harness required replacing Python 3.10 `hardlink_to` with `os.link`
for the Mac's Python 3.9. One historical test script lacked executable mode;
that is corrected. Fully sanitized emitted controls are a final-candidate gate.

Initial baseline: three alternating pairs all pass. RC median user CPU 4.97s,
Boehm 4.29s; RC median RSS 31,080,448 bytes versus Boehm 73,760,768 bytes.
This is the fixed pre-optimization RC RSS baseline. Historical S42 results do not
enter these numbers. Defender and desktop activity are present; host quietness
is not established. Profiling is separate: application, evaluate, apply_request,
and frame/edge work lead the sparse sampled windows. Nested suite timers are
retained individually, not summed. The 43.1 assoc template changed its fresh
variables and safely falls back; restoration is the first bounded candidate.

[Published baseline receipts and compressed raw logs](../evidence/shen-43.1-20261016/publication.json)
bind raw and publication hashes. Raw binaries/generated C and evidence remain in
`evidence/rc/frozen-baseline/` and the named `evidence/rc/` directories.

Reproduce from this worktree:

```sh
make -j2
make test
make sanitize
make -f Makefile.rc RC_SANITIZE=1 RC_CPPFLAGS=-DSHEN_RC_IMMEDIATE_INTEGERS=0 RC_BUILD=obj/boxed RC_BIN=bin/boxed test
CFLAGS='-O3 -g -flto=thin' scripts/certify-rc.sh
CFLAGS='-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 scripts/certify-rc.sh
python3 scripts/benchmark-rc.py --config evidence/rc/baseline-config.json --out evidence/rc/new-baseline
```

Boehm build log records the exact historical Make command and Nix include/library
paths. Linux, installed Nix qualification, and remote CI remain UNKNOWN here.
