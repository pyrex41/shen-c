# Canonical builtin identities and tiny owned entry points

The emitter selects separate fixed-arity entry points for `hd`, `tl`, `cons?`,
`number?`, `string?`, `=`, and eight arithmetic/comparison operations
(`+`, `-`, `*`, `/`, `>`, `<`, `>=`, `<=`). Their small bodies avoid generic opcode decoding,
expected-arity dispatch, and the large primitive switch. Other primitive calls
keep the existing dispatcher. Lexical shadows and calls with different arities
also keep ordinary dispatch.

Each context owns a fixed fourteen-entry canonical closure table. Successful builtin
installation stores the newly created original closure in its corresponding
slot, retaining the new root before releasing the previous root. This adds 112
bytes per context on 64-bit hosts and no bytes per managed object. The table
never grows, allocates no separate backing storage, and holds at most fourteen extra
closures that have been displaced by redefinition. Repeated installation replaces
these roots rather than accumulating previous versions.

## Safety contract

- The actual selected function is resolved and rooted before argument evaluation,
  preserving redefinition timing and snapshots. The specialization compares that
  exact pointer against the context's canonical root.
- An exact pointer match proves native callback, unbound/original arity, and
  builtin opcode because the private installer only registers original immutable
  builtin closures. Partial closures, replacements, other canonical operations,
  foreign functions, and old snapshots after reinstallation cannot match.
- Every argument still passes same-context validation. Null arrays or slots use
  ordinary checked dispatch, which consumes arguments and reports the error.
- Public specialized entry points consume and clear their fixed number of owned
  argument slots on success and failure. The selected function remains borrowed.
- Ordinary `hd` and `tl` calls drive projected managed tail requests, including
  deferred boolean guards. Tail-position projections return the request to the
  existing trampoline. No side-effecting call is repeated.
- Canonical closures are context roots, represented by retained reference counts
  exactly like context bindings. They remain visible to exact cycle collection.
  Context teardown invalidates and frees all objects through the existing path.

`rc_internal.h` contains the trusted installer/cache contract; it is not an
embedding API. Generated Makefiles track this header as a dependency.

## Validation

All 16 focused runtime executables pass fatal AddressSanitizer and
UndefinedBehaviorSanitizer in both default immediate and boxed-integer builds.
Canonical identity tests cover the original six entry points; consumed/null/foreign arguments;
partial, replaced, and other-operation callees; pending errors; ordinary versus
tail projections; deferred boolean guards; ten repeated builtin installations
with stable live-object counts; and old snapshots across reinstallation.

The existing 20-case generated ownership differential also passes fatal
sanitizers, including aliases, handlers, lexical shadows, binding replacements,
and resolution before argument effects. No full kernel or full language-suite
build is required for these checks.

The targeted `test_canonical_emit.sh` program additionally passes 11 differential
results under fatal sanitizers, checks all six specialized calls appear only in
optimized output, and covers lexical shadows plus redefinition during argument
evaluation. Empty-argument `hd` emission retains ordinary dispatch.

Arithmetic and comparison validation and measurements are documented separately
in `canonical-numeric.md`.

## Initial six-operation instruction measurements

The initial candidate was commit `3aa882f8a466cd228d59b7262428b9971f65150c`.
Baseline `4eb6b7c483a3543abdcdc5f35fa7a29753ff1e0f` and candidate used Apple
clang 21.0.0 on arm64 Darwin, `-O2 -g -std=c17`, no LTO or sanitizers. Each
operation ran 3,000,000 calls twice per binary, with reversed run order in the
second pass. Counts include startup/teardown, negligible at this denominator.
`test/rc/bench_canonical_primitives.c` isolates dispatch without a kernel build.
Raw process counters are in `canonical-primitive-measurements.json`.

| Operation | Before instructions/call | Tiny API | Reduction |
|---|---:|---:|---:|
| `hd` | 295.18 | 208.98 | 29.2% |
| `tl` | 276.16 | 190.08 | 31.2% |
| `cons?` | 289.20 | 201.30 | 30.4% |
| `number?` | 267.35 | 179.11 | 33.0% |
| `string?` | 289.50 | 201.07 | 30.6% |
| `=` (same pair pointer) | 385.43 | 285.58 | 25.9% |
| Integer `+`, identical generic caller control | 328.288 | 328.200 | 0.027% |

Managed allocation and retain/release counts match for every operation. Predicate
and equality runs lazily allocate one boolean symbol per process in both builds.
The integer control deliberately compiles the same generic benchmark caller
against both runtimes; this removes caller selection/loop-layout differences
from the control. Running that operation in the benchmark binary containing
both tiny and generic loops changes its caller layout, so those separate counts
are retained in raw evidence but are not used as the runtime control.

This establishes bounded dispatch instruction savings, without an end-to-end
language-suite performance claim. Timing improved in these runs, but host load
and clock changes make retired instructions the clearer evidence.

Example candidate build and run:

```sh
clang -O2 -g -std=c17 -DRC_BENCH_CANONICAL=1 -Isrc/rc \
  test/rc/bench_canonical_primitives.c src/rc/rc.c src/rc/primitives.c \
  src/rc/eval.c src/rc/read.c -lm -o /tmp/bench-canonical
/usr/bin/time -l /tmp/bench-canonical 0 3000000
```
