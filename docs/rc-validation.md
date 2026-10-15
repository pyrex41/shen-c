# Standalone RC toolchain validation

This continuation replaces the Boehm compiler host with the independent RC
reader/emitter, embeds the native kernel in the CLI, and makes the RC toolchain
the supported Make/CMake/Nix build. It adds local last-use ownership, free-variable
captures, owned calls, and selective pair reuse. Exact cycle reclamation remains
enabled for unrestricted mutable Shen values.

## Release performance

Performance remains **OPEN**. Correctness qualification did not establish a
speedup. The previously reported 752.389-second execution used ASan/UBSan;
it is not a release benchmark. Compilation time is excluded below.

Darwin arm64, serial runs of `script runme.shen`, identical unchanged S42
sources and stdin, no sanitizer or profiler:

| Executable | Wall seconds | User CPU seconds | Result |
| --- | ---: | ---: | --- |
| Preserved Boehm CLI, two runs | 12.54–15.43 | 12.00–14.66 | 134/134 each |
| RC checkpoint `4157892`, one run | 103.85 | 94.66 | 134/134 |
| Small-call fast path only, one run | 68.37 | 66.37 | 134/134 |
| Current combined changes, two runs | 57.09–70.88 | 55.55–64.86 | 134/134 each |

See [initial comparison](evidence/rc-performance-20261015/matched-initial.json)
and [final serial comparison](evidence/rc-performance-20261015/matched-final.json).
Wall time varies with host scheduling; these few runs are not a statistical
benchmark campaign. The final candidate still executes about 4.22 times the
retired instructions of the Boehm binary. This compares complete implementations,
not an isolated experiment measuring reference counting versus garbage collection.
RC uses a fresh O3 CMake build; exact historical Boehm compiler flags are unknown.
The original Boehm binary needed an explicitly recorded installed 8.2.12 collector
library because its original Nix store path had disappeared. Its bytes were unchanged.

The implemented fixes are:

- Exact unbound calls with at most eight arguments avoid allocating a request
  and argument arrays, preserving owned/borrowed arguments and callback roots.
- Tail argument arrays use three existing union slots when they fit, without
  increasing the value object size. Larger calls keep the heap fallback.
- Generated code skips external release calls for already-empty slots.

Exact cycle reclamation remains enabled. The original five-second profile
showed allocation/free, call dispatch, symbol lookup, and release overhead;
collector work occupied about 10% of that early sample. It cannot explain the
whole regression. The [current artifact profile](evidence/rc-performance-20261015/profile-current-flat.txt)
still puts symbol lookup, releases, and collector work near the top. This was
a separate, intentionally partial diagnostic, excluded from all suite timings.
Further performance work is required; no Boehm parity claim.

Current-source validation: fresh full kernel and CLI build, eight runtime tests,
13 reader cases, three empty-directory CLI checks, and dependency audits
[passed](evidence/rc-performance-20261015/cmake-release.json).
All eight runtime tests passed fatal
[ASan/UBSan](evidence/rc-performance-20261015/runtime-asan.json).
A fresh sanitizer compiler passed 64 emitted semantics cases, 20 ownership cases,
and four large-frame assertions
[on the current sources](evidence/rc-performance-20261015/emitted-asan.json).
The full 134-case sanitizer suite was not rerun after these performance changes;
its earlier result belongs to checkpoint `4157892` below.

The fresh release compiler and CLI are installed in `bin/`; their
[identities](evidence/rc-performance-20261015/installed-binaries.json) are recorded,
and the prior binaries are retained under `evidence/rc/perf-baseline-installed`.
Portable receipts replace local paths; the
[provenance manifest](evidence/rc-performance-20261015/provenance.json) binds raw copies.

## Evidence scope

The canonical runner builds a fresh compiler and generates all 14 bundled KL
modules, then runs the unchanged S42 `runme.shen`. It checks **134 passed,
0 failed**, process exits, source identities, and compiler/application dependency
audits. The suite loads its source test programs through the RC evaluator;
it does not establish that every dynamically loaded test was compiled ahead
of time. Native source compilation has separate application fixtures.

The canonical receipt binds all sources compiled into its host and generated
application. It excludes CLI `main.c`, which is compiled and hashed separately
by the fresh toolchain and CMake CLI checks. Source hashes identify the tested
working tree even when its parent commit is also recorded.

## Checkpoint 4157892 qualification

Executed on Darwin arm64 with Apple Clang. Checkpoint `4157892` passed these
local correctness checks. The subsequent performance changes have separate
evidence below; these historical receipts do not qualify changed sources.

| Check | Result | Evidence |
| --- | --- | --- |
| Complete kernel and unchanged S42, fatal ASan/UBSan | 134 passed, 0 failed; fresh compiler and application audits PASS; sources unchanged | [Receipt](evidence/rc-standalone-20261015/canonical-asan.json), [log](evidence/rc-standalone-20261015/canonical-asan.log) |
| Fresh default Make toolchain, CLI, native source, ownership measurements | PASS; unchanged sources, ten native comparison builds, canonical artifact revalidation, positive and negative dependency audits | [Receipt](evidence/rc-standalone-20261015/toolchain.json), [log](evidence/rc-standalone-20261015/toolchain.log) |
| Default CMake Release, complete kernel and CLI | PASS; 7 runtime tests, 13 reader cases, 3 dependency audits, 3 empty-directory CLI checks | [Receipt](evidence/rc-standalone-20261015/cmake-cli.json) |
| Complete standalone CLI and source frontend, fatal ASan/UBSan | PASS; 13 reader cases, 3 empty-directory checks, 3 audits | [Receipt](evidence/rc-standalone-20261015/cmake-asan-cli.json) |
| Runtime, fatal ASan/UBSan | 7/7 test binaries passed | [Receipt](evidence/rc-standalone-20261015/runtime-asan.json), [log](evidence/rc-standalone-20261015/runtime-asan.log) |
| Fresh compiler's emitted code, fatal ASan/UBSan | 64 semantics cases, 20 ownership cases, 4 large-frame assertions, metadata in both ownership modes, quoted paths: PASS | [Receipt](evidence/rc-standalone-20261015/focused.json) |
| Rich Shen source application, normal O2 | PASS; all compiler inputs and outputs retained and hashed | [Receipt](evidence/rc-standalone-20261015/native-source.json), [log](evidence/rc-standalone-20261015/native-source.log) |
| Independent native callback and artifact inspection | PASS; six actual registered callback bodies checked | [Review](evidence/rc-standalone-20261015/native-source-independent.json) |

The runtime heap matrix contains 20 named cases and 252,189 assertions; reuse
has 9 cases and the optional acyclic policy has 7. The runtime sanitizer receipt
predates the final emitter edits, but every runtime implementation, header, and
test source matched that checkpoint. Its older compiler-host fields are not final-emitter
evidence. The focused receipt uses the freshly rebuilt final compiler.

The large-frame regression initially produced 2,002,985 bytes of C, exceeded a
300-second O1 sanitizer compilation bound, and overflowed the sanitizer stack
when compiled at O0. Compact cleanup loops preserve each release/null operation
and its order. The same four assertions now pass with 84,697 bytes of generated
C; the fresh-host O1 sanitizer compilation took 4.681 seconds. This is one
regression measurement, not a general performance benchmark.

Portable receipt copies replace local paths with `${REPO}`, `${HOME}`, and
`${TMP}`. Their hashes describe original bytes; original receipts remain under
`evidence/rc`. Use those originals with the acceptance runner. Qualified CMake
Release binaries were installed at that checkpoint; see their
[binary identities](evidence/rc-standalone-20261015/local-binaries.json).
An [independent final crosscheck](evidence/rc-standalone-20261015/independent-review.json)
verifies checkpoint source hashes, retained artifacts, sanitizer flags, native
results, negative controls, and the correspondence of the published receipts.

## Ownership measurements

The paired native workloads check every list element, order, length, and sum.
They compare optimized output with `--conservative-ownership` using actual
runtime counters. Unique traversals must reuse cells proportionally to input
size; retained aliases must remain unchanged and force allocation. Closure
capture and early-release fixtures measure live objects as well as results.
These are scoped allocation/ownership measurements, not a general performance
comparison with the historical Boehm implementation or another Shen port.

| Native workload | Conservative cons allocations | Optimized cons allocations | Optimized cons reuses |
| --- | ---: | ---: | ---: |
| Unique list, 100 elements | 200 | 100 | 100 |
| Unique list, 1,000 elements | 2,000 | 1,000 | 1,000 |
| Retained alias, 1,000 elements | 2,000 | 2,000 | 0 |

In the last-use fixture, peak live objects fell from **4,200 to 2,201**.
The closure fixture's total capture-edge creation counter fell from **67 to 66**.
All ten native comparison executions returned their exact expected results;
the unique and aliased fixtures check every list element. The actual legacy
GC-linked compiler and a synthetic GC-symbol executable were both rejected
by the dependency auditor.

## Source frontend coverage and boundaries

The source pipeline uses the actual kernel reader/compiler. Its rich fixture
covers packages and reflection, empty packages, macros and persistent scalar/
property state, datatype checking, synonyms, source-definition timing, list
patterns, closures, exact literal ASTs, and ordinary top-level file effects
occurring only when the native application runs. Generated callbacks are
inspected independently of the kernel's legitimate dynamic evaluator support.

`test/rc/test_reader_capture.sh` has **13 frontend cases**. Its nine rejection
cases check that existing program and metadata outputs survive a failed
compilation. The compiler explicitly rejects unsupported persistent closure or
vector payloads, nonstandard macro/datatype callback objects, direct persistent
property-vector mutation, reader hook replacement, and nested persistent
function definitions created inside reader auxiliary functions or non-root
reader `eval-kl` forms. Ordinary runtime nested definitions and dynamic source
loading retain the runtime's semantics. Compiler-generated fresh-name sequences
may differ from interpreted loading.

## Reproduce

```sh
make CC=clang
make test
make sanitize
scripts/test-rc.sh --no-build bin/rc-build
test/rc/test_reader_capture.sh bin/shen-rc
test/rc/test_source_compile.sh
scripts/certify-rc.sh
scripts/test-rc-toolchain.sh --canonical-receipt evidence/rc/certify-TIMESTAMP/result.json
```

Use `CFLAGS` for generated applications and `RC_CFLAGS` for explicit host-build
flags. The default Make build also honors `CFLAGS`, `CPPFLAGS`, and `LDFLAGS`.
`NM` and `OTOOL` select platform audit tools. The large-frame test accepts
`RC_FRAME_BUILD_TIMEOUT` (default 300 seconds, maximum 1800) and kills its
entire compiler process group on timeout.

## Limits and historical evidence

Nix syntax is checked. Nix evaluation timed out during the pinned nixpkgs cache
fetch after 45 seconds and a separate 180-second retry; see the
[last receipt](evidence/rc-standalone-20261015/nix-evaluation.json).
Nix package evaluation/build/install checks and remote CI execution are
**UNKNOWN**. Their definitions contain no Boehm dependency.
Linux execution is not established by the Darwin results. The optional
acyclic-only policy has focused tests; it is not unrestricted Shen semantics.

The symbol/dependency audit checks global `GC_*` symbols and direct Boehm
library dependencies. It is combined with the independent source/build graph;
it is not a proof against an arbitrary hidden allocator in an unrelated binary.
On this host Apple Clang's ASan/UBSan executes successfully. The earlier Nix
Clang ASan startup hang remains a toolchain-specific unknown, not a test pass.
Apple's Darwin runtime rejected `detect_leaks=1`; that failed startup is retained
in the CLI receipt. The successful retry uses supported fatal ASan/UBSan options
and makes no LeakSanitizer claim. Runtime object-count assertions separately
check reclamation.

The earlier `97aa8e1` checkpoint, including its Boehm compiler host and old
conservative emitter, is preserved in [the checkpoint record](rc-validation-checkpoint.md).
Historical overlay, dictionary, and typecheck-cache interfaces are not part of
the replacement runtime API; see [the historical backend record](LEGACY-BOEHM.md).
