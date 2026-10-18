# Proven acyclic collection fast path

The unrestricted runtime keeps the existing exact cycle collector and allocation
budgets. A collection can skip its heap passes only when no live vector has ever
closed a cycle or exceeded a bounded insertion search. `acyclic_collections`
counts these skips; `collections` still counts every collection opportunity.

## Invariants

1. Only vector assignment can introduce a cycle. Other constructors point to
   existing objects. Pair reuse requires one owned reference, so no other heap
   object can already point to that pair. Tail argument removal only removes
   edges; replacement of a pending tail's dummy function is private.
2. Every vector has `reaches_vector` set. Pairs, closures and tails conservatively
   OR their children's flags, including bound arguments and pending functions.
   Unique pair reuse updates its flag. Removing tail arguments may leave a
   conservative true flag. A node's flag cannot gain a vector path while it has
   a heap parent, because the relevant rewrites require unique ownership.
3. Before a vector gains an edge whose target can reach a vector, the runtime
   checks whether the target reaches the destination vector. A closing edge
   marks that vector as a `cycle_source`, counted until the vector is freed.
   Already marked vectors remain marked through further mutations.
4. The unrestricted insertion check spends at most 256 units, charging each
   visited node and each inspected pointer slot. Exhaustion conservatively
   marks the source. The optional acyclic policy uses an unbounded exact check
   and still rejects actual cycles.

Any cycle therefore includes a live marked source: its last inserted edge was a
vector assignment, and the source cannot disappear while that cycle exists.
Zero counted sources proves the graph acyclic. Immediate reference counting
already frees unreachable acyclic objects, so a skipped collection has no garbage
to reclaim. Rooted cycles retain their source marks across collections; releasing
the last external root still leaves the full collector enabled. Collection
intervals and survivor-based adaptive budgets are unchanged.

A broken cycle or a search-budget false positive can leave a live vector marked.
This only costs future full collections; it cannot retain additional garbage or
hide a cycle. Source counts drop during ordinary release and cycle sweeping.

## Validation

All eight focused runtime executables pass with fatal AddressSanitizer and
UndefinedBehaviorSanitizer instrumentation. The cycle suite covers 20 cases and
252189 assertions. Additional collection cases cover vector paths through pairs,
mutation after immutable construction, pair reuse gaining a vector path, rooted
cycles followed by root release, and bounded-search fallback followed by a real
cycle. Existing adaptive-budget and fixed-interval assertions still pass.

A diagnostic build linked against the baseline generated kernel bootstraps and
evaluates `(+ 1 2)` with four collection opportunities, all four skipped, one
insertion search costing 52 nodes plus 103 slots, and no fallback. This is a
bootstrap applicability observation, not an end-to-end performance result.
