# Native optimization experiments before tail correction

These receipts cover the predicate prototype, rejected oversized pack, pruned
three-pair experiment, compiler profile and the integrated ea1ae24 checkpoint.
They are historical intermediate results. Whole-suite performance acceptance
remains OPEN; the noisy CPU observations do not establish a speedup.

The integrated source passed the complete suite but later failed the deeper
identity-tail emitted control. Its failure log is retained. The correction and
new qualification are recorded [separately](../native-tail-qualification-20261019/README.md).
A sanitizer preparation whose source changed during main integration is also
retained with status FAIL. The boxed build with inconsistent stale flags is a
rejected setup, followed by a fresh consistently boxed build and passing tests.

Portable receipts redact home paths in keys and values. Original local hashes
are recorded in the receipts/provenance; compressed logs preserve the relevant
output after the same path replacement. Machine code stays local. See the
[experiment and continuation report](../../performance/native-optimization-20261019.md)
for exact scope, retained/rejected measurements and reproduction commands.
