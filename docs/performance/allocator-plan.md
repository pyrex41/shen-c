# Bounded object cache

Reuse at most 1,024 dead `RcValue` allocations per context. Keep pointers in an
external fixed array so ASan can poison every byte of a cached object. Free
variable payloads immediately. Only live objects remain on the collector list.
Reuse unpoisons and zeroes the object before linking it into the live list.
Context teardown frees live objects directly and drains all cached blocks.

Logical object/ownership statistics keep their existing meaning. Additional
statistics report cached block count/bytes/peak and system block allocations and
frees. Cached bytes are separate from live bytes. The context pointer array costs
1,024 pointers; cached storage costs at most 1,024 times `sizeof(RcValue)`.

Validate eight runtime tests plus cache saturation, payload reuse, cycles,
context independence, logical/physical counters and intentional ASan UAF. No
kernel or full suite benchmark until granted a serial benchmark slot.

## Validation

Apple arm64 measured `sizeof(RcValue)` is 136 bytes. Maximum cached object storage
is 139,264 bytes, plus the 8,192-byte context pointer index. Allocator metadata is
additional. `cached_bytes` reports dead reserved blocks separately from live data;
`system_allocations` and `system_frees` count only `RcValue` blocks, not payloads.
The expanded `RcStats` return structure requires recompiling its callers.

Nine runtime tests passed with `-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer`, `ASAN_OPTIONS=halt_on_error=1:malloc_context_size=10` and `UBSAN_OPTIONS=halt_on_error=1`.
The pool test fills the cache past its limit, reuses 1,024 blocks without system
allocation, checks separate contexts, variable payloads, cycles and live teardown.
The intentional `test_object_cache --uaf` negative control aborted with ASan
`use-after-poison`. Cached blocks are poisoned until reuse; after an address is
reused, ASan cannot distinguish an old alias from a current handle. This cache
has no ASan allocation quarantine. Normal system frees apply beyond the bound.

Local receipts are in `evidence/rc/allocator/`. No full kernel or suite performance
claim is established by these checks.
