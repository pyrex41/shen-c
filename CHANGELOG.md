# Changelog

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
