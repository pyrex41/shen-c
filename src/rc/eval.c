#include "eval.h"
#include "native_compile.h"
#include "primitives.h"
#include "rc_internal.h"
#include <stdlib.h>
#include <string.h>

/* Immutable managed frames keep closure captures in the collector graph.
 * Function parameters share one frame; each let adds one inline binding. */
static RcValue *evaluate(RcContext *, RcValue *, RcValue *, bool);
typedef enum { F_CALL, F_IF, F_LET, F_COND, F_AND, F_OR, F_LAMBDA,
    F_FREEZE, F_DEFUN, F_TRAP, F_TYPE, F_QUOTE } Form;
/* Classify once per expression. Symbols are interned, while this text dispatch
 * also avoids installing or retaining compiler-only special-form constants. */
static Form form_of(RcValue *head) {
    if(rc_type(head)!=RC_SYMBOL)return F_CALL;
    const char *s=rc_text(head);
    switch(*s) {
    case 'i':if(!strcmp(s,"if"))return F_IF;break;
    case 'l':if(!strcmp(s,"let"))return F_LET;if(!strcmp(s,"lambda"))return F_LAMBDA;break;
    case 'c':if(!strcmp(s,"cond"))return F_COND;if(!strcmp(s,"c-quote"))return F_QUOTE;break;
    case 'a':if(!strcmp(s,"and"))return F_AND;break;
    case 'o':if(!strcmp(s,"or"))return F_OR;break;
    case 'f':if(!strcmp(s,"freeze"))return F_FREEZE;break;
    case 'd':if(!strcmp(s,"defun"))return F_DEFUN;break;
    case 't':if(!strcmp(s,"trap-error"))return F_TRAP;if(!strcmp(s,"type"))return F_TYPE;break;
    case 'q':if(!strcmp(s,"quote"))return F_QUOTE;break;
    }
    return F_CALL;
}
static bool boolean(RcValue *v) { return rc_is_boolean(v); }
static RcValue *lookup(RcValue *env, RcValue *name) {
    return rc_env_lookup(env,name);
}
/* The caller has excluded pair evaluation. Atomic values cannot execute
 * callbacks; retain the lexical value or literal before its owner can leave. */
static inline RcValue *atomic_value(RcContext *ctx, RcValue *input, RcValue *env) {
    if (!input) return rc_error(ctx,"eval-kl: null expression");
    RcValue *value=rc_type(input)==RC_SYMBOL ? lookup(env,input) : NULL;
    return rc_retain(value ? value : input);
}
static RcValue *extend(RcContext *ctx, RcValue *env, RcValue *name, RcValue *value) {
    return rc_env_frame(ctx,env,1,&name,&value);
}
static bool list_length(RcContext *ctx, RcValue *list, size_t *n) {
    RcListLengthStatus status=rc_list_length_cached(list,n);
    if(status==RC_LIST_LENGTH_OK)return true;
    rc_error(ctx,status==RC_LIST_LENGTH_IMPROPER ? "eval-kl: improper expression list"
                                               : "eval-kl: expression list length overflow");
    return false;
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
            if (name==rc_head_borrowed(q)) { rc_error(ctx,"eval-kl: duplicate parameter %s",rc_text(name)); return false; }
    }
    return true;
}
static RcValue *prepared_call(RcContext *,RcValue *,size_t,RcValue *const *);
static RcValue *plan_prepare(RcContext *,RcValue *,RcValue *,size_t,RcValue *const *,bool);
/* Optional native-tier hotness observer; absent hooks cost one branch. */
static inline void tier_call(RcContext *ctx, RcValue *self) {
    void *state; const RcTierHooks *hooks=rc_tier_hooks(ctx,&state);
    if(hooks)hooks->call(ctx,state,self);
}
static RcValue *dynamic_call(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *argv) {
    if(!rc_interpreter_enter(ctx)||!rc_stack_check(ctx))return NULL;
    RcValue *const *captures=rc_eval_captures(ctx,self,dynamic_call,argc);if(!captures)return NULL;
    RcValue *plan=plan_prepare(ctx,captures[0],captures[1],argc,captures+2,true);
    if(plan) {
        rc_eval_plan_promote(self,prepared_call,plan);rc_release(plan);
        return prepared_call(ctx,self,argc,argv);
    }
    if(rc_has_error(ctx))return NULL;
    tier_call(ctx,self);
    RcValue *body=NULL;
    RcValue *env=rc_eval_env_frame(ctx,self,dynamic_call,argc,argv,&body);
    if(!env)return NULL;
    /* A nested invocation can promote the same closure while this older
     * invocation is traversing its syntax. Hold that historical root locally
     * until the original evaluator finishes. */
    rc_retain(body);
    RcValue *result=evaluate(ctx,body,env,true);
    rc_release(body);rc_release(env);return result;
}
static RcValue *make_function(RcContext *ctx, RcValue *params, RcValue *body, RcValue *env) {
    size_t arity;
    if (!parameters(ctx,params,&arity)) return NULL;
    if(arity>SIZE_MAX/sizeof(RcValue *)-2)return rc_error(ctx,"eval-kl: parameter capture size overflow");
    RcValue *small[10];size_t count=arity+2;
    RcValue **captures=arity<=8 ? small : malloc(count*sizeof(*captures));
    if(!captures)return rc_error(ctx,"eval-kl: out of memory constructing parameter captures");
    captures[0]=body;captures[1]=env;
    for(size_t i=0;i<arity;i++,params=rc_tail_borrowed(params))captures[i+2]=rc_head_borrowed(params);
    /* parameters() has proved symbol keys are distinct. A direct reference
     * to parameter two has no effects and cannot inspect parameter one. */
    bool returns_second=arity==2 && body==captures[3];
    RcValue *plan=returns_second ? NULL : plan_prepare(ctx,body,env,arity,captures+2,false);
    if(plan)captures[0]=plan;
    RcValue *function=rc_has_error(ctx) ? NULL : returns_second ? rc_closure_return_second(ctx,dynamic_call,count,captures)
        : rc_closure(ctx,arity,plan ? prepared_call : dynamic_call,count,captures);
    rc_release(plan);
    if(captures!=small)free(captures);
    return function;
}
static RcValue *application(RcContext *ctx, RcValue *expr, RcValue *env, size_t argc, bool tail, RcValue *lexical_head) {
    RcValue *head=rc_head_borrowed(expr);
    RcValue *function=lexical_head ? rc_retain(lexical_head)
        : rc_type(head)==RC_SYMBOL ? rc_function_get(ctx,head)
        : evaluate(ctx,head,env,false);
    if (!function) return NULL;
    /* Function position may evaluate to a symbol via a lexical alias or an
     * arbitrary expression. Snapshot its definition before argument effects. */
    if (rc_type(function)==RC_SYMBOL) {
        RcValue *resolved=rc_function_get(ctx,function);
        rc_release(function);
        function=resolved;
        if (!function) return NULL;
    }
    if (tail && argc==2 && rc_closure_returns_second(ctx,function)) {
        /* The selected closure remains owned across both argument effects.
         * The exact body only returns argument two, whose owned result/request
         * is acquired before dead argument/callee owners leave. In particular,
         * deferred requests own every value needed by the next callback. */
        RcValue *args=rc_tail_borrowed(expr),*left=rc_head_borrowed(args);
        RcValue *ignored=rc_type(left)==RC_PAIR ? evaluate(ctx,left,env,false)
            : atomic_value(ctx,left,env);
        if (!ignored) { rc_release(function); return NULL; }
        RcValue *right=rc_head_borrowed(rc_tail_borrowed(args));
        bool valid_first=rc_call_values_belong(ctx,function,1,&ignored);
        RcValue *result=rc_type(right)==RC_PAIR ? evaluate(ctx,right,env,valid_first)
            : atomic_value(ctx,right,env);
        if (result && !valid_first) {
            /* Keep right-side effects/errors before the original operand
             * rejection, and preserve its exact generic diagnostic. */
            RcValue *argv[2]={ignored,result};
            result=rc_primitive_call_rooted_owned(ctx,function,2,argv,true);
            rc_release(argv[0]);rc_release(argv[1]);rc_release(function);
            return result;
        }
        rc_release(ignored); rc_release(function);
        if (!result) return NULL;
        if (rc_has_error(ctx)) { rc_release(result); return NULL; }
        result=rc_eval_require_argument_context(ctx,result);
        if(!result)return NULL;
        rc_safepoint(ctx);
        return result;
    }
    /* Partial cleanup reads only assigned slots; complete dispatch initializes
     * all argc entries. Keep the heap allocation path unchanged. */
    RcValue *small[8];
    RcValue **argv=argc<=8 ? small : calloc(argc,sizeof(*argv));
    if (!argv) { rc_release(function); return rc_error(ctx,"eval-kl: out of memory"); }
    RcValue *rest=rc_tail_borrowed(expr), *result=NULL;
    size_t i=0;
    for (; i<argc; ++i,rest=rc_tail_borrowed(rest)) {
        RcValue *argument=rc_head_borrowed(rest);
        argv[i]=rc_type(argument)==RC_PAIR ? evaluate(ctx,argument,env,false)
            : atomic_value(ctx,argument,env);
        if (!argv[i]) break;
    }
    if (i==argc) result=rc_primitive_call_rooted_owned(ctx,function,argc,argv,tail);
    for (size_t j=0; j<i; ++j) rc_release(argv[j]);
    if(argv!=small)free(argv); rc_release(function);
    return result;
}
#include "eval_plan.h"

static RcValue *evaluate(RcContext *ctx, RcValue *input, RcValue *environment, bool tail) {
    if(!rc_interpreter_enter(ctx))return NULL;
    if (!input) return rc_error(ctx,"eval-kl: null expression");
    /* Atomic lookup cannot invoke callbacks or collect. The caller keeps the
     * expression/environment rooted until this owned result is retained. */
    if (rc_type(input)!=RC_PAIR) return atomic_value(ctx,input,environment);
    /* The public entry owns input once; dynamic calls own their captured body
     * through the active closure. Recursive callers keep the same AST root
     * alive. All loop transitions select immutable descendants of that root.
     * Only let-created frames introduce a local environment owner. */
    RcValue *expr=input, *env=environment, *result=NULL;
    bool owned_env=false;
    for (;;) {
        if (rc_type(expr)!=RC_PAIR) {
            result=atomic_value(ctx,expr,env); break;
        }
        RcValue *head=rc_head_borrowed(expr), *args=rc_tail_borrowed(expr);
        size_t n;
        if (!list_length(ctx,args,&n)) break;
        RcValue *lexical_head=rc_type(head)==RC_SYMBOL ? lookup(env,head) : NULL;
        if (lexical_head) {
            result=application(ctx,expr,env,n,tail,lexical_head); break;
        }
        Form form=form_of(head);
        if (form==F_IF) {
            if (!count_is(ctx,n,3,"if")) break;
            RcValue *test=evaluate(ctx,nth(args,0),env,false);
            if (!test) break;
            bool yes=rc_is_true(test), valid=boolean(test); rc_release(test);
            if (!valid) { rc_error(ctx,"eval-kl: if test must be boolean"); break; }
            expr=nth(args,yes ? 1 : 2); continue;
        }
        if (form==F_LET) {
            if (!count_is(ctx,n,3,"let")) break;
            RcValue *name=nth(args,0);
            if (rc_type(name)!=RC_SYMBOL) { rc_error(ctx,"eval-kl: let binder must be a symbol"); break; }
            RcValue *value=evaluate(ctx,nth(args,1),env,false);
            if (!value) break;
            RcValue *next_env=extend(ctx,env,name,value); rc_release(value);
            if (!next_env) break;
            expr=nth(args,2);
            /* next_env has acquired its parent edge before the old local
             * owner is dropped; a borrowed incoming environment stays rooted
             * by the caller and must never be released here. */
            if (owned_env) rc_release(env);
            env=next_env; owned_env=true; continue;
        }
        if (form==F_COND) {
            RcValue *next=NULL;
            for (RcValue *cases=args; !rc_is_nil(cases); cases=rc_tail_borrowed(cases)) {
                RcValue *clause=rc_head_borrowed(cases); size_t count;
                if (!list_length(ctx,clause,&count) || !count_is(ctx,count,2,"cond clause")) break;
                RcValue *test=evaluate(ctx,nth(clause,0),env,false);
                if (!test) break;
                bool yes=rc_is_true(test), valid=boolean(test); rc_release(test);
                if (!valid) { rc_error(ctx,"eval-kl: cond test must be boolean"); break; }
                if (yes) { next=nth(clause,1); break; }
            }
            if (!next) { if (!rc_has_error(ctx)) rc_error(ctx,"eval-kl: no true cond case"); break; }
            expr=next; continue;
        }
        if (form==F_AND || form==F_OR) {
            bool is_and=form==F_AND;
            if (!count_is(ctx,n,2,is_and ? "and" : "or")) break;
            RcValue *first=evaluate(ctx,nth(args,0),env,false);
            if (!first) break;
            if (!boolean(first)) { rc_release(first); rc_error(ctx,"eval-kl: boolean arguments required"); break; }
            if (rc_is_true(first)!=is_and) { result=first; break; }
            rc_release(first);
            result=rc_require_boolean(ctx,evaluate(ctx,nth(args,1),env,tail));
            break;
        }
        if (form==F_LAMBDA || form==F_FREEZE) {
            bool freeze=form==F_FREEZE;
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
        if (form==F_DEFUN) {
            if (!count_is(ctx,n,3,"defun")) break;
            RcValue *name=nth(args,0);
            if (rc_type(name)!=RC_SYMBOL) { rc_error(ctx,"eval-kl: defun name must be a symbol"); break; }
            RcValue *fn=make_function(ctx,nth(args,1),nth(args,2),rc_nil());
            if (!fn) break;
            const RcValue *old=rc_function_slot_borrowed(ctx,name);
            if (rc_function_set(ctx,name,fn)) {
                void *state; const RcTierHooks *hooks=rc_tier_hooks(ctx,&state);
                if (hooks) hooks->defun(ctx,state,name,expr,fn,old);
                result=rc_retain(name);
            }
            rc_release(fn); break;
        }
        if (form==F_TRAP) {
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
        if (form==F_TYPE) {
            if (!count_is(ctx,n,2,"type")) break;
            expr=nth(args,0); continue;
        }
        if (form==F_QUOTE) {
            if (!count_is(ctx,n,1,rc_text(head))) break;
            result=rc_retain(nth(args,0)); break;
        }
        result=application(ctx,expr,env,n,tail,NULL); break;
    }
    if (owned_env) rc_release(env);
    return result;
}
RcValue *rc_eval(RcContext *ctx, RcValue *expression) {
    /* All environment binders and expression symbols then share one intern table,
     * so lexical lookup can compare symbol identity without structural equality. */
    if(!rc_call_values_belong(ctx,expression,0,NULL))return rc_error(ctx,"eval-kl: invalid expression/context");
    if(getenv("SHEN_RC_NATIVE_TRACE")){fputs("RC_NATIVE_FORM ",stderr);rc_print(stderr,expression);fputc('\n',stderr);}
    int backend=rc_execution_mode(ctx);
    if(backend==0)return rc_native_eval(ctx,expression);
    if(backend!=1)return rc_error(ctx,"unknown SHEN_RC_EXECUTION backend");
    /* Public borrowed inputs may lose an external owner during a callback.
     * Keep a pair root across the complete evaluation; atoms cannot invoke
     * callbacks and keep the allocation-free owned-result path above. */
    if (rc_type(expression)!=RC_PAIR) return evaluate(ctx,expression,rc_nil(),false);
    RcValue *root=rc_retain(expression);
    RcValue *result=evaluate(ctx,root,rc_nil(),false);
    rc_release(root);
    return result;
}
RcValue *rc_interpret_fallback(RcContext *ctx, RcValue *expression, RcValue *parameters) {
    if (parameters) return make_function(ctx,parameters,expression,rc_nil());
    return evaluate(ctx,expression,rc_nil(),false);
}
static RcValue *eval_native(RcContext *ctx, RcValue *self, size_t argc, RcValue *const *argv) {
    (void)self; (void)argc;
    void *state; const RcTierHooks *hooks=rc_tier_hooks(ctx,&state);
    if (hooks && !rc_has_error(ctx)) hooks->safepoint(ctx,state);
    return rc_eval(ctx,argv[0]);
}
bool rc_install_eval(RcContext *ctx) {
    RcValue *name=rc_symbol(ctx,"eval-kl"), *fn=rc_closure(ctx,1,eval_native,0,NULL);
    bool ok=name && fn && rc_function_set(ctx,name,fn);
    rc_release(name); rc_release(fn); return ok;
}
