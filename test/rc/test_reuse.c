#include "rc.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t cases;
static RcValue *take(RcValue **argv, size_t i) {
    RcValue *value = argv[i]; argv[i] = NULL; return value;
}
static RcValue *owned_identity(RcContext *ctx, RcValue *self, size_t argc, RcValue **argv) {
    (void)self; (void)argc;
    rc_collect(ctx); /* Moved argument slots must count as external roots. */
    return take(argv, 0);
}
static RcValue *borrowed_identity(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *argv) {
    (void)ctx; (void)self; (void)argc; return rc_retain(argv[0]);
}
static RcValue *return_capture(RcContext *ctx, RcValue *self, size_t argc, RcValue **argv) {
    (void)ctx; (void)argc; (void)argv; return rc_retain(rc_capture(self, 0));
}
static RcValue *reuse_countdown(RcContext *ctx, RcValue *self, size_t argc, RcValue **argv) {
    (void)argc;
    RcValue *cell = take(argv, 0);
    int64_t n = rc_integer_value(argv[1]);
    if (!n) return cell;
    rc_collect(ctx); /* Candidate is a C root, with no request edge to it. */
    cell = rc_cons_reuse(ctx, cell, rc_capture(self, 0), rc_nil());
    if (!cell) return NULL;
    RcValue *args[] = {cell, rc_integer(ctx, n - 1)};
    return rc_tailcall_owned_args(ctx, self, 2, args);
}
static RcValue *reuse_first(RcContext *ctx, RcValue *self, size_t argc, RcValue **argv) {
    (void)self; (void)argc;
    RcValue *cell = take(argv, 0);
    return rc_cons_reuse(ctx, cell, argv[1], rc_nil());
}
static RcValue *owned_error(RcContext *ctx, RcValue *self, size_t argc, RcValue **argv) {
    (void)self; (void)argc;
    RcValue *taken = take(argv, 0); rc_release(taken);
    return rc_error(ctx, "intentional owned callback error");
}
static RcValue *guarded_tail(RcContext *ctx, RcValue *self, size_t argc, RcValue **argv) {
    (void)argc;
    RcValue *arg[] = {take(argv, 0)};
    return rc_require_boolean(ctx, rc_tailcall_owned_args(ctx, rc_capture(self, 0), 1, arg));
}
static RcValue *reverse_reusing(RcContext *ctx, RcValue *self, size_t argc, RcValue **argv) {
    (void)argc;
    RcValue *list = take(argv, 0), *acc = take(argv, 1);
    if (rc_is_nil(list)) { rc_release(list); return acc; }
    RcValue *head = rc_head(ctx, list), *candidate = NULL;
    if (!head) { rc_release(list); rc_release(acc); return NULL; }
    RcValue *tail = rc_pair_take_tail_reuse(ctx, list, &candidate);
    if (!tail) { rc_release(head); rc_release(acc); return NULL; }
    RcValue *next = rc_cons_reuse(ctx, candidate, head, acc);
    rc_release(head); rc_release(acc);
    if (!next) { rc_release(tail); return NULL; }
    RcValue *args[] = {tail, next};
    return rc_tailcall_owned_args(ctx, self, 2, args);
}
static void unique_reuse(void) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    RcValue *a = rc_integer(ctx, 1), *b = rc_integer(ctx, 2);
    RcValue *tail = rc_cons(ctx, b, rc_nil()), *cell = rc_cons(ctx, a, tail);
    rc_release(a); rc_release(b); rc_release(tail);
    RcValue *old = cell, *head = rc_head_borrowed(cell), *rest = rc_tail_borrowed(cell);
    RcStats before = rc_stats(ctx);
    cell = rc_cons_reuse(ctx, cell, rest, head);
    assert(cell == old && rc_head_borrowed(cell) == rest && rc_tail_borrowed(cell) == head);
    RcStats after = rc_stats(ctx);
    assert(after.total_allocations == before.total_allocations && after.pair_reuses == 1);
    rc_collect(ctx); rc_release(cell); assert(rc_stats(ctx).live_objects == 0);
    rc_context_free(ctx); cases++;
}
static void aliases_and_failures(void) {
    RcContext *ctx = rc_context_new(), *other = rc_context_new(); assert(ctx && other);
    RcValue *one = rc_integer(ctx, 1), *two = rc_integer(ctx, 2);
    RcValue *cell = rc_cons(ctx, one, rc_nil()), *alias = rc_retain(cell);
    RcValue *out = rc_cons_reuse(ctx, cell, two, rc_nil());
    assert(out && out != alias && rc_head_borrowed(alias) == one && rc_head_borrowed(out) == two);
    assert(rc_refcount(alias) == 1); rc_release(out); rc_release(alias);
    cell = rc_cons(ctx, one, rc_nil());
    out = rc_cons_reuse(ctx, cell, cell, cell);
    assert(out && out != cell && rc_head_borrowed(out) == cell && rc_tail_borrowed(out) == cell);
    assert(rc_head_borrowed(cell) == one && rc_is_nil(rc_tail_borrowed(cell)));
    rc_release(out);
    /* Replacing children must retain a candidate-owned borrowed child first. */
    cell = rc_cons(ctx, two, rc_nil());
    RcValue *replacement = rc_head_borrowed(cell);
    rc_release(two); two = NULL;
    out = rc_cons_reuse(ctx, cell, replacement, replacement);
    assert(out == cell && rc_integer_value(rc_head_borrowed(out)) == 2);
    rc_release(out);
    RcValue *foreign = rc_integer(other, 9);
    cell = rc_cons(ctx, one, rc_nil());
    size_t live = rc_stats(ctx).live_objects;
    out = rc_cons_reuse(ctx, cell, foreign, rc_nil());
    assert(!out && rc_has_error(ctx) && rc_stats(ctx).live_objects == live - 1);
    rc_clear_error(ctx);
    /* A non-pair candidate can also be the borrowed replacement child. */
    out = rc_cons_reuse(ctx, rc_retain(one), one, rc_nil());
    assert(out && rc_head_borrowed(out) == one); rc_release(out);
    RcValue *token = rc_nil();
    out = rc_pair_take_tail_reuse(ctx, rc_retain(one), &token);
    assert(!out && !token && rc_has_error(ctx)); rc_clear_error(ctx);
    cell = rc_cons(ctx, one, one);
    out = rc_pair_take_tail_reuse(ctx, cell, &token);
    assert(out == one && token == cell && rc_is_nil(rc_head_borrowed(token)) && rc_is_nil(rc_tail_borrowed(token)));
    rc_collect(ctx); rc_release(out); rc_release(token);
    cell = rc_cons(ctx, one, rc_nil());
    assert(!rc_pair_take_tail_reuse(ctx, cell, NULL) && rc_has_error(ctx)); rc_clear_error(ctx);
    RcStats stats = rc_stats(ctx);
    assert(stats.pair_reuse_attempts == 5 && stats.pair_reuses == 1 && stats.pair_reuse_misses == 4);
    rc_release(foreign); rc_release(one);
    assert(rc_stats(ctx).live_objects == 0 && rc_stats(other).live_objects == 0);
    rc_context_free(ctx); rc_context_free(other); cases++;
}
static void tail_reuse(void) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    rc_set_collection_interval(ctx, 1);
    RcValue *item = rc_integer(ctx, 42);
    RcValue *fn = rc_closure_owned(ctx, 2, reuse_countdown, 1, &item);
    RcValue *cell = rc_cons(ctx, item, rc_nil());
    RcValue *args[] = {cell, rc_integer(ctx, 100000)};
    RcValue *result = rc_apply_owned_args(ctx, fn, 2, args);
    assert(!args[0] && !args[1] && result == cell && !rc_has_error(ctx));
    RcStats stats = rc_stats(ctx);
    assert(stats.pair_allocations == 1 && stats.pair_reuses == 100000 && stats.pair_reuse_misses == 0);
    assert(stats.peak_objects < 32 && stats.live_capture_edges == 1);
    rc_release(result); rc_release(fn); rc_release(item);
    assert(rc_stats(ctx).live_objects == 0 && rc_stats(ctx).live_capture_edges == 0);
    rc_context_free(ctx); cases++;
}
static void borrowed_alias_control(void) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    RcValue *old = rc_integer(ctx, 1), *new = rc_integer(ctx, 2);
    RcValue *fn = rc_closure_owned(ctx, 2, reuse_countdown, 1, &new);
    RcValue *cell = rc_cons(ctx, old, rc_nil()), *n = rc_integer(ctx, 10);
    RcValue *args[] = {cell, n};
    RcValue *result = rc_apply(ctx, fn, 2, args);
    assert(result && result != cell && rc_head_borrowed(cell) == old && rc_head_borrowed(result) == new);
    RcStats stats = rc_stats(ctx);
    assert(stats.pair_allocations == 2 && stats.pair_reuses == 9 && stats.pair_reuse_misses == 1);
    rc_release(result); rc_release(cell); rc_release(n); rc_release(fn); rc_release(old); rc_release(new);
    assert(rc_stats(ctx).live_objects == 0); rc_context_free(ctx); cases++;
}
static void partial_and_overapply(void) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    RcValue *a = rc_integer(ctx, 1), *b = rc_integer(ctx, 2), *d = rc_integer(ctx, 3);
    RcValue *cell = rc_cons(ctx, a, rc_nil());
    RcValue *fn = rc_closure_owned(ctx, 2, reuse_first, 0, NULL);
    RcValue *args[] = {cell};
    RcValue *part = rc_apply_owned_args(ctx, fn, 1, args);
    assert(part && !args[0] && rc_arity(part) == 1);
    RcValue *argb[] = {rc_retain(b)}, *argd[] = {rc_retain(d)};
    RcValue *first = rc_apply_owned_args(ctx, part, 1, argb);
    RcValue *second = rc_apply_owned_args(ctx, part, 1, argd);
    assert(first && second && first != cell && second != cell);
    assert(rc_head_borrowed(first) == b && rc_head_borrowed(second) == d && rc_head_borrowed(cell) == a);
    assert(rc_stats(ctx).pair_reuses == 0 && rc_stats(ctx).pair_reuse_misses == 2);
    rc_release(first); rc_release(second); rc_release(part); rc_release(fn);
    RcValue *id = rc_closure_owned(ctx, 1, owned_identity, 0, NULL);
    RcValue *outer = rc_closure_owned(ctx, 1, return_capture, 1, &id);
    RcValue *excess[] = {rc_cons(ctx, a, rc_nil()), rc_cons(ctx, b, rc_nil())};
    cell = excess[1];
    RcValue *result = rc_apply_owned_args(ctx, outer, 2, excess);
    assert(result == cell && !excess[0] && !excess[1]);
    rc_release(result); rc_release(outer); rc_release(id);
    rc_release(a); rc_release(b); rc_release(d);
    assert(rc_stats(ctx).live_objects == 0); rc_context_free(ctx); cases++;
}
static void errors_and_borrowed_bridge(void) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    RcValue *fn = rc_closure_owned(ctx, 2, owned_error, 0, NULL);
    RcValue *cycle = rc_vector(ctx, 1); assert(rc_vector_set(ctx, cycle, 0, cycle));
    RcValue *args[] = {rc_integer(ctx, 1), cycle};
    assert(!rc_apply_owned_args(ctx, fn, 2, args) && !args[0] && !args[1] && rc_has_error(ctx));
    rc_clear_error(ctx); rc_collect(ctx); assert(rc_stats(ctx).live_objects == 1);
    rc_release(fn);
    RcValue *notfn = rc_integer(ctx, 0), *invalid[] = {rc_integer(ctx, 9)};
    assert(!rc_apply_owned_args(ctx, notfn, 1, invalid) && !invalid[0] && rc_has_error(ctx));
    rc_clear_error(ctx); rc_release(notfn);
    fn = rc_closure(ctx, 1, borrowed_identity, 0, NULL);
    RcValue *arg[] = {rc_integer(ctx, 42)}, *value = arg[0];
    RcValue *result = rc_apply_owned_args(ctx, fn, 1, arg);
    assert(result == value && !arg[0] && rc_refcount(result) == 1);
    rc_release(result); rc_release(fn);
    assert(rc_stats(ctx).live_objects == 0); rc_context_free(ctx); cases++;
}
static void shared_tail_and_guards(void) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    RcValue *id = rc_closure_owned(ctx, 1, owned_identity, 0, NULL);
    RcValue *value = rc_integer(ctx, 77), *args[] = {value};
    RcValue *tail = rc_tailcall(ctx, id, 1, args);
    RcValue *getter = rc_closure_owned(ctx, 0, return_capture, 1, &tail);
    for (size_t i = 0; i < 3; ++i) {
        RcValue *result = rc_apply(ctx, getter, 0, NULL);
        assert(result == value && rc_integer_value(result) == 77); rc_release(result);
    }
    rc_release(getter); rc_release(tail);
    RcValue *guard = rc_closure_owned(ctx, 1, guarded_tail, 1, &id);
    RcValue *yes = rc_true(ctx), *valid[] = {rc_retain(yes)};
    RcValue *result = rc_apply_owned_args(ctx, guard, 1, valid);
    assert(result == yes && !valid[0]); rc_release(result);
    RcValue *bad[] = {rc_retain(value)};
    assert(!rc_apply_owned_args(ctx, guard, 1, bad) && !bad[0] && rc_has_error(ctx));
    rc_clear_error(ctx);
    rc_release(yes); rc_release(guard); rc_release(value); rc_release(id);
    rc_collect(ctx); assert(rc_stats(ctx).live_objects == 1); /* interned true */
    rc_context_free(ctx); cases++;
}
static void length_proportional_reuse(bool shared) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    const size_t length = shared ? 100 : 10000;
    RcValue *list = rc_nil();
    for (size_t i = 0; i < length; ++i) {
        RcValue *head = rc_integer(ctx, (int64_t)i), *next = rc_cons(ctx, head, list);
        assert(next); rc_release(head); rc_release(list); list = next;
    }
    RcValue *alias = shared ? rc_retain(list) : NULL;
    RcValue *fn = rc_closure_owned(ctx, 2, reverse_reusing, 0, NULL);
    RcValue *args[] = {list, rc_nil()};
    RcStats before = rc_stats(ctx);
    RcValue *result = rc_apply_owned_args(ctx, fn, 2, args);
    assert(result && !args[0] && !args[1]);
    RcStats after = rc_stats(ctx);
    if (shared) {
        assert(after.pair_allocations - before.pair_allocations == length);
        assert(after.pair_reuses == 0 && after.pair_reuse_misses == length);
        assert(after.pair_tail_transfers == 0 && after.pair_tail_shares == length);
        assert(rc_integer_value(rc_head_borrowed(alias)) == (int64_t)length - 1);
    } else {
        assert(after.pair_allocations == before.pair_allocations);
        assert(after.pair_reuses == length && after.pair_reuse_misses == 0);
        assert(after.pair_tail_transfers == length && after.pair_tail_shares == 0);
        assert(after.total_allocations - before.total_allocations == length + 1);
    }
    RcValue *cursor = result;
    for (size_t i = 0; i < length; ++i) {
        assert(rc_type(cursor) == RC_PAIR && rc_integer_value(rc_head_borrowed(cursor)) == (int64_t)i);
        cursor = rc_tail_borrowed(cursor);
    }
    assert(rc_is_nil(cursor));
    rc_release(alias); rc_release(result); rc_release(fn);
    assert(rc_stats(ctx).live_objects == 0); rc_context_free(ctx); cases++;
}
int main(void) {
    unique_reuse(); aliases_and_failures(); tail_reuse(); borrowed_alias_control();
    partial_and_overapply(); errors_and_borrowed_bridge(); shared_tail_and_guards();
    length_proportional_reuse(false); length_proportional_reuse(true);
    printf("RC ownership and pair reuse: %zu/9 cases PASS\n", cases);
    return 0;
}
