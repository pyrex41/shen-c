# Trial-count and work-link storage sharing

`RcValue` now stores its trial count and intrusive work pointer in one anonymous
union. The common header keeps every persistent field at its previous offset;
only the payload moves eight bytes earlier. On the measured 64-bit host this
shrinks the managed object from 136 to 128 bytes without adding a collector pass,
allocation, or metadata write. The separate visit epoch remains unchanged.

## Phase invariant

1. Exact collection initializes every trial count, then subtracts every graph
   edge. No work links are live during either phase.
2. Root discovery reads the current object's trial once. `mark_edge` pushes only
   that same object, replacing its consumed trial with a work link. It does not
   traverse children or modify another object's unread trial.
3. Transitive marking begins after the entire root-discovery loop. No trial
   counts are read from that point onward. Marking, dead-list construction, and
   freeing use only work links.
4. Ordinary reference-count release and insertion-cycle search use work links
   outside the collector's trial phases. Every link is set before insertion;
   the next collection initializes all trial counts before reading them.

The root-discovery loop includes a source comment documenting this ordering so
future changes do not accidentally interleave child traversal and unread trials.
Independent review found the phase lifetimes disjoint.

## Correctness and accounting

All twenty runtime executables pass fatal AddressSanitizer and
UndefinedBehaviorSanitizer in default and boxed-integer builds. The new test
keeps 2,048 pair roots plus a rooted self-cycle, forcing eight complete collector
passes while checking references, allocation/free totals, exact retain/release
counts, and unchanged survivors. It then drains a deep ordinary release work
list and collects the final unrooted cycle. The test is registered in Make,
CMake, and the validation script.

Collection budgets, graph-slot counts, cycle-source flags, reference operations,
and allocation/free counts are unchanged. Physical byte counters correctly
reflect the smaller actual header. Existing object-cache checks report 128-byte
blocks and 131,072 reserved logical bytes at the unchanged 1,024-block limit.

## Actual allocator size

On this arm64 Darwin host, `malloc_size` reports:

| Managed header request | Allocator usable bytes |
|---:|---:|
| Baseline 136 | 160 |
| Union 128 | 128 |

The requested header shrinks **5.88%**, while the observed allocator block
shrinks **20%**. A full cached set therefore has 131,072 logical header bytes,
down from 139,264. Allocator usable size is a separate measurement and must not
be substituted for the runtime's logical byte counters. Other allocators may
have different size classes.

## Bounded allocation and cycle measurements

Baseline is `8dbc9414a402a148dee0e5c0bde6fd087c7e9fce`. Both binaries use Apple
clang 21.0.0, arm64 Darwin, `-O3 -flto=thin -std=c17`, no sanitizer. The longer
control runs reverse variant and mode order in the second pass. Short pilots
and longer raw receipts are preserved in `trial-work-union-measurements.json`.
The benchmark makes no full kernel build and leaves normal cache/statistics
behavior enabled.

| Workload per run | Retired-instruction change | Mean seconds before / after |
|---|---:|---:|
| 10M cached pair allocate/free operations | -0.393% | 0.263719 / 0.284784 |
| 128 lists of 16,384 pairs, allocate and release | +0.936% | 0.169415 / 0.154623 |
| 2M self-cycles, fixed interval 128 | +0.233% | 0.158886 / 0.149570 |
| 10M allocation-free integer additions | +0.059% | 0.128418 / 0.133529 |

Allocation/free, retain/release, collection, and cycle-reclamation counts match
for every workload. The cycle workload performs 15,626 collections and reclaims
all 2M cycle objects in both binaries. Instructions are nearly flat; timings are
mixed and do not establish a speed improvement. The demonstrated result is the
smaller managed and allocator footprint. A full language-suite run remains a
separate performance gate.

Example bounded build and allocation control:

```sh
clang -O3 -flto=thin -std=c17 -Isrc/rc \
  test/rc/bench_trial_work_union.c src/rc/rc.c src/rc/primitives.c \
  src/rc/eval.c src/rc/read.c -lm -o /tmp/bench-trial-union
/usr/bin/time -l /tmp/bench-trial-union 0 10000000
```
