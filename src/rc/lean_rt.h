#ifndef SHEN_RC_LEAN_RT_H
#define SHEN_RC_LEAN_RT_H
/* Runtime support for "lean C": kernel functions compiled by the lean
 * backend (src/rc/lean.c, docs in DESIGN B1). Generated kernel code includes
 * this header after rc_layout.h. Nothing here changes the value
 * representation, the collector or the interpreter.
 *
 * Contents
 * - Symbols and guards: per-context resolved kernel symbols (hot.lean_syms)
 *   and their canonical function bindings (hot.lean_canon), installed by
 *   rc_lean_install after the bootstrap installs. A primitive site is
 *   open-coded only while its symbol's binding is still canonical; any other
 *   binding (a user redefinition, a compile-kl reader hook) takes the generic
 *   call through that binding.
 * - Continuations: Kref is a stack LK (tag 2) or a heap closure. A stack LK
 *   is materialised into a heap closure at most once, through the per-kfn
 *   trampoline that LEAN_KFN_TRAMPOLINE defines; the closure's code is that
 *   trampoline, so captures hold only values and the parent. KDONE releases
 *   (never frees) the materialised closure, so a K stored by user code stays
 *   valid. */
#include "rc.h"
#include "rc_layout.h"
#include <stdint.h>

#define L_LIKELY(x) __builtin_expect(!!(x), 1)
#define L_UNLIKELY(x) __builtin_expect(!!(x), 0)

static inline const RcContextHot *lean_hot(const RcContext *ctx) { return (const RcContextHot *)ctx; }
/* Borrowed symbol i of the installed kernel table. */
static inline RcValue *lean_sym(const RcContext *ctx, size_t i) { return lean_hot(ctx)->lean_syms[i]; }
/* Symbol i's function binding is still the canonical one. */
static inline bool lean_canonical(const RcContext *ctx, size_t i) {
    const RcContextHot *hot = lean_hot(ctx);
    RcValue *canon = hot->lean_canon[i];
    return canon && hot->lean_syms[i]->as.text.function == canon;
}
/* Same test for a binding snapshot taken before argument evaluation. */
static inline bool lean_is_canonical(const RcContext *ctx, size_t i, const RcValue *fn) {
    RcValue *canon = lean_hot(ctx)->lean_canon[i];
    return canon && fn == canon;
}
static inline bool lean_unsealed(const RcContext *ctx) { return lean_hot(ctx)->lean_unsealed; }

/* ---------- continuations ---------- */
typedef struct { uintptr_t w; } Kref; /* struct: never usable as an RcValue * */
typedef struct LK LK;
typedef RcValue *(*LKFn)(RcContext *, LK *);
#define LK_MAXV 10
struct LK {
    LKFn fn;              /* code; NULL after KDONE in verify builds */
    RcNativeOwned tramp;  /* this kfn's heap-closure trampoline */
    RcValue *heap;        /* materialised closure, or NULL */
    Kref up;              /* parent continuation, or KNONE */
    uint32_t nv, owns;    /* captured values; owns: heap is ours to release */
    RcValue *v[LK_MAXV];  /* borrowed captures, rooted by the creating frame */
};
static inline Kref lean_knone(void) { return (Kref){0}; }
static inline Kref lean_kstack(LK *k) { return (Kref){(uintptr_t)k | 2u}; }
static inline Kref lean_kheap(RcValue *closure) { return (Kref){(uintptr_t)closure}; }
static inline bool lean_kis_none(Kref k) { return k.w == 0; }
static inline bool lean_kis_stack(Kref k) { return (k.w & 3u) == 2u; }
static inline LK *lean_kptr(Kref k) { return (LK *)(k.w & ~(uintptr_t)3u); }
static inline RcValue *lean_kclosure(Kref k) { return (RcValue *)k.w; }

/* Define fn##_tramp for kfn fn (declared before use). */
#define LEAN_KFN_TRAMPOLINE(fn) \
    static RcValue *fn##_tramp(RcContext *ctx, RcValue *self, size_t argc, RcValue **argv) { \
        (void)argc; (void)argv; return lean_k_enter(ctx, self, fn, fn##_tramp); \
    }
#define LK_INIT(k, kfn, up_, n) \
    do { (k).fn = (kfn); (k).tramp = kfn##_tramp; (k).heap = NULL; (k).up = (up_); \
         (k).nv = (uint32_t)(n); (k).owns = 0; } while (0)
#ifdef SHEN_RC_VERIFY_OWNERSHIP
#define LEAN_KPOISON(k) ((k).fn = NULL)
#else
#define LEAN_KPOISON(k) ((void)0)
#endif
#define KDONE(k) do { if (L_UNLIKELY((k).owns)) { rc_release((k).heap); (k).heap = NULL; (k).owns = 0; } LEAN_KPOISON(k); } while (0)

/* Rebuild a stack frame from a materialised closure and run its code. The
 * closure stays borrowed (owns=0): the caller of the trampoline roots it. */
static inline RcValue *lean_k_enter(RcContext *ctx, RcValue *self, LKFn fn, RcNativeOwned tramp) {
    size_t n = self->as.closure.capture_count;
    RcValue **c = self->as.closure.captures;
    if (L_UNLIKELY(n < 1 || n > LK_MAXV + 1)) return rc_error(ctx, "invalid lean continuation");
    LK k;
    k.fn = fn; k.tramp = tramp; k.heap = self; k.owns = 0;
    k.up = rc_is_nil(c[0]) ? lean_knone() : lean_kheap(c[0]);
    k.nv = (uint32_t)(n - 1);
    for (size_t i = 1; i < n; i++) k.v[i - 1] = c[i];
    return fn(ctx, &k);
}
/* Borrowed heap closure for k: owned by its LK (stack form) or by whoever
 * passed the heap form. NULL with the error set on failure. */
static inline RcValue *lean_materialize(RcContext *ctx, Kref k) {
    if (lean_kis_none(k)) return rc_error(ctx, "invalid lean continuation");
    if (!lean_kis_stack(k)) return lean_kclosure(k);
    LK *p = lean_kptr(k);
    if (p->heap) return p->heap;
    if (!rc_stack_check(ctx)) return NULL;
    RcValue *caps[1 + LK_MAXV];
    caps[0] = rc_nil();
    if (!lean_kis_none(p->up)) { caps[0] = lean_materialize(ctx, p->up); if (!caps[0]) return NULL; }
    for (uint32_t i = 0; i < p->nv; i++) caps[1 + i] = p->v[i];
    RcValue *h = rc_closure_owned(ctx, 0, p->tramp, 1 + p->nv, caps);
    if (!h) return NULL;
    p->heap = h; p->owns = 1;
    return h;
}
/* (thaw K): owned result, or NULL with the error set. */
static inline RcValue *lean_thaw(RcContext *ctx, Kref k) {
    if (L_LIKELY(lean_kis_stack(k))) { LK *p = lean_kptr(k); return p->fn(ctx, p); }
    if (lean_kis_none(k)) return rc_error(ctx, "invalid lean continuation");
    return rc_apply(ctx, lean_kclosure(k), 0, NULL);
}
#endif
