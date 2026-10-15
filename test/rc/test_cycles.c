#include "rc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks;
static size_t cases;
#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static RcContext *context(void)
{
    RcContext *ctx = rc_context_new();
    CHECK(ctx != NULL);
    rc_set_collection_interval(ctx, 0);
    CHECK(rc_stats(ctx).live_objects == 0);
    cases++;
    return ctx;
}

static void empty(RcContext *ctx)
{
    rc_collect(ctx);
    RcStats s = rc_stats(ctx);
    CHECK(!rc_has_error(ctx));
    CHECK(s.live_objects == 0);
    CHECK(s.live_bytes == 0);
    CHECK(s.total_allocations == s.total_frees);
    rc_context_free(ctx);
}

static RcValue *capture_zero(RcContext *ctx, RcValue *self,
                             size_t argc, RcValue *const *argv)
{
    (void)ctx; (void)argc; (void)argv;
    return rc_retain(rc_capture(self, 0));
}

static RcValue *count_down(RcContext *ctx, RcValue *self,
                           size_t argc, RcValue *const *argv)
{
    CHECK(argc == 1);
    int64_t n = rc_integer_value(argv[0]);
    if (n == 0) return rc_integer(ctx, 42);
    RcValue *next = rc_integer(ctx, n - 1);
    RcValue *args[] = {next};
    RcValue *tail = rc_tailcall(ctx, self, 1, args);
    rc_release(next);
    return tail;
}

static void test_acyclic_aliases(void)
{
    RcContext *ctx = context();
    RcValue *text = rc_string(ctx, "owned");
    RcValue *alias = rc_retain(text);
    rc_release(text);
    CHECK(strcmp(rc_text(alias), "owned") == 0);
    CHECK(rc_stats(ctx).total_frees == 0);
    rc_release(alias);
    CHECK(rc_stats(ctx).total_frees == 1);
    empty(ctx);
}

static void test_self_cycle(void)
{
    RcContext *ctx = context();
    RcValue *v = rc_vector(ctx, 1);
    CHECK(rc_vector_set(ctx, v, 0, v));
    rc_release(v);
    CHECK(rc_stats(ctx).live_objects == 1);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).cycle_objects_freed == 1);
    empty(ctx);
}

static void test_two_node_cycle(void)
{
    RcContext *ctx = context();
    RcValue *a = rc_vector(ctx, 1), *b = rc_vector(ctx, 1);
    CHECK(rc_vector_set(ctx, a, 0, b));
    CHECK(rc_vector_set(ctx, b, 0, a));
    rc_release(a); rc_release(b);
    CHECK(rc_stats(ctx).live_objects == 2);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).cycle_objects_freed == 2);
    empty(ctx);
}

static void test_rooted_cycle(void)
{
    RcContext *ctx = context();
    RcValue *a = rc_vector(ctx, 2), *b = rc_vector(ctx, 1);
    RcValue *text = rc_string(ctx, "reachable");
    CHECK(rc_vector_set(ctx, a, 0, b));
    CHECK(rc_vector_set(ctx, a, 1, text));
    CHECK(rc_vector_set(ctx, b, 0, a));
    rc_release(text); rc_release(b);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).live_objects == 3);
    CHECK(rc_stats(ctx).total_frees == 0);
    RcValue *held = rc_vector_get(ctx, a, 1);
    CHECK(strcmp(rc_text(held), "reachable") == 0);
    rc_release(held);
    rc_release(a);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).total_frees == 3);
    empty(ctx);
}

static void test_external_alias_root(void)
{
    RcContext *ctx = context();
    RcValue *v = rc_vector(ctx, 1), *alias = rc_retain(v);
    CHECK(rc_vector_set(ctx, v, 0, v));
    rc_release(v);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).live_objects == 1);
    RcValue *same = rc_vector_get(ctx, alias, 0);
    CHECK(same == alias);
    rc_release(same); rc_release(alias);
    empty(ctx);
}

static void test_closure_vector_cycle(void)
{
    RcContext *ctx = context();
    RcValue *v = rc_vector(ctx, 1);
    RcValue *captures[] = {v};
    RcValue *fn = rc_closure(ctx, 0, capture_zero, 1, captures);
    CHECK(rc_vector_set(ctx, v, 0, fn));
    rc_release(v);
    rc_collect(ctx);
    RcValue *result = rc_apply(ctx, fn, 0, NULL);
    CHECK(result == rc_capture(fn, 0));
    rc_release(result); rc_release(fn);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).cycle_objects_freed == 2);
    empty(ctx);
}

static void test_alias_replacement(void)
{
    RcContext *ctx = context();
    RcValue *v = rc_vector(ctx, 1), *s = rc_string(ctx, "saved");
    CHECK(rc_vector_set(ctx, v, 0, s));
    rc_release(s);
    RcValue *alias = rc_vector_get(ctx, v, 0);
    CHECK(rc_vector_set(ctx, v, 0, alias)); /* Retain before replacing. */
    CHECK(rc_vector_set(ctx, v, 0, rc_nil()));
    CHECK(strcmp(rc_text(alias), "saved") == 0);
    CHECK(rc_stats(ctx).total_frees == 0);
    rc_release(alias);
    CHECK(rc_stats(ctx).total_frees == 1);
    rc_release(v);
    empty(ctx);
}

static void test_break_cycle(void)
{
    RcContext *ctx = context();
    RcValue *v = rc_vector(ctx, 1);
    CHECK(rc_vector_set(ctx, v, 0, v));
    CHECK(rc_vector_set(ctx, v, 0, rc_nil()));
    CHECK(rc_stats(ctx).live_objects == 1);
    rc_release(v);
    CHECK(rc_stats(ctx).live_objects == 0);
    CHECK(rc_stats(ctx).cycle_objects_freed == 0);
    empty(ctx);
}

static void test_collect_unrelated(void)
{
    RcContext *ctx = context();
    RcValue *live = rc_vector(ctx, 1), *dead = rc_vector(ctx, 1);
    CHECK(rc_vector_set(ctx, live, 0, live));
    CHECK(rc_vector_set(ctx, dead, 0, dead));
    rc_release(dead);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).live_objects == 1);
    CHECK(rc_stats(ctx).cycle_objects_freed == 1);
    RcValue *same = rc_vector_get(ctx, live, 0);
    CHECK(same == live);
    rc_release(same); rc_release(live);
    empty(ctx);
}

static void test_cycle_to_live_value(void)
{
    RcContext *ctx = context();
    RcValue *v = rc_vector(ctx, 2), *held = rc_string(ctx, "shared");
    CHECK(rc_vector_set(ctx, v, 0, v));
    CHECK(rc_vector_set(ctx, v, 1, held));
    rc_release(v);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).live_objects == 1);
    CHECK(rc_stats(ctx).cycle_objects_freed == 1);
    CHECK(rc_refcount(held) == 1);
    CHECK(strcmp(rc_text(held), "shared") == 0);
    rc_release(held);
    empty(ctx);
}

static void test_pair_vector_cycle(void)
{
    RcContext *ctx = context();
    RcValue *v = rc_vector(ctx, 1);
    RcValue *pair = rc_cons(ctx, v, rc_nil());
    CHECK(rc_vector_set(ctx, v, 0, pair));
    rc_release(pair); rc_release(v);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).cycle_objects_freed == 2);
    empty(ctx);
}

static void test_partial_closure_cycle(void)
{
    RcContext *ctx = context();
    RcValue *v = rc_vector(ctx, 1);
    RcValue *fn = rc_closure(ctx, 2, capture_zero, 0, NULL);
    RcValue *args[] = {v};
    RcValue *part = rc_apply(ctx, fn, 1, args);
    CHECK(part != NULL);
    CHECK(rc_arity(part) == 1);
    CHECK(rc_vector_set(ctx, v, 0, part));
    rc_release(part); rc_release(v); rc_release(fn);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).cycle_objects_freed == 2);
    empty(ctx);
}

static void test_tail_value_cycle(void)
{
    RcContext *ctx = context();
    RcValue *v = rc_vector(ctx, 1);
    RcValue *fn = rc_closure(ctx, 0, capture_zero, 0, NULL);
    RcValue *args[] = {v};
    RcValue *tail = rc_tailcall(ctx, fn, 1, args);
    CHECK(rc_vector_set(ctx, v, 0, tail));
    rc_release(tail); rc_release(v); rc_release(fn);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).cycle_objects_freed == 3);
    empty(ctx);
}

static void test_global_replacement(void)
{
    RcContext *ctx = context();
    RcValue *name = rc_symbol(ctx, "root");
    size_t baseline = rc_stats(ctx).live_objects;
    RcValue *v = rc_vector(ctx, 1);
    CHECK(rc_vector_set(ctx, v, 0, v));
    CHECK(rc_global_set(ctx, name, v));
    rc_release(v);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).live_objects == baseline + 1);
    RcValue *old = rc_global_get(ctx, name);
    CHECK(rc_global_set(ctx, name, old));
    CHECK(rc_global_set(ctx, name, rc_nil()));
    rc_collect(ctx);
    CHECK(rc_stats(ctx).live_objects == baseline + 1);
    rc_release(old);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).live_objects == baseline);
    CHECK(rc_stats(ctx).cycle_objects_freed == 1);
    rc_release(name);
    rc_context_free(ctx); /* Intern table remains a root until shutdown. */
}

static void test_function_redefinition(void)
{
    RcContext *ctx = context();
    RcValue *name = rc_symbol(ctx, "function");
    size_t baseline = rc_stats(ctx).live_objects;
    RcValue *v = rc_vector(ctx, 1);
    RcValue *captures[] = {v};
    RcValue *fn = rc_closure(ctx, 0, capture_zero, 1, captures);
    CHECK(rc_vector_set(ctx, v, 0, fn));
    CHECK(rc_function_set(ctx, name, fn));
    rc_release(fn); rc_release(v);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).live_objects == baseline + 2);
    RcValue *held = rc_function_get(ctx, name);
    CHECK(rc_function_set(ctx, name, held));
    RcValue *replacement = rc_closure(ctx, 0, capture_zero, 0, NULL);
    CHECK(rc_function_set(ctx, name, replacement));
    rc_release(replacement);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).live_objects == baseline + 3);
    RcValue *result = rc_apply(ctx, held, 0, NULL);
    CHECK(rc_type(result) == RC_VECTOR);
    rc_release(result); rc_release(held);
    rc_collect(ctx);
    CHECK(rc_stats(ctx).live_objects == baseline + 1);
    CHECK(rc_stats(ctx).cycle_objects_freed == 2);
    rc_release(name);
    rc_context_free(ctx);
}

static void test_deep_release(void)
{
    RcContext *ctx = context();
    RcValue *list = rc_nil();
    for (size_t i = 0; i < 200000; i++) {
        RcValue *head = rc_integer(ctx, (int64_t)i);
        RcValue *next = rc_cons(ctx, head, list);
        rc_release(head); rc_release(list);
        list = next;
    }
    CHECK(rc_stats(ctx).live_objects == 400000);
    rc_release(list);
    CHECK(rc_stats(ctx).live_objects == 0);
    empty(ctx);
}

static void test_tail_trampoline(void)
{
    RcContext *ctx = context();
    rc_set_collection_interval(ctx, 31);
    RcValue *fn = rc_closure(ctx, 1, count_down, 0, NULL);
    RcValue *n = rc_integer(ctx, 250000), *args[] = {n};
    RcValue *result = rc_apply(ctx, fn, 1, args);
    CHECK(result != NULL);
    CHECK(rc_integer_value(result) == 42);
    rc_release(result); rc_release(n); rc_release(fn);
    empty(ctx);
}

static void test_repeated_collection(void)
{
    RcContext *ctx = context();
    for (size_t i = 0; i < 1000; i++) {
        RcValue *v = rc_vector(ctx, 1);
        CHECK(rc_vector_set(ctx, v, 0, v));
        rc_release(v);
        rc_collect(ctx);
        CHECK(rc_stats(ctx).live_objects == 0);
    }
    CHECK(rc_stats(ctx).cycle_objects_freed == 1000);
    empty(ctx);
}

static void test_error_release(void)
{
    RcContext *ctx = context();
    RcValue *v = rc_vector(ctx, 1);
    CHECK(rc_vector_get(ctx, v, 1) == NULL);
    CHECK(rc_has_error(ctx));
    CHECK(rc_error_message(ctx)[0] != '\0');
    rc_clear_error(ctx);
    CHECK(!rc_has_error(ctx));
    rc_release(v);
    empty(ctx);
}

static void test_context_teardown_with_cycle(void)
{
    RcContext *ctx = context();
    RcValue *v = rc_vector(ctx, 1);
    CHECK(rc_vector_set(ctx, v, 0, v));
    /* Context shutdown explicitly invalidates even outstanding C roots. */
    rc_context_free(ctx);
}

int main(void)
{
    test_acyclic_aliases();
    test_self_cycle();
    test_two_node_cycle();
    test_rooted_cycle();
    test_external_alias_root();
    test_closure_vector_cycle();
    test_alias_replacement();
    test_break_cycle();
    test_collect_unrelated();
    test_cycle_to_live_value();
    test_pair_vector_cycle();
    test_partial_closure_cycle();
    test_tail_value_cycle();
    test_global_replacement();
    test_function_redefinition();
    test_deep_release();
    test_tail_trampoline();
    test_repeated_collection();
    test_error_release();
    test_context_teardown_with_cycle();
    printf("RC runtime: %zu/%zu cases PASS (%zu assertions)\n", cases, cases, checks);
    return 0;
}
