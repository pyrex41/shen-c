# Native tail qualification, 2026-10-19

Source checkpoint: **55bf4a9**. Correctness and diagnostic evidence are separate
from whole-suite performance acceptance, which remains **OPEN**.

The emitter forwards pending tail requests through certified second-argument
identities and preserves boolean completion guards. Arbitrary redefinitions
retain eager checked application. This fixes the native `and`/`do` stack
overflow found after the preceding complete-suite checkpoint.

- Default and boxed runtimes: all 26 runtime controls plus strict native compiler
  controls pass under fatal ASan/UBSan and ownership verification.
- Deep tail control: 17 outcomes, both ownership lowerings, both native and
  diagnostic interpreter execution; default and boxed sanitizers pass. Native
  runs explicitly require zero interpreter entries.
- Emitted controls: 87 semantics outcomes, 49 condition outcomes, shared-frame
  chunks, constant data, ownership/alias/reuse, primitive snapshots, 24 canonical
  primitive outcomes and 36 guarded Prolog outcomes pass under fatal sanitizers.
- Source compilation covers packages, macros, reader global/property effects,
  datatypes, synonyms, NUL literals/reflection, raw defun timing, closures and
  invalid input. Empty-directory kernel startup, dependency audits and release
  validator adversarial controls pass. Focused CMake sanitizer tests pass.
- `release-suite.json` and `sanitize-suite.json` bind the complete pinned 43.1
  suite, expected redirect output, source, binary, input and generated modules.
  Both require 136/0, identical normalized output, zero interpreter entries,
  zero compiler calls and unchanged source/binary hashes. Plain packed images
  and build identities are recorded separately.

`counter-control.json` compares 15,000 loaded simple typechecks with the frozen
pre-optimization native binary: retains fall from 56,805,000 to 51,975,000
(8.50%); releases from 62,205,000 to 57,345,000 (7.81%). No timed compiler CPU or
interpreter activity occurs. This one pair overlapped other work and sampling;
its CPU ratio receives no acceptance credit. `system_allocations` counts managed
object allocator calls, not every C malloc.

The accepted `profile-result.json` binds a separate three-second sample during
a checked million-call typecheck loop with a billion-inference budget. Its 1,031
main-thread samples still place release edges (57), owned tail creation (53),
request dispatch (47), release (45), edge traversal (42), collection (33),
closure creation (32) and argument consumption (28) among the largest collapsed
leaves. These are exclusive leaves, not additive inclusive timings. A profiler
command that exited zero without publishing its stack report is retained as
`profile-missing-stack-rejected-result.json` with status FAIL.

Portable JSON and compressed logs replace home paths in keys and values.
`archive-provenance.json` preserves the original local receipt/log hashes;
`build.json` records the compiler, SDK, flags, sources, emitted kernel C, link
inputs, binary hashes and direct dependencies. Generated binaries/cache images
stay local; the image receipts include their hashes and canonical inventories.

Reproduction commands and the next measured compiler ownership experiment are
in [the continuation report](../../performance/native-optimization-20261019.md).
Current Linux/Nix/macOS CI follows the branch push. Earlier green CI does not
qualify this source. A matched current 16-module Boehm build and two quiet-host
five-pair batches remain required for the performance gate.
