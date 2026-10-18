# Managed evaluator frames

The interpreted evaluator now stores lexical scopes in immutable `RC_ENV`
objects. A function's parameters share one frame; each `let` adds one frame with
an inline key/value binding. Lookup scans newer frames first and bindings in
reverse order, preserving the previous prepend order and lexical shadowing.
Closures capture the current frame through the ordinary closure capture API.

An environment owns its parent and values. Symbol keys borrow the context's
intern roots, which remain stable until context destruction. Construction
validates the parent, keys, and values against the context. Frames have no
mutation API. All constructor fields are initialized even when the object comes
from the allocation cache.

The cycle collector traces parent and value edges, includes them in its graph
budget, and releases them during ordinary destruction and cycle reclamation.
Frames propagate the existing `reaches_vector` flag from parents and values, so
the vector write that closes a vector/closure/environment cycle remains visible
to cycle detection. No graph edges live outside managed objects.

The union still fits the existing 136-byte value header. A one-binding frame
uses no separate payload: 136 bytes replaces two 136-byte pair objects. An
eight-parameter frame uses one header plus a 128-byte packed key/value payload,
264 bytes compared with 2,176 bytes for the previous 16 pairs. Zero-argument
calls retain the captured parent without allocating a frame.

## Focused evidence

ASan/UBSan checks pass with both immediate integers and boxed fallback:

- Existing evaluator, 20-case cycle runtime, automatic collection, and object
  cache tests.
- New environment tests: inline and packed frames, captured parents, nested
  shadowing, ten parameters, partial applications, function and special-form
  redefinition, type annotations, trapped errors, and repeated error cleanup.
- Rooted closure aliases survive collection of a
  `closure -> child environment -> parent environment -> vector -> closure`
  cycle. Dropping the last root reclaims the entire cycle; repeated runs return
  to the exact live-object baseline.
- Acyclic policy rejects the same closing edge without modifying the vector.

The bounded O3 microbenchmark uses eight interpreted parameters, two local lets,
and 20,000 tail iterations. Parsing and bootstrap are excluded. Twelve runs in
repeated baseline/candidate/candidate/baseline order measured median 0.045501
seconds for the prior evaluator and 0.027246 seconds with frames (1.67x). Managed
allocations fell from 420,017 to 80,002, and environment pair allocations from
400,016 to zero. These short measurements establish the local effect only;
complete-kernel and canonical-suite performance remain unmeasured.

Local evidence is retained under `evidence/rc/env-frames`: `micro.json`, the
benchmark driver and build logs, source hashes, and sanitizer logs. The baseline
is `28c2bcb` plus the interpreted call/dispatch change `4f62bcc` (locally applied
as `0f4a5d4`). LeakSanitizer is disabled for these Darwin runs.
