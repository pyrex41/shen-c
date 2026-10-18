# Lazy evaluator proper-list length cache

A private getter caches only the requested pair head: unknown, proper length, improper, or overflow. The evaluator uses it for repeated immutable AST argument/parameter/clause lists. Improper-list diagnostics remain `eval-kl: improper expression list`. No tail caches are populated during a scan.

The pair payload gains one word inside the existing union. Static assertions require the payload to fit and 64-bit `RcValue` to remain 128 bytes. Constructors `rc_cons` and `rc_cons_owned`, successful `rc_cons_reuse`, and unique `rc_pair_take_tail_reuse` initialize/reset the cache. Pooled object storage is not assumed cleared. Shared projection does not alter edges.

An ancestor owning a tail prevents that tail from having the single independent owner required for destructive reuse. A live shared head therefore retains valid spine metadata. Mutation of heads/vectors does not alter pair-spine length; unique pair changes reset their own metadata.

## Validation

Fatal ASan/UBSan passed `list_length`, `eval`, `reuse`, `cycles`, `collection`, and `object_cache` in default and boxed-integer modes. The new test covers warmed ancestors/shared tails, shared and unique projection, both reuse mutations, pooled pair/string storage, 1000-cell improper and 10000-cell proper lists, nil, and repeated malformed evaluation with exact diagnostic. Overflow is guarded before increment; its reserved sentinel is unreachable with feasible allocations.

## Controls

Apple clang O3 ThinLTO, same source revision apart from this patch. Five alternating paired CPU samples; three alternating Darwin `time -l` instruction samples. These controls do not establish full-suite performance.

| Control | CPU baseline s | CPU cache s | CPU change | Instructions change |
|---|---:|---:|---:|---:|
| cached-AST-eval | 0.057046 | 0.054448 | -4.55% | -1.16% |
| native-cons-owned | 0.044352 | 0.044990 | +1.44% | +0.46% |
| cold-list-64 | 0.031257 | 0.031727 | +1.50% | +0.37% |
| unique-cons-reuse | 0.025332 | 0.025379 | +0.19% | +0.64% |

The evaluator control executes a cached AST with 200000 dynamic recursive calls. Constructor and reuse controls execute 2000000 iterations; the cold-list control builds, traverses and releases 20000 lists of 64 cells. Cache stores add a measurable constructor instruction cost, so full-suite measurement must decide adoption.

Raw local evidence: `evidence/rc/list-length-cache/receipt.json`, micro sources/results, instruction logs and binary hashes.
