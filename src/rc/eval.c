#include "eval.h"
#include <stdlib.h>
#include <string.h>

/* An environment is a list of (symbol . value) pairs. Keeping it in ordinary
 * RC values gives closures the same ownership and cycle handling as user data. */
static RcValue *evaluate(RcContext *, RcValue *, RcValue *, bool);
static bool named(RcValue *v, const char *name) {
    return rc_type(v) == RC_SYMBOL && strcmp(rc_text(v), name) == 0;
}
static bool boolean(RcValue *v) { return rc_is_boolean(v); }
static RcValue *lookup(RcValue *env, RcValue *name) {
    while (rc_type(env) == RC_PAIR) {
        RcValue *binding = rc_head_borrowed(env);
        if (rc_equal(rc_head_borrowed(binding), name))
            return rc_tail_borrowed(binding);
        env = rc_tail_borrowed(env);
    }
    return NULL;
}
static RcValue *extend(RcContext *ctx, RcValue *env, RcValue *name, RcValue *value) {
    RcValue *binding = rc_cons(ctx,name,value);
    if (!binding) return NULL;
    RcValue *result = rc_cons(ctx,binding,env);
    rc_release(binding);
    return result;
}
static bool list_length(RcContext *ctx, RcValue *list, size_t *n) {
    *n = 0;
    while (rc_type(list) == RC_PAIR) { ++*n; list = rc_tail_borrowed(list); }
    if (!rc_is_nil(list)) { rc_error(ctx,"eval-kl: improper expression list"); return false; }
    return true;
}
static RcValue *nth(RcValue *list, size_t n) {
    while (n--) list = rc_tail_borrowed(list);
    return rc_head_borrowed(list);
}
static bool count_is(RcContext *ctx, size_t count, size_t expected, const char *form) {
    if (count == expected) return true;
    rc_error(ctx,"eval-kl: %s expects %zu arguments, received %zu",form,expected,count);
    return false;
}
static bool parameters(RcContext *ctx, RcValue *params, size_t *arity) {
    if (!list_length(ctx,params,arity)) return false;
    for (RcValue *p=params; !rc_is_nil(p); p=rc_tail_borrowed(p)) {
        RcValue *name=rc_head_borrowed(p);
        if (rc_type(name)!=RC_SYMBOL) { rc_error(ctx,"eval-kl: parameter must be a symbol"); return false; }
        for (RcValue *q=rc_tail_borrowed(p); !rc_is_nil(q); q=rc_tail_borrowed(q))
            if (rc_equal(name,rc_head_borrowed(q))) { rc_error(ctx,"eval-kl: duplicate parameter %s",rc_text(name)); return false; }
    }
    return true;
}
static RcValue *dynamic_call(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *argv) {
    RcValue *params=rc_capture(self,0), *env=rc_retain(rc_capture(self,2));
    for (size_t i=0; i<argc; ++i) {
        RcValue *next=extend(ctx,env,rc_head_borrowed(params),argv[i]);
        rc_release(env);
        if (!next) return NULL;
        env=next; params=rc_tail_borrowed(params);
    }
    RcValue *result=evaluate(ctx,rc_capture(self,1),env,true);
    rc_release(env);
    return result;
}
static RcValue *make_function(RcContext *ctx, RcValue *params, RcValue *body, RcValue *env) {
    size_t arity;
    if (!parameters(ctx,params,&arity)) return NULL;
    RcValue *captures[]={params,body,env};
    return rc_closure(ctx,arity,dynamic_call,3,captures);
}
static RcValue *application(RcContext *ctx, RcValue *expr, RcValue *env, size_t argc, bool tail) {
    RcValue *head=rc_head_borrowed(expr), *function=NULL;
    if (rc_type(head)==RC_SYMBOL && !lookup(env,head)) function=rc_function_lookup(ctx,head);
    if (!function) function=evaluate(ctx,head,env,false);
    if (!function) return NULL;
    /* Function position may evaluate to a symbol via a lexical alias or an
     * arbitrary expression. Snapshot its definition before argument effects. */
    if (rc_type(function)==RC_SYMBOL) {
        RcValue *resolved=rc_function_get(ctx,function);
        rc_release(function);
        function=resolved;
        if (!function) return NULL;
    }
    RcValue **argv=argc ? calloc(argc,sizeof(*argv)) : NULL;
    if (argc && !argv) { rc_release(function); return rc_error(ctx,"eval-kl: out of memory"); }
    RcValue *rest=rc_tail_borrowed(expr), *result=NULL;
    size_t i=0;
    for (; i<argc; ++i,rest=rc_tail_borrowed(rest)) {
        argv[i]=evaluate(ctx,rc_head_borrowed(rest),env,false);
        if (!argv[i]) break;
    }
    if (i==argc) result=tail ? rc_tailcall(ctx,function,argc,argv) : rc_apply(ctx,function,argc,argv);
    for (size_t j=0; j<i; ++j) rc_release(argv[j]);
    free(argv); rc_release(function);
    return result;
}
static RcValue *evaluate(RcContext *ctx, RcValue *input, RcValue *environment, bool tail) {
    RcValue *expr=rc_retain(input), *env=rc_retain(environment), *result=NULL;
    if (!input) { rc_release(env); return rc_error(ctx,"eval-kl: null expression"); }
    for (;;) {
        if (rc_type(expr)!=RC_PAIR) {
            RcValue *value=rc_type(expr)==RC_SYMBOL ? lookup(env,expr) : NULL;
            result=rc_retain(value ? value : expr); break;
        }
        RcValue *head=rc_head_borrowed(expr), *args=rc_tail_borrowed(expr);
        size_t n;
        if (!list_length(ctx,args,&n)) break;
        if (rc_type(head)==RC_SYMBOL && lookup(env,head)) {
            result=application(ctx,expr,env,n,tail); break;
        }
        if (named(head,"if")) {
            if (!count_is(ctx,n,3,"if")) break;
            RcValue *test=evaluate(ctx,nth(args,0),env,false);
            if (!test) break;
            bool yes=rc_is_true(test), valid=boolean(test); rc_release(test);
            if (!valid) { rc_error(ctx,"eval-kl: if test must be boolean"); break; }
            RcValue *next=rc_retain(nth(args,yes ? 1 : 2)); rc_release(expr); expr=next; continue;
        }
        if (named(head,"let")) {
            if (!count_is(ctx,n,3,"let")) break;
            RcValue *name=nth(args,0);
            if (rc_type(name)!=RC_SYMBOL) { rc_error(ctx,"eval-kl: let binder must be a symbol"); break; }
            RcValue *value=evaluate(ctx,nth(args,1),env,false);
            if (!value) break;
            RcValue *next_env=extend(ctx,env,name,value); rc_release(value);
            if (!next_env) break;
            RcValue *next=rc_retain(nth(args,2));
            rc_release(expr); expr=next; rc_release(env); env=next_env; continue;
        }
        if (named(head,"cond")) {
            RcValue *next=NULL;
            for (RcValue *cases=args; !rc_is_nil(cases); cases=rc_tail_borrowed(cases)) {
                RcValue *clause=rc_head_borrowed(cases); size_t count;
                if (!list_length(ctx,clause,&count) || !count_is(ctx,count,2,"cond clause")) break;
                RcValue *test=evaluate(ctx,nth(clause,0),env,false);
                if (!test) break;
                bool yes=rc_is_true(test), valid=boolean(test); rc_release(test);
                if (!valid) { rc_error(ctx,"eval-kl: cond test must be boolean"); break; }
                if (yes) { next=rc_retain(nth(clause,1)); break; }
            }
            if (!next) { if (!rc_has_error(ctx)) rc_error(ctx,"eval-kl: no true cond case"); break; }
            rc_release(expr); expr=next; continue;
        }
        if (named(head,"and") || named(head,"or")) {
            bool is_and=named(head,"and");
            if (!count_is(ctx,n,2,is_and ? "and" : "or")) break;
            RcValue *first=evaluate(ctx,nth(args,0),env,false);
            if (!first) break;
            if (!boolean(first)) { rc_release(first); rc_error(ctx,"eval-kl: boolean arguments required"); break; }
            if (rc_is_true(first)!=is_and) { result=first; break; }
            rc_release(first);
            result=rc_require_boolean(ctx,evaluate(ctx,nth(args,1),env,tail));
            break;
        }
        if (named(head,"lambda") || named(head,"freeze")) {
            bool freeze=named(head,"freeze");
            if (!count_is(ctx,n,freeze ? 1 : 2,freeze ? "freeze" : "lambda")) break;
            RcValue *params=rc_nil();
            if (!freeze) {
                if (rc_type(nth(args,0))!=RC_SYMBOL) { rc_error(ctx,"eval-kl: lambda binder must be a symbol"); break; }
                params=rc_cons(ctx,nth(args,0),rc_nil());
                if (!params) break;
            }
            result=make_function(ctx,params,nth(args,freeze ? 0 : 1),env);
            rc_release(params); break;
        }
        if (named(head,"defun")) {
            if (!count_is(ctx,n,3,"defun")) break;
            RcValue *name=nth(args,0);
            if (rc_type(name)!=RC_SYMBOL) { rc_error(ctx,"eval-kl: defun name must be a symbol"); break; }
            RcValue *fn=make_function(ctx,nth(args,1),nth(args,2),rc_nil());
            if (!fn) break;
            if (rc_function_set(ctx,name,fn)) result=rc_retain(name);
            rc_release(fn); break;
        }
        if (named(head,"trap-error")) {
            if (!count_is(ctx,n,2,"trap-error")) break;
            result=evaluate(ctx,nth(args,0),env,false);
            if (result) break;
            RcValue *error=rc_string(ctx,rc_error_message(ctx));
            if (!error) break;
            rc_clear_error(ctx);
            RcValue *handler=evaluate(ctx,nth(args,1),env,false);
            if (handler) { result=tail ? rc_tailcall(ctx,handler,1,&error) : rc_apply(ctx,handler,1,&error); rc_release(handler); }
            rc_release(error); break;
        }
        if (named(head,"type")) {
            if (!count_is(ctx,n,2,"type")) break;
            RcValue *next=rc_retain(nth(args,0)); rc_release(expr); expr=next; continue;
        }
        if (named(head,"c-quote") || named(head,"quote")) {
            if (!count_is(ctx,n,1,rc_text(head))) break;
            result=rc_retain(nth(args,0)); break;
        }
        result=application(ctx,expr,env,n,tail); break;
    }
    rc_release(expr); rc_release(env); return result;
}
RcValue *rc_eval(RcContext *ctx, RcValue *expression) {
    return evaluate(ctx,expression,rc_nil(),false);
}
static RcValue *eval_native(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *argv) {
    (void)self; (void)argc; return rc_eval(ctx,argv[0]);
}
bool rc_install_eval(RcContext *ctx) {
    RcValue *name=rc_symbol(ctx,"eval-kl"), *fn=rc_closure(ctx,1,eval_native,0,NULL);
    bool ok=name && fn && rc_function_set(ctx,name,fn);
    rc_release(name); rc_release(fn); return ok;
}
