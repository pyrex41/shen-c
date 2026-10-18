/* Automatic cycle collection must remain bounded as the garbage heap grows. */
#include "rc.h"
#ifdef NDEBUG
#undef NDEBUG /* Test checks and their setup must run in Release builds. */
#endif
#include <assert.h>
#include <stdio.h>
static void cycle(RcContext *ctx) {
    RcValue *v = rc_vector(ctx, 1); assert(v);
    assert(rc_vector_set(ctx, v, 0, v));
    rc_release(v);
    rc_safepoint(ctx);
}
static void acyclic_fast_path(void) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    RcValue *leaf = rc_integer(ctx, 42), *pair = rc_cons(ctx, leaf, rc_nil());
    RcValue *v = rc_vector(ctx, 1); assert(leaf && pair && v);
    assert(rc_vector_set(ctx, v, 0, pair));
    rc_collect(ctx);
    assert(rc_stats(ctx).collections == 1 && rc_stats(ctx).acyclic_collections == 1);
    /* A vector reached through a pair can be mutated after construction.
     * The pair must already advertise that vector path before any mutation. */
    RcValue *inner = rc_vector(ctx, 1), *path = rc_cons(ctx, inner, rc_nil());
    assert(inner && path && rc_vector_set(ctx, v, 0, path));
    rc_collect(ctx);
    assert(rc_stats(ctx).acyclic_collections == 2);
    assert(rc_vector_set(ctx, inner, 0, v));
    rc_collect(ctx);
    assert(rc_stats(ctx).acyclic_collections == 2); /* Rooted cycle still tracked. */
    rc_release(path); rc_release(inner); rc_release(v);
    rc_collect(ctx);
    assert(rc_stats(ctx).cycle_objects_freed == 3);
    rc_release(pair); rc_release(leaf);
    rc_collect(ctx);
    assert(rc_stats(ctx).acyclic_collections == 3);
    assert(rc_stats(ctx).live_objects == 0);
    rc_context_free(ctx);
}
static void reuse_vector_path(void) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    RcValue *v = rc_vector(ctx, 1), *pair = rc_cons(ctx, rc_nil(), rc_nil());
    pair = rc_cons_reuse(ctx, pair, v, rc_nil()); assert(pair);
    assert(rc_vector_set(ctx, v, 0, pair));
    rc_release(pair); rc_release(v);
    rc_collect(ctx);
    assert(rc_stats(ctx).cycle_objects_freed == 2);
    assert(rc_stats(ctx).acyclic_collections == 0);
    rc_collect(ctx);
    assert(rc_stats(ctx).acyclic_collections == 1);
    rc_context_free(ctx);
}
static void bounded_write_fallback(void) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    RcValue *v = rc_vector(ctx, 1), *large = rc_vector(ctx, 300);
    assert(v && large && rc_vector_set(ctx, v, 0, large));
    /* The harmless edge exceeds the bounded search budget, so its source is
     * conservatively tracked. Collection remains exact and roots survive. */
    rc_collect(ctx);
    assert(rc_stats(ctx).acyclic_collections == 0);
    assert(rc_stats(ctx).live_objects == 2);
    assert(rc_vector_set(ctx, large, 0, v)); /* Can now close a real cycle. */
    rc_release(large); rc_release(v);
    rc_collect(ctx);
    assert(rc_stats(ctx).cycle_objects_freed == 2);
    assert(rc_stats(ctx).live_objects == 0);
    rc_collect(ctx);
    assert(rc_stats(ctx).acyclic_collections == 1);
    rc_context_free(ctx);
}
int main(void) {
    acyclic_fast_path();
    reuse_vector_path();
    bounded_write_fallback();
    RcContext *ctx = rc_context_new(); assert(ctx);
    for (size_t i = 0; i < 10000; ++i) cycle(ctx);
    RcStats stats = rc_stats(ctx);
    assert(stats.collections == 2 && stats.cycle_objects_freed == 8192);
    assert(stats.live_objects == 1808);
    rc_collect(ctx); assert(rc_stats(ctx).live_objects == 0);
    rc_context_free(ctx);

    /* The frozen survivor budget includes vector pointer slots, so a large
     * rooted vector is not rescanned at every 4096 temporary allocations. */
    ctx = rc_context_new(); assert(ctx);
    RcValue *root = rc_vector(ctx, 10000); assert(root);
    rc_collect(ctx);
    const size_t budget = 4 * (10000 + 1);
    for (size_t i = 0; i < budget - 1; ++i) cycle(ctx);
    assert(rc_stats(ctx).collections == 1);
    cycle(ctx);
    assert(rc_stats(ctx).collections == 2 && rc_stats(ctx).live_objects == 1);
    for (size_t i = 0; i < budget * 3; ++i) cycle(ctx);
    assert(rc_stats(ctx).collections == 5 && rc_stats(ctx).live_objects == 1);
    rc_release(root); assert(rc_stats(ctx).live_objects == 0);
    rc_context_free(ctx);

    /* Explicit configuration preserves the exact allocation interval and
     * disabling semantics rather than silently enabling adaptation. */
    ctx = rc_context_new(); assert(ctx);
    rc_set_collection_interval(ctx, 7);
    for (size_t i = 0; i < 13; ++i) cycle(ctx);
    assert(rc_stats(ctx).collections == 1 && rc_stats(ctx).live_objects == 6);
    rc_set_collection_interval(ctx, 0);
    for (size_t i = 0; i < 100; ++i) cycle(ctx);
    assert(rc_stats(ctx).collections == 1 && rc_stats(ctx).live_objects == 106);
    rc_collect(ctx); assert(rc_stats(ctx).live_objects == 0);
    rc_context_free(ctx);
    puts("adaptive automatic cycle collection: PASS");
    return 0;
}
