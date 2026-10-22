# Changelog

## Unreleased

- **Kernel: upstream ec16a9f.** Default to an unmodified build of
  Shen-Language/shen-sources commit ec16a9f, tizoc's merge of the
  type-checker fix #117 into 43.1 (pinned from pyrex41/shen-sources release
  `upstream-ec16a9f`, tarball sha256 `060066a8…acd`). `shen.by-hypothesis`
  and `shen.search-user-datatypes` deref the subject once and return `false`
  at once for an empty hypothesis or datatype list. `(version)` stays
  `43.1`. The official 43.1 release stays selectable through
  `shen/releases/shenoskernel-43.1.json`;
  `scripts/fetch-kernel.sh --version 43.1-ec16a9f|43.1 --check` verifies
  either.
- **Kernel lineage change.** Embed the community ShenOSKernel 43.1
  (Shen-Language/shen-sources tag `shen-43.1`, commit 715824a; release
  tarball sha256 `900eedef…920`) byte for byte, replacing Mark Tarver's S43
  kernel (S-series). `(version)` is still `43.1`, now set by the release's
  `init.kl`. The image adds `dict`, `init` and the seven bundled extensions;
  a port boot module runs `shen.initialise`, the extension initialisers and
  `stlib.initialise`. `scripts/fetch-kernel.sh --check` verifies the
  vendored files against the tarball.
- Qualify on the community suite: 154 assertions in 40 reports, replacing
  Tarver's 136-test suite.
- Native `symbol?`, `variable?` and `not`, and the guarded `shen.pvar?` fast
  path, now give exactly the kernel's results, checked by
  `test/rc/test_kernel_overrides.py`. The 42.x stream hooks
  (`shen.char-stinput?` and friends) are gone; use the public
  `char-stream?`, `read-unit-string` and `write-string`.
- Source-form handlers and the namespaces extension are booted, so the two
  Bifrost cases that S43 skipped now run.
- A load-time C compile that fails or exceeds `SHEN_RC_COMPILE_TIMEOUT` no
  longer aborts the program. That form is interpreted instead, with one stderr
  notice per process and a `fallbacks=` count in `SHEN_RC_NATIVE_STATS`; the
  result is unchanged. Nothing is cached for it, so the next run compiles it
  again. `SHEN_RC_VERIFY_NATIVE=1` keeps the 0.4.0 behaviour: any compiler
  failure is an error. This replaces the 0.4.0 rule that native mode never
  falls back to interpretation.
- Cut compiled-path ownership work (#7). Interned symbols are immortal, and so
  are capture-free top-level definitions and builtins: named calls through them
  perform no reference-count operations. Per-operation retain/release counters
  are compiled only with `SHEN_RC_STATS_COUNTERS=1` (`make counters`). RcValue
  shrinks from 128 to 112 bytes. Suite ownership operations fall from 1.18
  billion to 0.51 billion and peak RSS by about 13%. See
  [the measurements](docs/performance/rc-ownership-overhead-20261018.md).

## 0.4.0

- Compile loaded Shen source, REPL forms and explicit `eval-kl` to native C
  before execution. Function bodies can compile lazily on first call; native
  mode never falls back to interpretation. Dynamic code requires a C compiler.
  `SHEN_RC_EXECUTION=interpret` remains an explicit diagnostic mode.
- Cache native code by its verified code shape and compiler/ABI identity, with
  live literal bindings, bounded disk storage and optional packed images.
- Preserve dynamic redefinition, escaping closures, mutable cycles and guarded
  tail calls, including boolean recursion through identity `do` calls.
- Reduce retain/release traffic with guarded borrows of live lexical predicate
  arguments. Split huge generated initializers into shared-frame helpers and
  use constant construction tables to reduce C compiler work.
- Include the standard library in the embedded kernel and retain the complete
  pinned 136-test Shen 43.1 suite, source compiler and metadata behavior.
- Provide relocatable macOS and Linux binary toolchain archives for arm64 and
  x86_64, including runtime sources and the source compiler. Each archive has
  a build receipt and is checked after extraction outside the build checkout.

The complete suite passes in release and fully instrumented ASan/UBSan builds
with zero interpreter entries. The latest packed-cache Darwin qualification took
8.17 seconds wall / 7.27 seconds user CPU, including startup, loading and
typechecking; it was a single shared-host run, excluding initial compilation.
Current matched Boehm performance acceptance remains **OPEN**. The 0.3.0
performance results below describe that earlier implementation. See the
[compile-first report](docs/performance/compile-first-20261019.md) and
[latest correctness and profiling evidence](docs/performance/native-optimization-20261019.md).

## 0.3.0

- Make the native C17 compiler and reference-counted runtime the default
  toolchain, with exact cycle reclamation and no Boehm build/runtime dependency.
- Upgrade the complete portable kernel, Shen compiler sources, CLI and pinned
  upstream suite to official Shen 43.1. Validate all 136 assertions, intermediate
  reports and redirect output from fresh writable suite copies.
- Reduce ownership and evaluator costs with last-use moves, owned calls,
  bounded cons reuse, adaptive prepared evaluation, guarded Prolog paths and
  certified generated ownership boundaries. Dynamic redefinition, escaping
  closures, mutation and cycles retain generic semantic paths.
- Qualify default/boxed runtime and emitted controls under fatal ASan/UBSan,
  including a fully instrumented generated kernel and complete suite. Validate
  Make, CMake, installed Nix packages, source compilation and standalone CLI use
  on Linux/macOS CI.
- Meet the measured CPU target against the matched Boehm comparator: 16.4% and
  16.6% lower median paired user CPU in two five-pair Darwin arm64 batches,
  ten CPU wins, twenty complete 136/0 runs, and peak RSS 3.49% above the frozen
  RC baseline and below Boehm in every pair. Ordinary background services were
  active in the user-accepted measurement window. Median full-suite wall time:
  3.785 seconds RC, 4.510 seconds Boehm.

This is a source release. Build with `make CC=clang` or `nix build .#shen-c`.
`bin/shen-c` and `bin/shen-rc` run the same standalone implementation. CLI
commands remain available; borrowed/owned C API contracts are preserved.

The measured runtime/compiler checkpoint is `8e01579`; the release changes its
displayed implementation version and packaging metadata to 0.3.0. Historical
42.2 main changes are superseded by the qualified 43.1 source tree. Release
candidate CI verifies the final integrated tree independently. Exact results,
reproduction commands, rejected candidates and platform limits are in the
[final qualification report](docs/performance/rc-43.1-final-20261017.md).
