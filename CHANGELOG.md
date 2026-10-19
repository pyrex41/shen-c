# Changelog

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
