# Immediate exact integers

The RC runtime previously allocated one managed `RcValue` for every integer.
Small exact integers now use opaque handles with bit zero set, eliminating their
heap allocation and reference count writes. This change does not claim a whole
Shen suite speedup; that requires separate measurements of the generated kernel.

## Design and completed implementation plan

1. Audit the only concrete `RcValue` definition and its dereferences in `rc.c`.
2. Route value type dispatch through `rc_type`; guard heap ownership and graph
   callbacks before accessing a pointer payload.
3. Encode exact integers in `[-2^62, 2^62-1]` on conventional 64-bit ARM64 and
   x86-64 targets. Retain boxed numbers on other targets and with
   `SHEN_RC_IMMEDIATE_INTEGERS=0`.
4. Keep the full `int64_t` domain exact through boxed fallback, and keep every
   `rc_number(double)` boxed. Preserve existing mixed exact/inexact comparison
   and arithmetic overflow behavior.
5. Exercise representation boundaries, API rejection paths, arithmetic, printing,
   cycles, acyclic graph walks, and ownership in both representation modes.

`calloc` storage is aligned to at least two bytes, asserted against `RcValue`'s
alignment. Encoding casts to unsigned before shifting, so negative values never
undergo a signed shift or overflow. Decoding shifts an unsigned word, converts
only representable pieces to `int64_t`, then subtracts bit 62's sign contribution
from the lower 62 bits. The result spans exactly the immediate range.

Only the runtime interprets handles. Immediates never enter the managed object
list. Retain/release and every collector edge callback skip them; graph slots
still account for the actual pointer slots that contain these handles. Numeric
equality reads exact integers through accessors before examining boxed doubles.

## C API and statistics

Nil and immediate integers are immutable and context independent. They may be
used across contexts and remain valid after a context is freed. Heap values,
including boxed integers outside the immediate range and all doubles, retain
the existing single-context ownership restriction. Pointer identity is not a
substitute for numeric equality. A non-NULL context is still required by
`rc_integer` even when its result needs no allocation.

`rc_refcount` reports `SIZE_MAX` for immortal handles. Allocation, live storage,
and retain/release counters describe physical heap activity; immediates do not
increase them. Existing foreign-context and reference-count tests use boxed
doubles to continue exercising heap ownership. The unused-capture emitted test
requires reclamation of the 200 pair cells in its captured list; immediate
integer leaves no longer add another 200 managed objects.

The implementation relies on the integer-to-pointer representation supported by
the selected platforms, rather than claiming portable ISO C pointer provenance.
The boxed compile option is available for environments with different pointer
requirements.

## Validation on 2026-10-15

Apple Command Line Tools Clang, C17, macOS SDK, `-O2 -g`:

- All nine runtime C binaries passed with immediate integers enabled.
- All nine passed with `-DSHEN_RC_IMMEDIATE_INTEGERS=0`.
- All nine passed under AddressSanitizer and UndefinedBehaviorSanitizer with
  `-fno-sanitize-recover=all`. Darwin leak sanitizer was disabled because it is
  unsupported; no leak-sanitizer result is claimed.
- The cycle runtime reported 20/20 cases and 252189 assertions; ownership/reuse
  reported 9/9 cases; the optional acyclic policy reported 7/7 cases.
- The bounded emitted ownership test passed all 20 differential cases.
  Conservative versus optimized physical live objects were 340 versus 140;
  pair reuse was 102 and capture edges created were 76 versus 72.
- The focused new test covers immediate endpoints and adjacent fallback values,
  `INT64_MIN/MAX`, exact integers beyond double precision, mixed numeric equality,
  print round trips, arithmetic transitions/overflow, nonnumeric API guards,
  immediate-as-function rejection, cycles with all graph object kinds, and the
  acyclic walk.

No full generated kernel or full language suite was built in this isolated lane.
