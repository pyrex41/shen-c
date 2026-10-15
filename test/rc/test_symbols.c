#include "rc.h"
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

static RcValue *answer(RcContext *ctx, RcValue *self, size_t n, RcValue *const *args) {
    (void)self; (void)n; (void)args;
    return rc_integer(ctx, 42);
}
int main(void) {
    /* Reuse descriptors across simultaneously live contexts and repeated
     * destruction/recreation (including allocator address reuse). */
    RcContext *anchor = rc_context_new(); assert(anchor);
    RcValue *const *root = rc_symbol_table(anchor, &table); assert(root);
    for (int round = 0; round < 128; ++round) {
        RcContext *ctx = rc_context_new(); assert(ctx);
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
