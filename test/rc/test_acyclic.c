#include "rc.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t cases;
static RcValue *identity(RcContext *ctx, RcValue *self, size_t argc, RcValue **argv) {
    (void)ctx; (void)self; (void)argc;
    RcValue *result = argv[0]; argv[0] = NULL; return result;
}
static RcContext *context(void) {
    RcContext *ctx = rc_context_new_with_policy(RC_ACYCLIC_ONLY);
    assert(ctx && rc_context_is_acyclic(ctx)); return ctx;
}
static void rejected(RcContext *ctx, RcValue *vector, RcValue *value) {
    RcValue *old = rc_vector_get(ctx, vector, 0); assert(old);
    size_t refs = rc_refcount(value);
    assert(!rc_vector_set(ctx, vector, 0, value));
    assert(rc_has_error(ctx) && strstr(rc_error_message(ctx), "acyclic mode"));
    assert(rc_refcount(value) == refs);
    rc_clear_error(ctx);
    RcValue *after = rc_vector_get(ctx, vector, 0);
    assert(after == old); rc_release(after); rc_release(old);
}
static void self_and_mutual(void) {
    RcContext *ctx = context();
    RcValue *a = rc_vector(ctx, 1), *b = rc_vector(ctx, 1);
    rejected(ctx, a, a);
    assert(rc_vector_set(ctx, a, 0, b));
    rejected(ctx, b, a);
    assert(rc_vector_set(ctx, a, 0, rc_nil()));
    rc_release(a); rc_release(b);
    assert(rc_stats(ctx).live_objects == 0 && rc_stats(ctx).collections == 0);
    rc_context_free(ctx); cases++;
}
static void indirect_edges(void) {
    RcContext *ctx = context();
    RcValue *v = rc_vector(ctx, 1);
    RcValue *closure = rc_closure_owned(ctx, 2, identity, 1, &v);
    rejected(ctx, v, closure); /* capture edge */
    RcValue *pair = rc_cons(ctx, v, rc_nil());
    rejected(ctx, v, pair);
    RcValue *args[] = {v};
    RcValue *partial = rc_apply(ctx, closure, 1, args); assert(partial);
    rejected(ctx, v, partial); /* bound argument edge, not just captures */
    RcValue *request = rc_tailcall(ctx, closure, 1, args); assert(request);
    rejected(ctx, v, request); /* tail argument and function/capture edges */
    rc_release(request); rc_release(partial); rc_release(pair); rc_release(closure); rc_release(v);
    assert(rc_stats(ctx).live_objects == 0 && rc_stats(ctx).collections == 0);
    rc_context_free(ctx); cases++;
}
static void shared_dag(void) {
    RcContext *ctx = context();
    RcValue *number = rc_integer(ctx, 42), *pair = rc_cons(ctx, number, rc_nil());
    RcValue *vector = rc_vector(ctx, 2);
    assert(rc_vector_set(ctx, vector, 0, pair) && rc_vector_set(ctx, vector, 1, pair));
    assert(rc_refcount(pair) == 3);
    assert(rc_vector_set(ctx, vector, 0, rc_nil()) && rc_refcount(pair) == 2);
    assert(rc_vector_set(ctx, vector, 1, rc_nil()) && rc_refcount(pair) == 1);
    rc_collect(ctx); rc_safepoint(ctx); assert(rc_stats(ctx).collections == 0);
    rc_release(vector); rc_release(pair); rc_release(number);
    assert(rc_stats(ctx).live_objects == 0); rc_context_free(ctx); cases++;
}
static void roots_and_reuse(void) {
    RcContext *ctx = context();
    RcValue *symbol = rc_symbol(ctx, "root"), *v = rc_vector(ctx, 1);
    /* Slots are context roots rather than heap edges; symbolic lookup cycles
     * do not become hidden reference-count cycles or disable root retention. */
    assert(rc_global_set(ctx, symbol, v) && rc_vector_set(ctx, v, 0, symbol));
    rc_release(v); v = rc_global_get(ctx, symbol); assert(v);
    RcValue *head = rc_integer(ctx, 7), *cell = rc_cons(ctx, head, rc_nil());
    RcValue *candidate = NULL, *tail = rc_pair_take_tail_reuse(ctx, cell, &candidate);
    assert(tail && candidate == cell && rc_is_nil(rc_head_borrowed(candidate)));
    rc_collect(ctx); /* intentionally a no-op in restricted mode */
    cell = rc_cons_reuse(ctx, candidate, head, tail); rc_release(tail);
    assert(cell && rc_head_borrowed(cell) == head);
    assert(rc_global_set(ctx, symbol, rc_nil()));
    rc_release(cell); rc_release(head); rc_release(v); rc_release(symbol);
    assert(rc_stats(ctx).live_objects == 1 && rc_stats(ctx).collections == 0); /* intern root */
    rc_context_free(ctx); cases++;
}
static void deep_release(void) {
    RcContext *ctx = context(); RcValue *list = rc_nil();
    for (size_t i = 0; i < 100000; ++i) {
        RcValue *next = rc_cons(ctx, rc_nil(), list); assert(next);
        rc_release(list); list = next;
    }
    rc_release(list);
    assert(rc_stats(ctx).live_objects == 0 && rc_stats(ctx).collections == 0);
    rc_context_free(ctx); cases++;
}
static void bounded_writes(void) {
    RcContext *ctx = context(); rc_set_collection_interval(ctx, 1);
    RcValue *v = rc_vector(ctx, 1);
    for (size_t i = 0; i < 10000; ++i) {
        RcValue *n = rc_integer(ctx, (int64_t)i); assert(n);
        assert(rc_vector_set(ctx, v, 0, n)); rc_release(n); rc_safepoint(ctx);
        assert(rc_stats(ctx).live_objects == 2 && rc_stats(ctx).collections == 0);
    }
    assert(rc_stats(ctx).peak_objects == 3);
    rc_release(v); assert(rc_stats(ctx).live_objects == 0);
    rc_context_free(ctx); cases++;
}
static void default_policy(void) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    RcValue *v = rc_vector(ctx, 1);
#if defined(SHEN_RC_ACYCLIC_ONLY) && SHEN_RC_ACYCLIC_ONLY
    assert(rc_context_is_acyclic(ctx)); rejected(ctx, v, v);
    rc_release(v); rc_collect(ctx); assert(rc_stats(ctx).collections == 0);
#else
    assert(!rc_context_is_acyclic(ctx)); assert(rc_vector_set(ctx, v, 0, v));
    rc_release(v); rc_collect(ctx); assert(rc_stats(ctx).live_objects == 0);
#endif
    rc_context_free(ctx); cases++;
}
int main(void) {
    self_and_mutual(); indirect_edges(); shared_dag(); roots_and_reuse();
    deep_release(); bounded_writes(); default_policy();
    printf("RC optional acyclic policy: %zu/7 cases PASS\n", cases);
    return 0;
}
