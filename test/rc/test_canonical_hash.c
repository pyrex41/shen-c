#include "primitives.h"
#include "rc.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static RcValue *call(RcContext *ctx, RcValue *fn, RcValue *arg) {
    RcValue *args[] = {arg};
    RcValue *out = rc_apply(ctx, fn, 1, args);
    assert(out && !rc_has_error(ctx));
    return out;
}
static RcValue *old_hashkey(RcContext *ctx, RcValue *self, size_t argc, RcValue **args) {
    (void)self; (void)argc;
    RcValue *arg=args[0]; args[0]=NULL;
    RcValue *result=rc_integer(ctx, rc_type(arg) == RC_NUMBER && rc_number_is_exact_integer(arg) ? 17 : 23);
    rc_release(arg); return result;
}
static RcValue *always_false(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *args) {
    (void)self; (void)argc; (void)args; return rc_false(ctx);
}
static RcValue *dummy_dependency(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *args) {
    (void)self; (void)argc; (void)args; return rc_nil();
}
static void same_hash(RcContext *ctx, RcValue *fn, RcValue *a, RcValue *b) {
    assert(rc_equal(a, b));
    RcValue *ha = call(ctx, fn, a), *hb = call(ctx, fn, b);
    assert(rc_number_is_exact_integer(ha) && rc_number_is_exact_integer(hb));
    assert(rc_integer_value(ha) == rc_integer_value(hb));
    rc_release(ha); rc_release(hb);
}

int main(void) {
    RcContext *ctx = rc_context_new();
    assert(ctx && rc_install_primitives(ctx));
    const char *dependency_names[]={"explode","shen.app","map","string->n","shen.prodbutzero"};
    const size_t dependency_arities[]={1,3,2,1,2};
    for (size_t i=0;i<sizeof(dependency_names)/sizeof(*dependency_names);i++) {
        RcValue *s=rc_symbol(ctx,dependency_names[i]);
        RcValue *f=rc_closure(ctx,dependency_arities[i],dummy_dependency,0,NULL);
        assert(s&&f&&rc_function_set(ctx,s,f)); rc_release(f); rc_release(s);
    }
    RcValue *hash_name = rc_symbol(ctx, "shen.hashkey");
    RcValue *original = rc_closure_owned(ctx, 1, old_hashkey, 0, NULL);
    assert(hash_name && original && rc_function_set(ctx, hash_name, original));
    assert(rc_install_numeric_hash_owned(ctx, old_hashkey));
    RcValue *fn = rc_function_get(ctx, hash_name);
    assert(fn && !rc_has_error(ctx));

    RcValue *one = rc_integer(ctx, 1), *one_float = rc_number(ctx, 1.0);
    RcValue *zero = rc_integer(ctx, 0), *minus_zero = rc_number(ctx, -0.0);
    RcValue *two53 = rc_integer(ctx, INT64_C(9007199254740992));
    RcValue *two53_float = rc_number(ctx, 9007199254740992.0);
    RcValue *min = rc_integer(ctx, INT64_MIN), *min_float = rc_number(ctx, (double)INT64_MIN);
    same_hash(ctx, fn, one, one_float); same_hash(ctx, fn, zero, minus_zero);
    same_hash(ctx, fn, two53, two53_float); same_hash(ctx, fn, min, min_float);

    RcValue *max = rc_integer(ctx, INT64_MAX), *rounded_max = rc_number(ctx, (double)INT64_MAX);
    assert(!rc_equal(max, rounded_max));
    RcValue *unchanged = call(ctx, fn, rounded_max);
    assert(rc_integer_value(unchanged) == 23); rc_release(unchanged);
    rc_release(rounded_max); rc_release(max);

    RcValue *str_name = rc_symbol(ctx, "str"), *str_fn = rc_function_get(ctx, str_name);
    RcValue *printed = call(ctx, str_fn, one_float);
    assert(rc_type(printed) == RC_STRING && !strcmp(rc_text(printed), "1.0"));
    rc_release(printed); rc_release(str_fn); rc_release(str_name);

    RcValue *text = rc_string(ctx, "key"), *fallback = call(ctx, fn, text);
    assert(rc_integer_value(fallback) == 23); rc_release(fallback);
    RcValue *number_name = rc_symbol(ctx, "number?"), *false_fn = rc_closure(ctx, 1, always_false, 0, NULL);
    assert(number_name && false_fn && rc_function_set(ctx, number_name, false_fn));
    fallback = call(ctx, fn, one_float);
    assert(rc_integer_value(fallback) == 23); rc_release(fallback);

    rc_release(false_fn); rc_release(number_name); rc_release(text);
    rc_release(min_float); rc_release(min); rc_release(two53_float); rc_release(two53);
    rc_release(minus_zero); rc_release(zero); rc_release(one_float); rc_release(one);
    rc_release(fn); rc_release(original); rc_release(hash_name); rc_context_free(ctx);
    puts("canonical numeric hash wrapper: equality, printing, bounds, fallback PASS");
    return 0;
}
