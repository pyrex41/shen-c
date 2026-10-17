# Shen 43.1 qualification and performance continuation

Source of intent: user plan, 2026-10-16. Historical S42 checkpoint at 0b74f9d
is preserved. Work is isolated on feat/shen-43.1-qualification; historical
Boehm comparator is a detached worktree at 97aa8e1.

## Written implementation plan

Latest checkpoint: [43.1 final measured qualification](../performance/rc-43.1-final-20261017.md).
Code `8e01579` is pushed on `perf/shen-43.1-prepared-eval`. Two frozen five-pair
CPU ratios are **0.8811 / 0.8360**, with **10/10 CPU wins**, twenty complete 136/0
runs, wall improvements and RSS **+3.49%** versus the frozen RC baseline, below
Boehm in every pair. All four Linux/macOS/sanitizer CI jobs pass, alongside fresh
Darwin Make/CMake/Nix/source checks. Numerical performance is PASS; strict
quiet-host provenance remains UNKNOWN because background Defender/desktop
services are recorded. The full finish line remains OPEN for that condition.
These results supersede the historical OPEN CPU results below without replacing
their evidence. Reviewable checkpoints: 43.1 correctness `8625781`, measured
optimization `8e01579`, and the final qualification documentation/evidence commit.

Current optimization continuation: [prepared evaluator and certified ownership](../performance/rc-prepared-evaluator-20261017.md). The original checkpoints below remain historical evidence; new candidate qualification is recorded separately.

Follow-up on 2026-10-17: [comparative profiling and CPU budget](../performance/rc-comparative-profile-20261017.md).
Both backends now have repeated whole-process/load-focused profiles and exclusive
phase accounting. Reading/macro processing and translation show the main excess
RC time; diagnostic calibration limits precise phase comparisons. A two-counter
control saves 2.6% median paired CPU but violates the statistics contract and is
not a product candidate. Closure/symbol temporaries account for about 66% of
ownership operations. Next: compiler ownership/effect summaries at certified
source-processing call boundaries. Product remains `8625781`; target stays OPEN.

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

- 43.1 correctness: PASS on Darwin; Linux and remote CI UNKNOWN.
- Measured optimization: guarded assoc restored; two unsuccessful candidates reverted; target OPEN.
- Final qualification: OPEN because CPU/wall performance gates failed; Darwin checks pass.

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
This initial RSS estimate is superseded by the corrected baseline in checkpoint 3. Historical S42 results do not
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

## Checkpoint 2: measured candidates

The retained change recognizes the complete byte-equivalent 43.1 `assoc` AST
with V3543/V3544, alongside the historical S42 V3546/V3547 template. It retains
all runtime dependency/identity guards. Other fresh names or modified clauses
use the generic native body. Both template versions pass 22 differential cases
plus partial-closure controls: snapshots, every dependency redefinition, aliases,
malformed inputs, error handling and cyclic equality.

Three alternating pairs against rebuilt Boehm all passed 136/0. Paired median
user-CPU ratio was 1.0331 (OPEN); retired instructions were about 121.026 billion
versus baseline 123.928 billion, a 2.34% reduction. Host load varied substantially;
these batches do not establish a causal CPU speedup. A premature measurement
started before linking completed and is marked DISCARDED with every sample kept.

Two further bounded candidates were implemented, tested, measured, and rejected:

- Evaluator callee borrowing through atomic arguments, with promotion before
  computed argument effects: snapshot/redefinition/GC/error-order controls passed,
  but instructions increased about 0.27% versus the assoc candidate. Reverted.
- Compiler summaries borrowing lexical arguments proven live after exact canonical
  primitive calls: 2,039 emitted sites; fatal sanitizer runtime checks, 87 emitted
  semantic cases, ownership and canonical/redefinition controls passed. Instructions
  decreased only about 0.4% versus assoc. A matched comparison against the retained
  assoc binary gave CPU ratios 1.07143, 1.00242, 0.99629 (median 1.00242), with no
  repeatable speedup. Reverted the private API, emitter path and associated API test.

Rejected source snapshots and every measurement remain in the published evidence.
The useful interpreter snapshot/error-order regression tests remain. Exact cycle
collection remains enabled and object caches remain bounded. The deferred
interpreted self-loop experiment was not revived. Next measured cost is still
interpreter application/evaluation and call/frame/edge work; a larger ownership
change needs a demonstrated whole-suite gain before integration.

Final-candidate qualification and ten performance pairs follow this source freeze.

## Checkpoint 3: final Darwin qualification; performance OPEN

The upgrade is implemented and locally qualified. **The agreed performance finish
line is NOT met.** Final acceptance remains OPEN; no claim of a speedup, Linux
qualification, remote CI success, or a quiet host is made.

Fresh standalone CLI acceptance found an omission that the canonical suite did
not exercise: 43.1's reader calls public `char-stream?`. Both historical runtimes
only exposed the S42 helper names. RC now implements `char-stream?` for its byte
streams and the public `read-unit-string`/`write-string` bindings (plus the old
reader alias). Controls cover input/output direction, NUL/high bytes, EOF and
invalid operands. Both non-newline-terminated REPL controls now pass. The Boehm
comparator carries a separate [stream compatibility patch](boehm-43.1-streams.patch);
its kernel bytes still exactly match RC. No collector policy was disabled.
The upstream inventory now includes four byte-exact primitive reference files
(100 upstream files in total). The initial 96-file inventory remains in Git history.

The qualification harness also needed two repairs: keep its synthetic GC symbol
observable under ThinLTO, and recognize the existing two-argument identity closure
constructor in native-body evidence. Added controls still reject wrong arity,
wrong/missing bodies, unknown callback mappings, and evaluator forwarding. The
final acceptance also rejects the actual linked Boehm executable as a negative
artifact-audit control. Earlier failed acceptance receipts remain preserved and
are superseded by `qualified-toolchain-final`.

Final local checks passed:

- Default Make build and all 22 runtime tests.
- All 44 runtime executions in default/boxed fatal ASan/UBSan modes.
- Full emitted semantic/ownership/alias/reuse/escape/cycle/error/tail/redefinition
  controls with fatal ASan/UBSan, plus metadata controls in both ownership modes.
- Complete generated 43.1 kernel and 136-assertion suite in release and fatal
  ASan/UBSan builds; every report and expected redirect output checked.
- Fresh independent compiler/CLI, empty working directory commands, EOF/non-newline
  REPL, source compilation and metadata, native body evidence, optimizer metrics,
  compiler/app dependency audits and actual-GC negative control.
- Fresh CMake Release build, 22/22 CTest and 13 reader controls.
- Darwin Nix package build, runtime/reader checks, installed source compiler and
  installed REPL control. Final package: `/nix/store/668gjaapnsr937hhimpx2pcb75yx0r2q-shen-c-0.2.3`.

Linux and remote CI remain **UNKNOWN**. The checked-in CI now runs release-manifest
validator controls, both sanitizer number modes, emitted sanitizer controls, and
full generated-kernel sanitizer certification. It has not been submitted/run here.

### Corrected baseline and final measurements

The initial baseline predates the public stream bindings and is historical
navigation only. A corrected pre-optimization binary links the fully qualified
runtime/main with the original 43.1 generated kernel before the assoc intrinsic
was restored. Its matching original emitter and generated C are hash-bound. All
three baseline pairs pass the unchanged 136-assertion suite, and the baseline
passes stream/REPL compatibility controls. Use this baseline for the RSS gate.

Two batches of five alternating pairs follow the immutable source/generated/
binary/dependency/input freeze. All 20 executions pass 136/0 and every intermediate
report. Every sample is retained; profiling and counters run separately.

| Gate | Batch 1 | Batch 2 | Result |
| --- | ---: | ---: | --- |
| Median paired user CPU RC/Boehm <=0.90 | 1.102975 | 1.081264 | OPEN |
| Median paired wall RC/Boehm <=1.03 | 1.101124 | 1.077263 | OPEN |
| Median paired peak RSS RC/Boehm <1 | 0.421041 | 0.421170 | PASS |
| CPU wins, at least 8 of 10 | 0 of 10 total | | OPEN |
| Largest RC RSS / corrected baseline <=1.05 | 1.002115 total | | PASS |

RC medians: 4.82s user, 0.01s system, 4.905s wall, 31,014,912 bytes RSS,
121,028,274,630 retired instructions. Boehm medians: 4.39s user, 0.02s system,
4.465s wall, 73,621,504 bytes RSS, 89,030,986,842 retired instructions. These are
independent medians; acceptance uses the matched ratios, not their quotient.
The maximum RC RSS is 31,047,680 bytes. Desktop/Defender and unrelated build
activity occurred during this session; process snapshots bracket timing. A fully
quiet host is not established, and repeating on a quiet host remains necessary
for any future performance acceptance. Both CPU batches fail the target here.

### Refreshed diagnostic and next measured bottleneck

The final binary's sampled windows show application with 12/32/32 visible flat
samples; evaluate appears with 14/11 in the latter two windows, alongside
apply_request, rc_tailcall_owned_args, apply_small and argument/edge cleanup.
The report omits functions below five samples, so these are sparse lower bounds,
not percentages or a causal timing decomposition. Startup is measured in its own
process in `qualified-profile/startup.err`. Individual source-load/typecheck and
execution intervals are retained in every `*-stages.json`; they nest and must
not be summed. The broad cost remains source loading/typechecking through the
interpreter; the largest individual intervals and their source/test labels are
recorded per run.

The separate same-kernel+load diagnostic (not the CLI timing run) records
720,102,458 retains, 803,232,218 releases, 15,196,401 pair allocations,
527,584 system allocations, 382,534 system frees, 11,443 successful pair reuses,
78 collections (76 acyclic), and zero cycle objects reclaimed by this suite.
Runtime cycle controls exercise exact reclamation separately. Reuse caches remain
bounded at 1,024 objects / 131,072 bytes. This evidence does not support a collector
or object-layout rewrite.

Next experiment: specialize immutable interpreted argument shape and lexical
parameter access, keeping dynamic function snapshots and generic paths for
redefinition, partial application, escaping closures and uncertain ownership.
Measure argument/frame ownership transfers independently. The rejected broad
primitive borrow summary did not improve CPU, so it must not be revived unchanged.
Acceptance still requires a repeated whole-suite win and the unchanged RSS gate.

For example, the first final RC run reports an individual 2.362s interval for
loading/typechecking `interpreter.shen` (1,177,726 inferences), inside the complete
roughly 4.87s suite interval. These are nested intervals, not additive components.
Stage labels identify the most recently printed test; enclosing trailing load
intervals can inherit that label and must not be attributed to that last test.

### Reproduction and artifact boundary

Use this branch in its isolated worktree (`../shen-c-43.1`); the original
`feat/perceus-rc` worktree remains at the preserved S42 checkpoint. The three
review checkpoints are recorded as Git commits. The final source freeze records
the pre-final-commit revision plus SHA-256 for every runtime/compiler source,
generated kernel, binary and dependency; those hashes identify the tested bytes
independently of documentation commits.

Reproduce the final RC build and focused gates:

```sh
make CC=/usr/bin/cc -j2
make test
python3 test/rc/test_release_manifest.py
python3 test/rc/test_native_evidence.py
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make sanitize
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make -f Makefile.rc RC_SANITIZE=1 RC_CPPFLAGS=-DSHEN_RC_IMMEDIATE_INTEGERS=0 RC_BUILD=obj/boxed-new RC_BIN=bin/boxed-new test
CFLAGS='-O3 -g -flto=thin' scripts/certify-rc.sh
CFLAGS='-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 scripts/certify-rc.sh
make -f Makefile.rc RC_SANITIZE=1 RC_BUILD=obj/rc-sanitize RC_BIN=bin/rc-sanitize RC_TOOL_BIN=bin/sanitize bin/sanitize/rc-build
CFLAGS='-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' RC_BIN=bin/rc-sanitize ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 scripts/test-rc.sh --no-build bin/sanitize/rc-build
CFLAGS='-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 test/rc/test_source_metadata.sh bin/sanitize/rc-build
CFLAGS='-O3 -g -flto=thin' scripts/test-rc-toolchain.sh --canonical-receipt NEW_RELEASE_RECEIPT --gc-negative-control ../shen-c-boehm-43.1/bin/shen-c
cmake -G Ninja -B build-new -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_BUILD_TYPE=Release
cmake --build build-new -j2
ctest --test-dir build-new --output-on-failure
nix build --no-link --max-jobs 2 --cores 2 'path:.#shen-c'
```

The release receipt for the frozen candidate is
`evidence/rc/certify-20261016T215755.221415Z/result.json`; the fully instrumented
receipt is `evidence/rc/certify-20261016T215755.223795Z/result.json`.
Fresh complete acceptance is `evidence/rc/qualified-toolchain-final/result.json`.
Those directories are retained locally. Portable copies and compressed logs are
under [the published evidence](../evidence/shen-43.1-20261016/publication.json).
Raw and portable hashes are distinct and both recorded. Published paths use
placeholders; local receipts retain exact paths. Final summary:
[final-summary.json](../evidence/shen-43.1-20261016/final-summary.json).

To rebuild the comparator, create a detached 97aa8e1 worktree, copy the 14 pinned
43.1 KL files from this checkout into its `shen/src/kl`, apply the compatibility
patch, and build with the exact flags/dependencies:

```sh
git worktree add --detach ../shen-c-boehm-43.1-new 97aa8e1
python3 - <<'PY'
from pathlib import Path
import json, shutil
r=json.loads(Path('shen/releases/s43.1.json').read_text())
for m in r['modules']:
    shutil.copy2(Path(r['kernel_directory'])/(m+'.kl'),Path('../shen-c-boehm-43.1-new/shen/src/kl')/(m+'.kl'))
PY
git -C ../shen-c-boehm-43.1-new apply ../shen-c-43.1/docs/qualification/boehm-43.1-streams.patch
PKG_CONFIG=/nix/store/nhgylvj98klqggy54h7gbc3b2ia4a00y-pkg-config-wrapper-0.29.2/bin/pkg-config PKG_CONFIG_PATH=/nix/store/q8mzql20dwjdc6pd9bn49wya477daxsz-boehm-gc-8.2.12-dev/lib/pkgconfig make -C ../shen-c-boehm-43.1-new -j2 CFLAGS='-O3 -g -flto=thin -std=c17 -fno-optimize-sibling-calls -fsigned-char -I/nix/store/q8mzql20dwjdc6pd9bn49wya477daxsz-boehm-gc-8.2.12-dev/include' LDFLAGS='-L/nix/store/r9bdx29an70k9jg6jj2h1ccnpjqdbwzg-boehm-gc-8.2.12/lib -lgc -flto=thin'
```

The Nix store paths identify the arm64 Darwin dependencies used here; restore
those exact paths from the cache or record a new dependency set for another host.
Do not compare a changed toolchain/dependency set against this receipt.

For a fresh pre-optimization baseline, use a separate worktree at this branch
and replace only the emitter with the version from checkpoint 1. It retains the
stream corrections and all other final runtime semantics:

```sh
git worktree add --detach ../shen-c-43.1-baseline feat/shen-43.1-qualification
git show e384b3d:src/rc/emit.c > ../shen-c-43.1-baseline/src/rc/emit.c
make -C ../shen-c-43.1-baseline CC=/usr/bin/cc -j2
python3 scripts/freeze-rc-benchmark.py --rc bin/shen-rc --boehm ../shen-c-boehm-43.1-new/bin/shen-c --boehm-root ../shen-c-boehm-43.1-new --boehm-lib /nix/store/r9bdx29an70k9jg6jj2h1ccnpjqdbwzg-boehm-gc-8.2.12/lib/libgc.1.dylib --baseline ../shen-c-43.1-baseline/bin/shen-rc --baseline-c ../shen-c-43.1-baseline/obj/rc/kernel.c --baseline-emitter ../shen-c-43.1-baseline/src/rc/emit.c --out evidence/rc/new-freeze
python3 scripts/benchmark-rc.py --config evidence/rc/new-freeze/baseline-config.json --out evidence/rc/new-baseline
python3 - <<'PY'
import json, hashlib
from pathlib import Path
freeze=Path('evidence/rc/new-freeze')
receipt=Path('evidence/rc/new-baseline/result.json').resolve()
result=json.loads(receipt.read_text())
assert result['execution_status']=='PASS'
config=json.loads((freeze/'final-template.json').read_text())
config['rc_baseline_rss_bytes']=result['summary']['rc']['max_rss_bytes']['median']
config['rc_baseline_receipt_sha256']=hashlib.sha256(receipt.read_bytes()).hexdigest()
for case in config['cases']:
    case['dependency_sha256'][str(receipt)]=config['rc_baseline_receipt_sha256']
(freeze/'final-config.json').write_text(json.dumps(config,indent=2)+'\n')
PY
python3 scripts/benchmark-rc.py --config evidence/rc/new-freeze/final-config.json --out evidence/rc/new-final-ten-pairs
python3 scripts/profile-rc.py --cli bin/shen-rc --out evidence/rc/new-profile
```

The retained local corrected baseline was equivalently linked from the original
43.1 generated kernel and the final runtime/main; its source and generated
identities are in the freeze receipt. The actual frozen final configuration is
`evidence/rc/qualified-freeze-v2/final-config.json`. Ten repetitions run serially;
the acceptance report separates pairs 1–5 and 6–10 and requires eight CPU wins.
The RSS gate uses the largest candidate RSS, not merely its median, and requires
RC RSS below Boehm in every pair.

Run profiling separately. Freeze environment, compiler, SDK, libraries, input and
source, retain every run, and record host contention. The benchmark clears inherited
Shen/RC/GC/profiling variables; intentional case overrides are explicit in config.
