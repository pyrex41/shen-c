# Primitive dispatch and consuming construction

Exact built-in leaf calls now validate their closure and call inputs in one
runtime helper, decode the opcode once, and pass that opcode into the primitive
body. Validation still requires the current native callback, no bound arguments,
original arity equal to supplied arity, same-context function and arguments, and
a first capture. The selected closure remains borrowed and must stay rooted.
Partials, replacements, invalid inputs, and overapplication use ordinary dispatch.
The emitter still resolves the current function before evaluating arguments.

For exact built-in `cons`, the wrapper clears both owned argument slots and
transfers their references directly into the new pair. `rc_cons_owned` validates
both children and consumes both references on every path, including validation or
allocation failure. Aliasing children require one independently owned reference
per slot. Vector reachability metadata is initialized explicitly, including when
reusing allocator blocks. The borrowed constructor and reuse API are unchanged.

Integer arithmetic classifies exact integer operands before unpacking doubles.
Integer division checks zero and the signed overflow case before modulus; mixed
or inexact division uses the previous double behavior. Overflow errors, exact
quotients, nonintegral quotients, signed zero, infinity, and NaN remain covered.

Pure `hd`, `tl`, and `do` projections retain their existing behavior for managed
tail requests: ordinary calls drive a returned request, while tail calls can
return it to the surrounding trampoline. The patch does not eagerly invoke
arbitrary native closures.

## Focused validation

All 12 runtime executables pass with fatal AddressSanitizer and
UndefinedBehaviorSanitizer, both with default immediate integers and with
`SHEN_RC_IMMEDIATE_INTEGERS=0`. Extended primitive cases cover aliased child
ownership and exact retain/release counts; partial and replaced `cons`;
overapplication; foreign contexts; null inputs; pending errors; vector/pair cycle
reclamation; integer overflow and division; mixed floating arithmetic; NaN and
infinity; and both ordinary and tail-position managed tail projections.

## Bounded microbenchmark

`test/rc/bench_primitive_fusion.c` runs independently of the generated kernel.
Baseline is commit `28c2bcb`; candidate contains only this patch. Both binaries
used Apple clang 21.0.0, arm64 Darwin, `-O2 -g -std=c17`, no LTO or sanitizer.
Each cell is the mean of two process runs of 3,000,000 calls, ordered before/after
and after/before. Retired instructions come from `/usr/bin/time -l` and include
process startup and teardown. Startup is negligible at this denominator.

| Operation | Before instructions/call | After | Reduction |
|---|---:|---:|---:|
| Resolved built-in `hd`, shared heap child | 356.4 | 299.2 | 16.0% |
| Function lookup, `hd`, function release | 419.4 | 362.2 | 13.6% |
| Built-in `cons`, two heap children | 645.7 | 497.9 | 22.9% |
| Built-in exact integer addition | 404.5 | 330.1 | 18.4% |

`cons` allocates exactly one pair per call in both binaries. Its reference-count
traffic drops from four retains/five releases to two retains/three releases per
call. The other operations preserve their allocation and reference-count counts.
Run timings also improved, but host contention and CPU frequency make those
microseconds less useful than the instruction counts. These measurements do not
establish a full language-suite improvement.

Example candidate compilation and one bounded run:

```sh
clang -O2 -g -std=c17 -Isrc/rc test/rc/bench_primitive_fusion.c \
  src/rc/rc.c src/rc/primitives.c src/rc/eval.c src/rc/read.c -lm \
  -o /tmp/bench-primitive-fusion
/usr/bin/time -l /tmp/bench-primitive-fusion 2 3000000
```
