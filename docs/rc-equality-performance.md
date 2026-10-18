# Equality optimization evidence

Scope: runtime equality against baseline `7933147781b616e1772a35c0d0a2123e23a4511e`.
No full-kernel or language-suite measurement was run in this lane.

The candidate compares distinct same-context interned symbols by identity,
retains textual equality across contexts, and compares scalar children without
putting them in a structural worklist. Flat immutable lists use an allocation-free
iterative walk. Nested pair trees have a bounded explicit stack and branching
budget; vector/cycle/shared-graph fallback uses an iterative worklist and a hash
set of structural node pairs. The fallback preserves equality by unfolding rather
than requiring the graphs to have identical sharing topology.

Focused ASan/UBSan execution passed with immediate integers enabled and disabled.
The test covers 100,000-element lists, 100-deep nested heads, 30-level shared DAGs,
shared versus unshared trees, 128-node cyclic vector graphs, mixed pair/vector
cycles, differing graph shapes with equal unfoldings, mismatches after worklist
and hash growth, cross-context symbols/strings, embedded NUL strings, exact
integer limits, mixed exact/float rounding boundaries, infinity, and NaN. Existing
NaN object identity behavior is preserved. The equality API still reports fallback
allocation/size failures through the left context; allocation failure injection
was not performed.

`test/rc/test_equal.c` runs assertions by default and its microbenchmarks with
any additional argument. It is registered in CMake, Makefile.rc, and test-rc.sh.

## Microbenchmarks

Darwin arm64, Apple CLT clang, `-std=c17 -O3 -DNDEBUG`, same source harness
linked separately against baseline and candidate runtime. Three serial runs each
in alternating order; elapsed CPU seconds from `clock()`. These are short focused
measurements, not full-program speedups. Medians:

| Case | Comparisons | Baseline seconds | Candidate seconds | Ratio |
| --- | ---: | ---: | ---: | ---: |
| Different same-context symbols | 5,000,000 | .033565 | .028326 | 1.18x |
| Equal strings across contexts | 2,000,000 | .019591 | .019894 | .98x |
| Equal 64-element lists | 10,000 | .069529 | .002147 | 32.4x |
| Equal 1024-element lists | 100 | .128102 | .000617 | 207.6x |
| Equal 128-node vector rings | 1,000 | .007600 | .002593 | 2.93x |
| Equal 30-level doubly shared DAGs | 1,000 | .000833 | .000185 | 4.50x |

The string result is effectively unchanged within the resolution and runtime
variation of this short benchmark. Whole-program impact remains to be measured.
