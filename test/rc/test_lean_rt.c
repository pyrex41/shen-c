/* Lean C runtime scaffolding (src/rc/lean_rt.h): the per-context symbol and
 * canonical-binding tables with their guards, the unseal switch, and stack /
 * heap continuations with per-kfn trampolines. */
#include "rc.h"
#include "primitives.h"
#include "lean_rt.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const names[] = {"hd", "cons?", "lean-test.undefined", "lean-test.mortal"};
static const RcSymbolTable table = {names, 4};

static RcValue *identity(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *argv) {
    (void)ctx; (void)self; (void)argc; return rc_retain(argv[0]);
}

static void guards(RcContext *ctx) {
    assert(rc_install_primitives(ctx));
    RcValue *mortal_sym = rc_symbol(ctx, "lean-test.mortal");
    RcValue *mortal = rc_closure(ctx, 1, identity, 0, NULL);
    assert(mortal && rc_refcount(mortal) != SIZE_MAX && rc_function_set(ctx, mortal_sym, mortal));
    assert(rc_lean_install(ctx, &table));
    RcValue *hd = lean_sym(ctx, 0);
    assert(hd == rc_symbol(ctx, "hd") && !strcmp(rc_text(lean_sym(ctx, 1)), "cons?"));
    /* Primitives are immortal: canonical. Undefined and mortal: never. */
    assert(lean_canonical(ctx, 0) && lean_canonical(ctx, 1));
    assert(!lean_canonical(ctx, 2) && lean_hot(ctx)->lean_canon[2] == NULL);
    assert(!lean_canonical(ctx, 3) && lean_hot(ctx)->lean_canon[3] == NULL);
    /* Rebinding and restoring (as compile-kl's reader capture does). */
    RcValue *original = rc_function_get(ctx, hd);
    assert(lean_is_canonical(ctx, 0, original));
    RcValue *other = rc_closure(ctx, 1, identity, 0, NULL);
    assert(rc_function_set(ctx, hd, other));
    assert(!lean_canonical(ctx, 0) && lean_is_canonical(ctx, 0, original) && !lean_is_canonical(ctx, 0, other));
    assert(rc_function_set(ctx, hd, original));
    assert(lean_canonical(ctx, 0));
    /* Reinstalling replaces the tables. */
    assert(rc_lean_install(ctx, &table) && lean_canonical(ctx, 1));
    rc_release(other); rc_release(original); rc_release(mortal); rc_release(mortal_sym);
}

/* A kfn: returns its first capture consed onto (thaw up), or onto () at the
 * root. Exercises captures, the parent chain and thaw of both forms. */
static RcValue *kfn_cons(RcContext *ctx, LK *k);
LEAN_KFN_TRAMPOLINE(kfn_cons)
static RcValue *kfn_cons(RcContext *ctx, LK *k) {
    assert(k->fn == kfn_cons && k->tramp == kfn_cons_tramp && k->nv == 1);
    RcValue *rest = lean_kis_none(k->up) ? rc_nil() : lean_thaw(ctx, k->up);
    if (!rest) return NULL;
    RcValue *r = rc_cons(ctx, k->v[0], rest);
    rc_release(rest);
    return r;
}

static void continuations(RcContext *ctx) {
    size_t live0 = rc_stats(ctx).live_objects;
    RcValue *a = rc_integer(ctx, 1), *b = rc_string(ctx, "two");
    LK outer, inner;
    LK_INIT(outer, kfn_cons, lean_knone(), 1); outer.v[0] = a;
    LK_INIT(inner, kfn_cons, lean_kstack(&outer), 1); inner.v[0] = b;
    Kref k = lean_kstack(&inner);
    assert(lean_kis_stack(k) && lean_kptr(k) == &inner && !lean_kis_none(k));
    /* Stack thaw: a direct call. */
    RcValue *r = lean_thaw(ctx, k);
    assert(r && rc_type(r) == RC_PAIR && rc_head_borrowed(r) == b);
    assert(rc_head_borrowed(rc_tail_borrowed(r)) == a && rc_is_nil(rc_tail_borrowed(rc_tail_borrowed(r))));
    rc_release(r);
    /* Materialise once; the parent chain too; the code is the trampoline. */
    RcValue *h = lean_materialize(ctx, k);
    assert(h && rc_type(h) == RC_CLOSURE && rc_closure_is_owned_native(h, kfn_cons_tramp));
    assert(lean_materialize(ctx, k) == h && inner.owns && outer.owns && outer.heap);
    assert(rc_capture(h, 0) == outer.heap && rc_capture(h, 1) == b && rc_capture(outer.heap, 0) == rc_nil());
    /* A user stores the closure, then the frames finish: it stays valid. */
    RcValue *stored = rc_retain(h);
    KDONE(inner); KDONE(outer);
    assert(!inner.owns && !inner.heap);
    RcValue *again = rc_apply(ctx, stored, 0, NULL);
    assert(again && rc_head_borrowed(again) == b && rc_head_borrowed(rc_tail_borrowed(again)) == a);
    rc_release(again);
    /* Heap thaw. */
    again = lean_thaw(ctx, lean_kheap(stored));
    assert(again && rc_head_borrowed(again) == b);
    rc_release(again);
    rc_release(stored);
    rc_release(a); rc_release(b);
    rc_collect(ctx);
    assert(rc_stats(ctx).live_objects == live0);
    /* KNONE is invalid to thaw or materialise, with an error. */
    assert(!lean_thaw(ctx, lean_knone()) && rc_has_error(ctx));
    rc_clear_error(ctx);
    assert(!lean_materialize(ctx, lean_knone()) && rc_has_error(ctx));
    rc_clear_error(ctx);
}

static void unsealed(void) {
    unsetenv("SHEN_RC_UNSEALED");
    RcContext *ctx = rc_context_new(); assert(ctx && !lean_unsealed(ctx)); rc_context_free(ctx);
    setenv("SHEN_RC_UNSEALED", "0", 1);
    ctx = rc_context_new(); assert(ctx && !lean_unsealed(ctx)); rc_context_free(ctx);
    setenv("SHEN_RC_UNSEALED", "1", 1);
    ctx = rc_context_new(); assert(ctx && lean_unsealed(ctx)); rc_context_free(ctx);
    unsetenv("SHEN_RC_UNSEALED");
}

int main(void) {
    RcContext *ctx = rc_context_new();
    assert(ctx && !lean_hot(ctx)->lean_syms && !lean_hot(ctx)->lean_canon);
    guards(ctx);
    continuations(ctx);
    rc_context_free(ctx);
    unsealed();
    puts("RC lean runtime: guards, unseal switch, stack/heap continuations PASS");
    return 0;
}
