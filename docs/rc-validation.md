# RC validation and acceptance boundaries

The RC runtime is built from `src/rc` with `Makefile.rc`. Its generated
applications must link that runtime, release acyclic values promptly, and reclaim
unreachable mutable cycles at safepoints. `bin/rc-build` is a compiler host built
with the existing runtime; its dependencies do not establish the dependencies of
the executable it emits. Audit the emitted executable itself.

## Reproducible commands

```
make -f Makefile.rc test
scripts/test-rc.sh --no-build --runtime-only
make -f Makefile.rc sanitize
# Build bin/rc-build with the normal project toolchain, then:
scripts/test-rc.sh --no-build bin/rc-build
```

The runner requires every selected test binary to exist and succeed. The default
scope also requires the compiler host, emitted semantic cases, large-frame
regression, and builder path regression; it does not
silently skip an unavailable component. `--runtime-only` selects the heap,
primitive, evaluator, boolean-tail, and automatic-collection binaries
explicitly. `RC_BIN` selects a separate binary
directory. `NM` and `OTOOL` can select working platform tools. The audit rejects
Boehm `GC_*` symbols, including global definitions, and direct libgc/libbdwgc
link dependencies. It cannot establish absence of every possible hidden or
indirect runtime allocation strategy.

For generated-code sanitizer coverage, build the RC library and generated app
with sanitizer instrumentation as well as the test harness. A successful plain
run does not qualify a sanitizer run. On Darwin, leak sanitizer support depends
on the toolchain; object accounting tests independently check actual RC frees.

## Heap ownership matrix

`test/rc/test_cycles.c` has **20 named runtime cases**. It links the real
implementation, constructs actual runtime values, and observes live object/byte
counts and free counters. It contains no substitute collector or model heap.
The assertion count includes loop iterations and is not the case denominator.

| Case | Required evidence |
| --- | --- |
| Acyclic aliases | Last release frees the object; a retained alias remains usable. |
| Self vector cycle | Release alone leaves one object; collection frees exactly one cycle object. |
| Two vector cycle | Collection frees exactly two cycle objects. |
| Rooted cycle with outgoing string | Collection preserves all three objects while rooted; dropping the root permits all frees. |
| External alias root | A retained C alias protects a self cycle across collection. |
| Closure/vector cycle | Capture and application remain valid while rooted; both objects are reclaimed after release. |
| Aliased vector slot replacement | Retain before overwrite; a separately held element survives slot replacement. |
| Explicitly broken cycle | Ordinary RC frees the vector without crediting cycle collection. |
| Independent live/dead cycles | Only the unreachable cycle is reclaimed. |
| Cycle pointing to a live value | Collection releases the dead edge while preserving the independently retained value and its exact refcount. |
| Pair/vector cycle | Pair traversal participates in collection; both objects are freed. |
| Partial closure cycle | Bound application arguments are traced; the partial closure/vector cycle is freed. |
| Tail request cycle | Function and argument edges in tail request values participate in collection. |
| Global replacement | A global protects a cycle; replacing it preserves C aliases and permits eventual reclamation. |
| Function redefinition | Old closure/vector cycle remains valid through a held function alias and is collected after release. |
| Deep release | Releasing a 200,000 pair list frees 400,000 objects without recursive destructor stack growth. |
| Tail trampoline | 250,000 native tail bounces return 42 with periodic collection and complete reclamation. |
| Repeated collection | 1,000 successive cycles leave zero live objects after each collection. |
| Error cleanup | Bounds error is observable/clearable; releasing the vector leaves zero live objects. |
| Context shutdown | Shutdown invalidates an outstanding rooted cycle without walking stale edges. |

Bare-context tests finish with zero live bytes/objects and equal allocation/free
counts. Symbol interning is an intentional context root; global/function cases
compare against a baseline after interning and then destroy their context.
Context destruction itself cannot prove a collection reclaimed a cycle: cycle
cases inspect counters before destruction.

The separate `test_bool_tail` binary exercises 100,000 guarded tail bounces
with a bounded live-object peak, rejection/cleanup of nonboolean final
results, guards ordered before outer overapplication and after inner
overapplication, and an aliased tail request. It supplements the 20-case heap
matrix; it is a separate binary and is not included in that denominator.

The separate `test_collection` binary checks automatic collection on 10,000
unrooted self cycles, a frozen survivor budget including slots in a rooted
10,000-element vector, and exact explicit interval/disable behavior. The
default budget is recomputed after collection from survivors, so growth in
new unreachable cycles cannot continually postpone collection.

## Emitted language semantics

`test/rc/test_semantics.sh` creates temporary KL input, builds one actual emitted
application, and compares **64 result lines** with exact expected values. It
checks lexical function bindings and shadowing; arithmetic; lists and aliases;
nested closure captures; partial and excess arguments; mutable vectors,
replacement and cycles; globals and aliases; self and mutual tail calls;
error handlers with captures and nested errors; bounds/type errors; lazy
branches; strings/characters; dynamic `eval-kl`; function redefinition; top-level definition source order; ordinary `do`
redefinition; callee resolution before argument-side function redefinition; 100,000-call
self/mutual recursion under final `and`/`or` operands; and trapping invalid
boolean operands both as immediate values and tail-call results. It also
checks partial `do`, a redefined unary `do`, indirect symbol callees captured
before argument-side redefinition, and undefined indirect callees rejected
before argument-side global effects.
The executable also receives the dependency/symbol audit. Test inputs and
expected values live in this script; existing fixtures and goldens are not
modified.

The 64-case emitted application passed with Apple Clang ASan+UBSan
instrumentation on the runtime and generated C, and passed the executable
Boehm symbol/dependency audit. The final compiler `bin/rc-build` produced this result; the log is
`evidence/rc/emitted-semantics-final.log`. The evaluator agent independently
ran the matching `do` and callee snapshot regressions under Apple Clang
ASan+UBSan successfully. These cases establish selected semantics. They do
not replace the checked-in Shen suite or prove a full language port.

`test/rc/test_frames.sh` passed four additional output checks with generated C
and runtime instrumented by Apple ASan+UBSan: a non-tail function with 300
lexical bindings recursed 256 levels; a closure retained a captured value
across an 80-binding frame; a trapped failure cleaned up an 80-binding frame;
and an 80-binding tail-recursive function completed 5,000 calls.

`test/rc/test_builder_paths.sh` also passed. It compiled and ran an emitted
application from a checkout path containing spaces and an apostrophe, with
input and output paths containing spaces, then rebuilt using the generated
Makefile default after removing `SHEN_C_HOME`.

## Observed results and source snapshots

| Check | Result | Evidence |
| --- | --- | --- |
| Make runtime tests, Apple ASan+UBSan | 5/5 binaries passed; heap test 20/20 cases with 252,189 assertions | `evidence/rc/sanitizers-final.log` |
| CMake Release runtime tests, Apple ASan+UBSan | 5/5 passed in 5.53 seconds with assertions active and fatal sanitizer errors | `evidence/rc/cmake-tests-final.log` |
| Aggregate runner, Apple ASan+UBSan | 5 runtime binaries and their artifact audits, 64/64 emitted cases and artifact audit, four frame outputs, and builder path regression passed; exit 0 | `evidence/rc/emitted-semantics-final.log` |
| Complete bundled kernel with unmodified S42 `runme.shen`, normal build | 134 passed, 0 failed, suite exit 0, executable audit passed, source unchanged during run | `evidence/rc/certify-20261015T193823.156765Z/result.json` |
| Complete bundled kernel with unmodified S42 `runme.shen`, final Apple ASan+UBSan build | 134 passed, 0 failed, suite exit 0, executable audit passed, source unchanged during run, suite matches recorded revision | `evidence/rc/certify-20261015T194151.087992Z/result.json` |

The normal canonical build took 21.715 seconds and suite execution took
61.289 seconds. It used the same runtime and emitter as the final aggregate
run, before the final compiler-host checkout-path quoting fix and runner
additions. Its `result.json` retains its own source and executable SHA-256
values; it does not claim the final host-builder source identity. This establishes the bundled canonical language-suite result;
shipping CLI, packaging, overlays, caches, and broader ecosystem acceptance
remain separate gates. Tests using `assert` preserve their checks in Release
builds, including explicit `NDEBUG` removal in the primitive and evaluator
test files. The earlier Nix ASan startup problem below was bypassed by the
successful Apple Clang sanitizer runs.


The final canonical Apple Clang 21.0.0 run built in **86.744 seconds**, ran the
suite in **434.359 seconds**, and audited the executable in **0.106 seconds**.
Both the runtime and emitted C used `-fsanitize=address,undefined` with
`-fno-sanitize-recover=all`; ASan and UBSan had `halt_on_error=1`.
The suite matched checkout revision
`cc07d1045c8aba1c1fa4057d5cd9c8ec0d71668d`, and every recorded source hash
remained unchanged during the run. The final executable SHA-256 is
`197140571ab9060d629201a04bbe5cae955ef0e2326d8f9833785753aa49e85f`.
The recorded compiler-host binary SHA-256 is
`5c26d482e025d4b20cc9a25d7deb97eb810ca341ae3d92832a7e15896365ef42`.
The certifier records that binary separately; it does not independently
establish correspondence between the host binary and its source. Preserve
the host build log for that provenance.

The certifier bounds build and suite execution with configurable timeouts,
each process log at 64 MiB, and artifact audit at 30 seconds. Its default
build timeout is 300 seconds and suite timeout is 600 seconds; environment
variables can override both. Only an exit-zero execution with the final
134/0 report, successful audit, and unchanged source is accepted. This
canonical result covers the bundled language suite and complete bundled
kernel. CLI and wider ecosystem qualification retain their separate gates.

The final sanitizer [certification record](evidence/rc-s42-asan.json) and
[complete suite output](evidence/rc-s42-asan.log) are tracked with this change.
Other development logs remain in the local `evidence/rc` directory.

## Existing product acceptance matrix

The baseline project's `Makefile` establishes the following independent gates.
Reusing a gate name or a historical result is insufficient: run the equivalent
workload against the RC executable and retain its output and source identity.

| Existing gate | Required RC equivalent | Scope boundary |
| --- | --- | --- |
| `test_foundation` | Numbers/lists, primitive application, reader EOF, argv, environments, version/type special forms | Existing tagged heap/GC-specific assertions need RC ownership equivalents. |
| `test_abi` | Interning, native registration/application, trap errors, dynamic eval, captures, partial/application behavior, both trampolines | Overlay installation and typecheck cache record/replay/errors need independent migration evidence. |
| `test_emit` | C emission, native closures, calls, tail transfers, module/kernel generation, structural emitter checks | Output containing C functions alone does not establish execution semantics. |
| `sum`, `fib-small` | Exact 42 and 6765 results; generated build systems | Make and CMake artifacts are separate gates. |
| `hello-ygg`, `fib-ygg` | Shaken real kernel, expected registrations, exact hello/fib outputs | Small handwritten KL tests cannot replace shaken-kernel execution. |
| `tc-ygg` | 568 definition typechecker/interpreter fixture; normal form 7 and inference output | Dynamic eval/typechecker behavior needs current evidence. |
| `interp-aot` | 83 definitions; native captures; normal form 7; source constraints | An evaluator fallback does not satisfy existing no-evaluator AOT constraints. |
| `runme-aot` | 579 definition runner with real kernel/sidecar dependencies and source constraints | Successful compilation does not establish language-suite acceptance. |
| CLI smoke | Version 42 without `SHEN_C_HOME`; arithmetic, strings, trap, stream writes | A generated application does not itself provide the shipping CLI. |
| `certify` | Run checked-in `shen/test/s42/runme.shen`, final `passed ... 134`, `failed ... 0`, successful exit | Last report and denominator must match; an interrupted run is UNKNOWN. |
| No Boehm runtime | Inspect actual shipped/generated executable imports and linked libraries | Compiler-host dependency and emitted-runtime dependency are distinct. |
| Sanitizers | Instrument runtime plus emitted C and run ownership, semantic and product workloads | Unsupported/hung sanitizer startup is UNKNOWN, never PASS. |

## Host sanitizer limitation observed during development

A focused run with Apple Clang 21 from
`/Library/Developer/CommandLineTools/usr/bin/clang`, explicit
`-isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`, and
`-fsanitize=address,undefined -O1 -g -fno-omit-frame-pointer` passed all
**20/20 heap cases (252,189 assertions)** with both sanitizer halt-on-error
options enabled. A separate Nix Clang UBSan-only run passed the initial 16-case matrix.
These results cover the heap test executable, not all emitted applications.

On this macOS 27.0.1 arm64 host, the Nix Clang 21.1.8 AddressSanitizer runtime
hung before `main` during a focused heap test. A one-second process sample showed
`libSystem_initializer -> __malloc_init -> AsanInitInternal ->
InitializeShadowMemory -> MemoryRangeIsAvailable ->
dyld_shared_cache_iterate_text_swift -> _Block_copy -> malloc ->
AsanInitFromRtl -> StaticSpinMutex::LockSlow`. This is startup evidence, not
execution of the RC test. The bounded run was stopped. This toolchain-specific failure does not invalidate the successful Apple
Clang sanitizer run; it must not be counted as an executed test.
A direct compiler-driver link with `-g` also required an unavailable `dsymutil`;
compiling debug objects and linking them separately avoids that tool dependency.

## Source snapshot for final aggregate validation run

The following SHA-256 values identify the sources used for the final aggregate
validation run on 2026-10-15, including its 64-case emitted application. The compiler reported 21 native definitions,
24 lambdas, and 64 printed top-level expressions. These hashes identify this
run; later changes require fresh execution evidence.

| Source | SHA-256 |
| --- | --- |
| `src/rc/rc.c` | `5ee8c7c359e42c318c53bb75d12b4c6ea239624841b0a9a2eef154fea110aa1d` |
| `src/rc/rc.h` | `61099dbed188eee06629ebe4319172c71f339cd49fd1520224ffdcd30dcdef29` |
| `src/rc/primitives.c` | `c48dca0328efd10679ce5503cbb9e6c2751aded2d86629e33e11b3735fc21019` |
| `src/rc/primitives.h` | `3f65d47bfe0f7b5a2e80b73648dea203cf2e3c3cc388dc759692e5bfbb6f3d75` |
| `src/rc/eval.c` | `1fa1e61b5ebc2bb55d17b62153e8be9c55e46c0b42948fa5b497f31c5310da13` |
| `src/rc/eval.h` | `40840e20cb00334a7032ef11aec08ce6a0846912ae05dfb0d08e894ba7ad2e15` |
| `src/rc/read.c` | `f97cfe8c99ce3132d0470477f3cd6ee2c2dbccbcf446478627822f6d275b6cde` |
| `src/rc/read.h` | `65bdee933b532e442f4bcdd9398cbdebb1dd0ba45181e84e6abefe79dcd4d553` |
| `src/c/emit_rc.c` | `28557c27498245d5f60e7e7c8943742750db178e1767233c960765cf58a53087` |
| `src/c/emit_rc.h` | `a40d1ae3ac208019b667060d8c1ff4828293af85daf797dc09dbb52935fe5036` |
| `tools/rc-build.c` | `ac97b377feb5e3dfb02828b9323908ab81d68ee63a1a3508455948281f40530b` |
| `Makefile.rc` | `ecf5097d30eef152c8296ccc69a10d3af9d319575eb2aa9e35b59d4d03d6107e` |
| `CMakeLists.txt` | `57079e4c09f7565413f425dd8bfdf2ceba6f8eaf1cf72565aae71fbacefe97d1` |
| `test/rc/test_cycles.c` | `e44dc65af160ee6ad0fc00c597fdfc3e78ffddfc28cf6cb42527152b29de5dc2` |
| `test/rc/test_primitives.c` | `f2b740c89ab3c86fdb88680db127f2bc49521b84dfe8384e22abe953e9a6d417` |
| `test/rc/test_eval.c` | `ea94558730dcdf4acab3852d4f9a49c1829efa002ce1a46b1ede8cc85a97bb35` |
| `test/rc/test_bool_tail.c` | `c12069632d5a260066b549f791b6c8fddb153751bf9a2ce229189de38e3588d9` |
| `test/rc/test_collection.c` | `215e51f6aade5a9211e60443bc1990fe2beb8322cd8fb093f3fa6831b2649416` |
| `test/rc/test_semantics.sh` | `53d0d421669aa875356980dbe354eee602df838a2b43a055f0c143267c2686ef` |
| `test/rc/test_frames.sh` | `42d8e8423a521f78a6f84cf1516e7da803568c700b15fb25a2cb7dd209e1bf97` |
| `test/rc/test_builder_paths.sh` | `603cfb1c0666a6566192f4764ba8d0238c4da51c236344813b13b315a7179dbc` |
| `scripts/test-rc.sh` | `59167616d3251a3736e5861aae4f6b38e1daea0fb98aafd16f8d672ccadabaa6` |
| `scripts/certify-rc.sh` | `e8cf499a8615f13b7c4796cfb0a418e84c9c0521895facd5830e14e9b346b104` |
