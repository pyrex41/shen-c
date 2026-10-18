# Comparative profiling follow-up

Preserved checkpoint: `8625781`, branch `feat/shen-43.1-qualification`.
Work happens in the isolated `perf/shen-43.1-profile` worktree. Performance
acceptance remains OPEN. The unchanged official 136-assertion suite is the oracle.

1. Freeze the qualified RC/Boehm executables and their source/generated identities.
2. Build diagnostic-only versions with nested CPU phase accounting. Instrument
   generated/non-tail ABI boundaries, without changing upstream kernel or suite
   bytes. Attribute reading, translation, typechecking, and evaluation exclusively;
   retain unclassified time. Account for Boehm exception unwinding explicitly.
3. Collect complete-process and load-focused Darwin samples from BOTH backends,
   with generated-symbol-to-Shen maps. Keep diagnostics outside timing evidence.
4. Measure an isolated removal of the two retain/release diagnostic increments.
   Preserve actual reference counts, collection budgets, and exact cycle collection.
   Compare to the checkpoint with alternating complete-suite runs.
5. Use the measured CPU budget to rank further work. Retain a candidate only if
   focused correctness, sanitizers, and whole-suite measurements support it.
6. Publish bounded evidence, commands, uncertainty, rejected experiments, and
   the next measured bottleneck. No final acceptance claim without its full gates.
