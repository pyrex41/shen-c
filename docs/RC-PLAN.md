# Boehm-free Shen-C implementation

Status: implementation complete for the experimental backend. The complete
bundled S42 suite passes 134/134; validation results and remaining acceptance
limits are recorded in `rc-validation.md`. CLI/extension migration, precise Perceus
ownership inference, reuse optimization, and comparative performance remain
separate work.

The current user request authorizes an implementation beyond the earlier
documentation-only rung-2 proposal. The goal is full Shen semantics with an
explicit reference-counted C runtime and exact cycle reclamation. This is
automatic memory management; it is not a claim of no garbage collection.

## Architecture

- A separate `src/rc` runtime owns its values, heap edges, globals, function
  bindings, closures, and mutable vectors. Public arguments are borrowed;
  value results are owned. Errors are explicit, not nonlocal C jumps.
- Reference counts reclaim acyclic garbage; an exact cycle pass reclaims
  unreachable cyclic graphs without scanning the C stack.
- A new native emitter lowers KL functions to C and inserts ownership
  operations. Runtime `eval-kl` uses a separate RC evaluator when invoked.
  Native compilation must not become a wrapper around source evaluation.
- The compiler host may use the existing Boehm-backed KL parser. Generated
  applications must link only the new runtime and ordinary C dependencies.
- Existing interpreter/AOT artifacts remain available as a compatibility
  baseline. Full compatibility is a goal, not assumed from a small fixture.

## Team ownership

| Lane | Owned paths |
| --- | --- |
| Runtime | `src/rc/rc.h`, `src/rc/rc.c`, heap unit tests |
| Compiler | `src/c/emit_rc.*`, `tools/rc-build.c` |
| Primitives | `src/rc/primitives.*`, primitive tests |
| Dynamic evaluation | `src/rc/eval.*`, `src/rc/read.*`, evaluator tests |
| Independent validation | cycle tests, validation scripts and inventory |
| Coordinator | build integration, documentation, integration fixes, full checks |

All lanes use the isolated `feat/perceus-rc` worktree based on `cc07d10`.
They coordinate the runtime header before relying on its ABI. Heavy integration
builds and full suites are serialized by the coordinator.

## Execution and acceptance

1. Agree runtime ownership and native-call ABI; implement the heap and cycles.
2. Implement primitives and dynamic evaluation with explicit cleanup.
3. Implement direct C emission, closures, exception paths, and tail calls.
4. Integrate builder and run focused ASan/UBSan ownership tests.
5. Run generated fixtures and full bundled S42 tests against the same inputs.
6. Inspect linked artifacts for Boehm symbols/dependencies and source for
   genuine native definitions. Review the final diff and record exact results.

Acceptance includes self/two-node/mixed cycles, live aliases, mutation,
global replacement, closure escape, partial/overapplication, deep destruction,
self/mutual tail calls, error unwinding, dynamic definitions and eval, I/O,
and full kernel execution. Allocation statistics must demonstrate reclamation
during execution as well as context teardown.

Precise last-use insertion, reuse optimization, performance, and full-language
coverage are separate claims. Scoped ownership alone is not full Perceus.
Unknown or failed gates remain explicit. No publishing is requested.
