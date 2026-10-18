/* Standalone boolean-continuation tests; link only rc.c. */
#include "rc.h"
#ifdef NDEBUG
#undef NDEBUG /* Test checks and their setup must run in Release builds. */
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned called;
static RcValue *countdown(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *argv) {
    (void)argc;
    int64_t n = rc_integer_value(argv[0]);
    if (!n) return rc_retain(rc_capture(self, 0));
    RcValue *next = rc_integer(ctx, n - 1);
    RcValue *request = rc_tailcall(ctx, self, 1, &next);
    rc_release(next);
    return rc_require_boolean(ctx, request);
}
static RcValue *identity(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *argv) {
    (void)ctx; (void)self; (void)argc; called++;
    return rc_retain(argv[0]);
}
static RcValue *captured(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *argv) {
    (void)ctx; (void)argc; (void)argv;
    return rc_retain(rc_capture(self, 0));
}
static RcValue *guarded(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *argv) {
    (void)argc; (void)argv;
    return rc_require_boolean(ctx, rc_tailcall(ctx, rc_capture(self, 0), 0, NULL));
}
static RcValue *guarded_overapply(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *argv) {
    (void)argc; (void)argv;
    RcValue *arg = rc_true(ctx);
    RcValue *request = rc_tailcall(ctx, rc_capture(self, 0), 1, &arg);
    rc_release(arg);
    return rc_require_boolean(ctx, request);
}
int main(void) {
    RcContext *ctx = rc_context_new(); assert(ctx);
    RcValue *yes = rc_true(ctx), *bad = rc_integer(ctx, 7);
    RcValue *fn = rc_closure(ctx, 1, countdown, 1, &yes);
    RcValue *n = rc_integer(ctx, 100000), *result = rc_apply(ctx, fn, 1, &n);
    assert(result == yes && !rc_has_error(ctx));
    assert(rc_stats(ctx).peak_objects < 32);
    rc_release(result); rc_release(fn);
    fn = rc_closure(ctx, 1, countdown, 1, &bad);
    result = rc_apply(ctx, fn, 1, &n);
    assert(!result && rc_has_error(ctx)); rc_clear_error(ctx); rc_release(fn);

    /* A guard on an outer result runs before the outer excess arguments. */
    RcValue *id = rc_closure(ctx, 1, identity, 0, NULL);
    RcValue *returns_id = rc_closure(ctx, 0, captured, 1, &id);
    RcValue *outer = rc_closure(ctx, 0, guarded, 1, &returns_id);
    result = rc_apply(ctx, outer, 1, &yes);
    assert(!result && rc_has_error(ctx) && called == 0);
    rc_clear_error(ctx); rc_release(outer);

    /* A guard on a request runs after that request's own overapplication. */
    outer = rc_closure(ctx, 0, guarded_overapply, 1, &returns_id);
    result = rc_apply(ctx, outer, 0, NULL);
    assert(result == yes && !rc_has_error(ctx) && called == 1);
    rc_release(result); rc_release(outer);

    /* An owned alias is not necessarily unique; adding a guard copies it. */
    RcValue *returns_bad = rc_closure(ctx, 0, captured, 1, &bad);
    RcValue *shared = rc_tailcall(ctx, returns_bad, 0, NULL);
    RcValue *guard = rc_require_boolean(ctx, rc_retain(shared));
    assert(guard && guard != shared);
    rc_release(guard); rc_release(shared); rc_release(returns_bad);
    rc_release(returns_id); rc_release(id); rc_release(n); rc_release(bad); rc_release(yes);
    rc_collect(ctx); assert(!rc_has_error(ctx));
    rc_context_free(ctx);
    puts("boolean tail continuations: PASS");
    return 0;
}
