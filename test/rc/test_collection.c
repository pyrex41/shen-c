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
int main(void) {
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
