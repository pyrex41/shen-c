#ifndef SHEN_C_CONTEXT_H
#define SHEN_C_CONTEXT_H

#include <stddef.h>

#include "gc.h"

/*
 * Foundation heap context. KLObject graphs are allocated with Boehm GC
 * (Nix pkg-config bdw-gc). Option 5 rung 1 generated C keeps allocations
 * on this context / Boehm — not Chicken-on-C-stack, not malloc/free
 * KLObject, not C++.
 *
 * Public embedding/codegen calls (intern, cons, hd/tl, numbers, strings,
 * symbols, trap-error, apply, NativeFunction defun registration, eval-kl
 * for needs-eval shakes) are declared in abi.h. Include abi.h from
 * generated C and link libshenc.
 *
 * Rung 1 generated NativeFunctions self-tail via goto and other tails via
 * shen_tail_apply bouncing into shen_apply (bounce state is saved across
 * nested apply). Non-tail natives hop to a fresh C stack near the thread
 * stack bound. Lifetime-inserted non-GC C
 * (rung 2, no eval-kl) is a later stage; this type is the GC allocation root.
 */
typedef struct shen_context {
  int gc_ready;
} shen_context;

extern shen_context shen_root_context;

inline void shen_context_init (shen_context* ctx)
{
  /* Heap cons/symbol/string tags (kl.h 2/4/6). No-op if interior
   * pointers are on (Nix bdw-gc default); required if they are off. */
  GC_register_displacement(2);
  GC_register_displacement(4);
  GC_register_displacement(6);
  GC_init();
  ctx->gc_ready = 1;
}

inline void* shen_gc_malloc (shen_context* ctx, size_t size)
{
  (void)ctx;
  return GC_malloc(size);
}

inline void* shen_gc_malloc_atomic (shen_context* ctx, size_t size)
{
  (void)ctx;
  return GC_malloc_atomic(size);
}

inline void* shen_gc_realloc (shen_context* ctx, void* pointer, size_t size)
{
  (void)ctx;
  return GC_realloc(pointer, size);
}

#endif
