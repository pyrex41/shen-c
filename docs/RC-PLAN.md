# Boehm-free Shen-C implementation

Status: implementation is integrated and locally qualified on Darwin arm64.
The experimental checkpoint `97aa8e1` passed the bundled S42 suite but still
depended on Boehm in the compiler, used conservative ownership, and lacked a
standalone Shen executable. The continuation replaces those components and
adds a native Shen source compilation driver. Exact executed results are kept
in [the validation record](rc-validation.md).

Completed: independent compiler host, standalone embedded-kernel CLI, native
Shen source driver, local ownership insertion, free-variable capture, owned
calls, selective pair reuse, and supported build migration away from Boehm.
The final unchanged S42 suite passed 134/0 with fatal ASan/UBSan. Fresh Make
and CMake qualification, standalone frontend sanitizer checks, native source
fixtures, and dependency audits passed. Nix build/install, Linux execution,
and remote CI remain unqualified; the validation record states those limits.

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
- The compiler host uses the independent RC reader and AST. Compiler, CLI,
  and generated applications link only the RC runtime and ordinary C dependencies.
- Historical interpreter/AOT source remains a comparison reference; supported
  build paths have no Boehm dependency. Compatibility requires full suite evidence.

## Team ownership

| Lane | Owned paths |
| --- | --- |
| Runtime | `src/rc/rc.h`, `src/rc/rc.c`, heap unit tests |
| Compiler | `src/rc/emit.*`, `tools/rc-build.c` |
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

## Continuation after 97aa8e1

1. Replace the Boehm/libshenc compiler frontend with the RC reader and AST.
   Build and audit both compiler and emitted applications without Boehm.
2. Insert last-use moves/drops, capture only free variables, and introduce
   owned call boundaries so unique values can remain unique across calls.
3. Reuse unique cons storage in actual emitted programs; measure allocations
   and preserve aliases, errors, partial calls, and cycle safety.
4. Produce a standalone `shen-rc` executable with the actual compiled kernel,
   source loading, evaluation, and REPL; replace the default CLI.
5. Run focused ownership/reuse/CLI checks, canonical unchanged S42 suite,
   sanitizers, and dependency audits against the new source snapshot.

The user explicitly confirmed that cycle reclamation is acceptable and asked
to remove Boehm completely. Retain unrestricted Shen semantics and exact cycle
reclamation by default. An optional acyclic-only build is supplementary, not
a replacement for the default semantics. Published
Perceus does not establish collector-free reclamation of arbitrary mutable
cycles; do not claim that local liveness or reuse removes this requirement.

Compiler owns the AST/frontend migration first, then hands the native emitter
to the ownership lane. Runtime owns reuse and opt-in owned call ABI. Evaluator
owns the CLI. Coordinator owns builds, integration, and full qualification;
validation and review independently check executable evidence and semantics.
