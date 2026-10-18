# String and vector fusion exploration

Base: `802e724` (qualified v0.3.0 code, subsequent README correction).
Isolated branch: `perf/string-vector-fusion`. Preserve all released artifacts,
the pinned 43.1 kernel/suite bytes and earlier worktrees.

1. Trace upstream `@s`/`@v` construction and pattern decomposition into KL;
   retain concrete lowered forms. Characterize primitive payload allocation
   and copying separately from source parsing, compilation and execution.
2. Measure string concatenation, suffix scanning, recursive reconstruction,
   and vector construction/tail scanning at increasing sizes. Include aliases,
   mutation, error ordering, dynamic redefinition and embedded-NUL controls.
   Gather diagnostic byte/call counts separately from uninstrumented timing.
3. Try one bounded runtime candidate first: eliminate `cn`'s temporary buffer
   by allocating its final string payload once. This is primitive copy
   elimination, not general expression fusion and not a no-allocation claim.
   Preserve borrowed/owned C contracts and all context/error checks.
4. Run affected fatal ASan/UBSan controls, default/boxed runtime checks and the
   unchanged 136-test suite. Compare matched release builds. Retain all samples
   and rejected attempts; do not give diagnostic timings performance credit.
5. Write the decision record for deeper candidates: suffix views with bounded
   retention; guarded string-builder/cursor lowering; fresh vector builders;
   vector fusion requiring uniqueness/effect and alias proofs. Identify what
   each can change asymptotically and which guarantees remain unimplemented.
6. Candidate B explores suffix sharing behind `SHEN_RC_STRING_SLICES=1`, off
   by default. Views have a single direct owning-string edge, preserve NUL
   termination, copy below 64 bytes or above 4x backing retention, and remain
   visible to exact cycle accounting. Qualify owner escape, pooled reuse,
   mixed cycles and full generated-kernel sanitizers before timing. This is
   the string-decomposition experiment, not general compiler fusion.

## Semantic constraints

- Strings are byte sequences; embedded NULs must survive operations. Existing
  `rc_text` callers also expect NUL termination. Arbitrary views must account
  for this contract; `tlstr` suffixes alone end at the existing terminator.
- Vectors are mutable, and `tlv` currently copies storage. A shared vector view
  would change alias/mutation behavior without a proof or a copy boundary.
- Callee lookup, argument effects and intermediate errors must keep their
  ordering. Function names alone cannot justify fused `@s`, `cn`, `@v` or
  helper calls, because Shen permits dynamic redefinition.
- Exact cycle reclamation remains enabled. Adding a managed backing owner to
  text would require edge traversal, graph accounting and destruction changes.
  Tiny slices retaining large buffers must be bounded before memory claims.
- A microbenchmark win is not a complete-suite win. Broad compiler fusion,
  FP² checking and a portable ownership IR are outside the first candidate.
