/* Exact immediate boundaries, heap fallback, API guards, and graph ownership. */
#include "primitives.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static RcValue *identity(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *args) {
    (void)ctx; (void)self; assert(argc == 1); return rc_retain(args[0]);
}
static RcValue *call(RcContext *ctx, const char *name, RcValue *a, RcValue *b) {
    RcValue *symbol = rc_symbol(ctx, name), *args[] = {a, b};
    assert(symbol);
    RcValue *out = rc_apply(ctx, symbol, 2, args); rc_release(symbol); return out;
}
static void boundaries(void) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    const int64_t numbers[] = {INT64_MIN, -INT64_C(4611686018427387905),
        -INT64_C(4611686018427387904), -INT64_C(4611686018427387903),
        -INT64_C(9007199254740993), -1, 0, 1, INT64_C(9007199254740993),
        INT64_C(4611686018427387902), INT64_C(4611686018427387903),
        INT64_C(4611686018427387904), INT64_MAX};
    assert(!rc_integer(NULL, 0));
    for (size_t i = 0; i < sizeof(numbers) / sizeof(*numbers); ++i) {
        int64_t n = numbers[i];
        bool immediate = SHEN_RC_IMMEDIATE_INTEGERS &&
            n >= -INT64_C(4611686018427387904) && n <= INT64_C(4611686018427387903);
        RcStats before = rc_stats(ctx);
        RcValue *v = rc_integer(ctx, n), *copy = rc_integer(ctx, n);
        assert(v && rc_type(v) == RC_NUMBER && rc_number_is_integer(v));
        assert(rc_number_is_exact_integer(v) && rc_number_is_int64(v));
        assert(rc_integer_value(v) == n && rc_number_value(v) == (double)n);
        assert(rc_equal(v, copy));
        assert(rc_stats(ctx).total_allocations - before.total_allocations == (immediate ? 0 : 2));
        assert(rc_refcount(v) == (immediate ? SIZE_MAX : 1));
        assert(rc_retain(v) == v); rc_release(v);
        assert(rc_stats(ctx).retain_calls - before.retain_calls ==
               (immediate || !rc_stats_counts_ownership() ? 0 : 1));
        RcValue *floating = rc_number(ctx, (double)n);
        assert(floating && !rc_number_is_exact_integer(floating));
        double d = (double)n;
        bool same = d >= -9223372036854775808.0 && d < 9223372036854775808.0 && (int64_t)d == n;
        assert(rc_equal(v, floating) == same && rc_equal(floating, v) == same);
        char expected[64], actual[64] = {0};
        snprintf(expected, sizeof(expected), "%" PRId64, n);
        FILE *out = tmpfile(); assert(out && rc_print(out, v)); rewind(out);
        assert(fread(actual, 1, sizeof(actual)-1, out) == strlen(expected)); fclose(out);
        assert(!strcmp(expected, actual));
        rc_release(floating); rc_release(copy); rc_release(v);
        assert(rc_stats(ctx).live_objects == 0);
    }
    rc_context_free(ctx);
}
static void arithmetic(void) {
    RcContext *ctx = rc_context_new(); assert(ctx && rc_install_primitives(ctx));
    const int64_t high = INT64_C(4611686018427387903), low = -INT64_C(4611686018427387904);
    RcValue *a = rc_integer(ctx, high), *b = rc_integer(ctx, low), *one = rc_integer(ctx, 1), *result;
    result = call(ctx, "+", a, one); assert(result && rc_integer_value(result) == high + 1); rc_release(result);
    result = call(ctx, "-", b, one); assert(result && rc_integer_value(result) == low - 1); rc_release(result);
    result = call(ctx, "+", a, b); assert(result && rc_integer_value(result) == -1); rc_release(result);
    RcValue *max = rc_integer(ctx, INT64_MAX), *min = rc_integer(ctx, INT64_MIN), *minus = rc_integer(ctx, -1);
    assert(!call(ctx, "+", max, one) && rc_has_error(ctx)); rc_clear_error(ctx);
    assert(!call(ctx, "-", min, one) && rc_has_error(ctx)); rc_clear_error(ctx);
    assert(!call(ctx, "*", min, minus) && rc_has_error(ctx)); rc_clear_error(ctx);
    RcValue *floating = rc_number(ctx, 0.5);
    result = call(ctx, "+", one, floating);
    assert(result && rc_number_value(result) == 1.5 && !rc_number_is_exact_integer(result)); rc_release(result);
    rc_release(floating); rc_release(max); rc_release(min); rc_release(minus);
    rc_release(a); rc_release(b); rc_release(one); rc_context_free(ctx);
}
static void graphs_and_guards(void) {
    RcContext *ctx = rc_context_new(), *other = rc_context_new(); assert(ctx && other);
    RcValue *n = rc_integer(ctx, -7);
    /* All accessors and rejection paths must treat the handle as a number
     * before any heap payload access, including callable/tail dispatch. */
    assert(!rc_text(n) && rc_text_length(n) == 0 && !rc_is_true(n));
    assert(!rc_head_borrowed(n) && !rc_tail_borrowed(n));
    assert(rc_vector_length(n) == 0 && !rc_capture(n, 0) && rc_arity(n) == 0);
    assert(!rc_closure_is_native(n, identity) && !rc_stream_file(n) && !rc_stream_is_input(n));
    assert(!rc_head(ctx, n)); rc_clear_error(ctx);
    assert(!rc_tail(ctx, n)); rc_clear_error(ctx);
    assert(!rc_vector_get(ctx, n, 0)); rc_clear_error(ctx);
    assert(!rc_vector_set(ctx, n, 0, n)); rc_clear_error(ctx);
    assert(!rc_stream_close(ctx, n)); rc_clear_error(ctx);
    assert(!rc_global_get(ctx, n)); rc_clear_error(ctx);
    assert(!rc_global_set(ctx, n, n)); rc_clear_error(ctx);
    assert(!rc_function_lookup(ctx, n) && !rc_has_error(ctx));
    assert(!rc_function_set(ctx, n, n)); rc_clear_error(ctx);
    assert(!rc_apply(ctx, n, 0, NULL)); rc_clear_error(ctx);
    assert(!rc_require_boolean(ctx, rc_retain(n))); rc_clear_error(ctx);
    RcValue *reuse = rc_retain(n), *unused = NULL;
    assert(!rc_pair_take_tail_reuse(ctx, reuse, &unused) && !unused); rc_clear_error(ctx);
    RcValue *pair = rc_cons_reuse(ctx, rc_retain(n), n, rc_nil()); assert(pair); rc_release(pair);
    RcValue *foreign = rc_number(other, 1.0);
    assert(!rc_cons(ctx, foreign, rc_nil())); rc_clear_error(ctx);
    rc_release(foreign);
    if (SHEN_RC_IMMEDIATE_INTEGERS) {
        pair = rc_cons(other, n, rc_nil()); assert(pair); rc_release(pair);
    }
    RcValue *v = rc_vector(ctx, 3), *captures[] = {v, n};
    RcValue *fn = rc_closure(ctx, 1, identity, 2, captures);
    RcValue *args[] = {n}, *tail = rc_tailcall(ctx, fn, 1, args);
    assert(rc_vector_set(ctx, v, 0, tail) && rc_vector_set(ctx, v, 1, n));
    pair = rc_cons(ctx, n, v); assert(pair && rc_vector_set(ctx, v, 2, pair));
    RcValue *out = rc_apply(ctx, fn, 1, args); assert(out == n); rc_release(out);
    rc_release(pair); rc_release(tail); rc_release(fn); rc_release(v);
    rc_collect(ctx);
    assert(rc_stats(ctx).cycle_objects_freed == 4);
    assert(rc_stats(ctx).live_objects == (SHEN_RC_IMMEDIATE_INTEGERS ? 0 : 1));
    rc_context_free(other);
    rc_context_free(ctx);
    if (SHEN_RC_IMMEDIATE_INTEGERS) { assert(rc_integer_value(n) == -7); rc_release(n); }
    /* The acyclic graph walk must also ignore immediate leaves. */
    ctx = rc_context_new_with_policy(RC_ACYCLIC_ONLY); assert(ctx);
    n = rc_integer(ctx, 13); v = rc_vector(ctx, 1);
    pair = rc_cons(ctx, n, rc_nil()); assert(rc_vector_set(ctx, v, 0, pair));
    rc_release(pair); rc_release(v); rc_release(n);
    assert(rc_stats(ctx).live_objects == 0); rc_context_free(ctx);
}
int main(void) {
    boundaries(); arithmetic(); graphs_and_guards();
    puts("exact immediate integers: boundaries, fallback, arithmetic, guards and cycles PASS");
    return 0;
}
