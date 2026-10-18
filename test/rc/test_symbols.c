#include "rc.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

static const char *const names[] = {"cached", "same", "cached", "true"};
static const RcSymbolTable table = {names, 4};
static const char *const other_names[] = {"same", "cached"};
static const RcSymbolTable other = {other_names, 2};
static const RcSymbolTable empty = {NULL, 0};
static const RcSymbolTable invalid = {NULL, 1};
static const RcSymbolTable invalid_name = {(const char *const[]){"valid", NULL}, 2};
static const RcSymbolTable overflow = {names, SIZE_MAX};

static void booleans(RcContext *ctx) {
    /* Exercise dynamic interning before and after the constructor fast path. */
    RcValue *no = rc_symbol(ctx, "false"), *yes = rc_true(ctx);
    RcValue *interned = rc_symbol(ctx, "true"), *again = rc_false(ctx);
    assert(yes && no && interned == yes && again == no);
    assert(rc_is_true(yes) && !rc_is_false(yes) && rc_is_boolean(yes));
    assert(rc_is_false(no) && !rc_is_true(no) && rc_is_boolean(no));
    rc_release(interned); rc_release(again);
    RcValue *text = rc_string(ctx, "true"), *not_bool = rc_symbol(ctx, "True");
    RcValue *values[] = {NULL, rc_nil(), text, not_bool};
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); ++i) {
        assert(!rc_is_true(values[i]) && !rc_is_false(values[i]) && !rc_is_boolean(values[i]));
    }
    rc_release(text); rc_release(not_bool);
    rc_release(yes); rc_release(no); rc_collect(ctx);
    yes = rc_true(ctx); no = rc_false(ctx);
    assert(rc_is_true(yes) && rc_is_false(no));
    assert(rc_require_boolean(ctx, rc_retain(yes)) == yes); rc_release(yes);
    assert(rc_require_boolean(ctx, rc_retain(no)) == no); rc_release(no);
    rc_release(yes); rc_release(no);
}

static RcValue *answer(RcContext *ctx, RcValue *self, size_t n, RcValue *const *args) {
    (void)self; (void)n; (void)args;
    return rc_integer(ctx, 42);
}
int main(void) {
    /* Reuse descriptors across simultaneously live contexts and repeated
     * destruction/recreation (including allocator address reuse). */
    RcContext *anchor = rc_context_new(); assert(anchor);
    RcValue *const *root = rc_symbol_table(anchor, &table); assert(root);
    booleans(anchor);
    assert(!rc_true(NULL) && !rc_false(NULL));
    for (int round = 0; round < 128; ++round) {
        RcContext *ctx = rc_context_new(); assert(ctx);
        booleans(ctx);
        /* Identity inspection uses the value's owner; consuming validation
         * continues to reject values from another context. */
        assert(rc_is_true(root[3]));
        assert(!rc_require_boolean(ctx, rc_retain(root[3])) && rc_has_error(ctx));
        rc_clear_error(ctx);
        RcValue *const *symbols = rc_symbol_table(ctx, &table); assert(symbols);
        assert(symbols != root && symbols[0] != root[0]);
        assert(symbols[0] == symbols[2]);
        RcValue *normal = rc_symbol(ctx, "cached"); assert(normal == symbols[0]);
        rc_release(normal);
        RcValue *const *second = rc_symbol_table(ctx, &other); assert(second);
        assert(second[0] == symbols[1] && second[1] == symbols[0]);
        assert(rc_symbol_table(ctx, &table) == symbols);
        assert(rc_symbol_table(ctx, &empty));
        assert(!rc_symbol_table(ctx, &invalid)); rc_clear_error(ctx);
        assert(!rc_symbol_table(ctx, &invalid_name)); rc_clear_error(ctx);
        assert(!rc_symbol_table(ctx, &overflow)); rc_clear_error(ctx);
        /* Bindings change after caching, and collection keeps intern roots. */
        RcValue *value = rc_integer(ctx, 7); assert(value);
        assert(rc_global_set(ctx, symbols[0], value)); rc_release(value);
        RcValue *fn = rc_closure(ctx, 0, answer, 0, NULL); assert(fn);
        assert(rc_function_set(ctx, symbols[0], fn)); rc_release(fn);
        rc_collect(ctx);
        value = rc_global_get(ctx, symbols[0]); assert(rc_integer_value(value) == 7); rc_release(value);
        fn = rc_function_get(ctx, symbols[0]); assert(fn);
        value = rc_apply(ctx, fn, 0, NULL); assert(rc_integer_value(value) == 42);
        rc_release(fn); rc_release(value);
        assert(rc_symbol_table(ctx, &table) == symbols);
        rc_context_free(ctx);
        assert(rc_is_true(root[3]));
    }
    rc_context_free(anchor);
    puts("RC compiled symbol tables: PASS");
}
