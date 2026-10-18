# Canonical builtin tags for generic application

Generic primitive application, including evaluator calls, can enter the fourteen
existing tiny builtin bodies directly. Original builtin closures receive a
one-byte slot-plus-one tag when installed. The byte occupies existing padding
beside the three header booleans; the managed object remains 136 bytes on the
measured host. Every allocation explicitly resets the byte to zero, including
cached-header reuse. New partial closures therefore start untagged.

The fast probe checks context, heap representation, nonzero tag, closure type,
context identity, tag bounds, current canonical pointer, exact argument count,
and every argument's context. An old snapshot's stale tag cannot match a newly
installed canonical pointer. Untagged values check the common header byte before
closure-specific fields. No new context roots or growing caches are introduced.

Public tiny entry points and generic application share private validated bodies.
Public tiny guards fall directly to the private slow dispatcher on rejection;
the generic probe does the same. Invalid or foreign arguments cannot recurse
between generic and tiny entry points. Result ownership, error cleanup,
safepoints, projected tail requests, and deferred boolean guards retain the
existing behavior. Forced inlining keeps direct tiny calls compact while a fixed
function table provides generic dispatch to the same bodies.

A compile-time table assertion deliberately requires adaptation when new
canonical slots are appended. In particular, adding a canonical `cons` slot
requires its validated body/table entry and an additional named test case.

## Focused validation

All eighteen runtime executables pass fatal AddressSanitizer and
UndefinedBehaviorSanitizer with default immediate integers and boxed integers.
The new generic-entry cases check all fourteen registered slots; foreign and null
arguments; foreign functions; wrong arity; old snapshots after reinstallation;
user and partially bound closures; and reusing a displaced builtin's cached
header for another closure and a nonfunction. Existing numeric differential,
evaluator, tail, boolean guard, ownership, alias, environment, and error cases
also pass. Object-cache tests report the unchanged 136-byte block size. The new
test is registered in Make, CMake, and the runtime validation script.

No full kernel or language-suite performance result is claimed for this patch.

## Bounded instruction evidence and tradeoffs

Baseline `e28bee990468334166765db8c1cf56bd0ea07d4e` and candidate used Apple
clang 21.0.0, arm64 Darwin, `-O3 -flto=thin -std=c17`, no sanitizers. Each mode ran
1,000,000 calls twice per binary, reversing mode and binary order for the second
pass. Counts include process setup and teardown. The identical benchmark caller
is compiled against both runtimes. Raw counters and reference-count/allocation
outputs are in `generic-builtin-tag-measurements.json`.

| Generic operation | Before instructions/call | Tagged | Change |
|---|---:|---:|---:|
| `hd` | 258.16 | 166.47 | -35.52% |
| `tl` | 237.56 | 147.15 | -38.06% |
| `cons?` | 251.70 | 158.78 | -36.92% |
| `number?` | 225.86 | 133.52 | -40.88% |
| `string?` | 252.56 | 159.20 | -36.97% |
| `=` | 330.84 | 227.34 | -31.29% |
| `+` | 243.48 | 174.47 | -28.34% |
| `-` | 245.37 | 173.89 | -29.13% |
| `*` | 246.86 | 175.80 | -28.79% |
| `/` | 249.90 | 180.13 | -27.92% |
| `>` | 270.53 | 199.30 | -26.33% |
| `<` | 271.72 | 202.52 | -25.47% |
| `>=` | 270.75 | 201.48 | -25.58% |
| `<=` | 271.94 | 199.65 | -26.58% |
| Untagged user closure | 328.50 | 333.79 | +1.61% |
| Untagged `cons` | 462.29 | 466.75 | +0.97% |
| Partial `+` | 660.70 | 673.55 | +1.95% |
| Old `hd` snapshot after reinstall | 257.95 | 279.24 | +8.25% |
| Direct tiny `hd` control | 134.02 | 134.71 | +0.52% |

Managed allocations and retain/release counts match in every mode. The added
probe costs instructions on untagged and stale-tag fallback calls. These costs
are explicit tradeoffs against the canonical generic-call savings; a complete
language-suite comparison must decide the net result. Host timing was noisy and
is retained as raw evidence without a speedup claim.

Example bounded build and run:

```sh
clang -O3 -flto=thin -std=c17 -Isrc/rc \
  test/rc/bench_generic_builtin_tag.c src/rc/rc.c src/rc/primitives.c \
  src/rc/eval.c src/rc/read.c -lm -o /tmp/bench-generic-tags
/usr/bin/time -l /tmp/bench-generic-tags 0 1000000
```
