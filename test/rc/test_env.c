#include "rc.h"
#include "eval.h"
#include "primitives.h"
#include "read.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

static RcValue *captured(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)ctx;(void)argc;(void)args;return rc_retain(rc_capture(self,0));
}
static void direct(void) {
    RcContext *ctx=rc_context_new(),*foreign=rc_context_new();assert(ctx&&foreign);
    RcValue *keys[10],*values[10];
    for(size_t i=0;i<10;i++){char key[20];snprintf(key,sizeof(key),"env-key-%zu",i);keys[i]=rc_symbol(ctx,key);values[i]=rc_integer(ctx,(int64_t)i);assert(keys[i]&&values[i]);}
    rc_collect(ctx);size_t baseline=rc_stats(ctx).live_objects;
    for(size_t turn=0;turn<100;turn++) {
        size_t allocations=rc_stats(ctx).total_allocations;
        RcValue *parent=rc_env_frame(ctx,rc_nil(),1,keys,values);assert(parent&&rc_type(parent)==RC_ENV);
        assert(rc_stats(ctx).total_allocations==allocations+1);
        RcValue *child=rc_env_frame(ctx,parent,10,keys,values);assert(child);
        assert(rc_stats(ctx).total_allocations==allocations+2);
        assert(rc_env_lookup(child,keys[9])==values[9]);
        RcValue *shadow=rc_env_frame(ctx,child,1,keys,values+9);assert(shadow);
        assert(rc_env_lookup(shadow,keys[0])==values[9]);assert(rc_env_lookup(parent,keys[0])==values[0]);
        RcValue *empty=rc_env_frame(ctx,shadow,0,NULL,NULL);assert(empty==shadow);rc_release(empty);
        rc_release(parent);rc_release(child);rc_collect(ctx);assert(rc_env_lookup(shadow,keys[8])==values[8]);
        rc_release(shadow);rc_collect(ctx);assert(rc_stats(ctx).live_objects==baseline);
        RcValue *number=rc_number(ctx,1.25);assert(number);rc_release(number);
    }
    RcValue *wrong=rc_symbol(foreign,"env-key-0"),*bad=rc_env_frame(ctx,rc_nil(),1,&wrong,values);
    assert(!bad&&rc_has_error(ctx));rc_clear_error(ctx);
    bad=rc_env_frame(ctx,rc_nil(),1,keys,&wrong);assert(!bad&&rc_has_error(ctx));rc_clear_error(ctx);
    bad=rc_env_frame(ctx,values[0],1,keys,values);assert(!bad&&rc_has_error(ctx));rc_clear_error(ctx);
    bad=rc_env_frame(ctx,rc_nil(),SIZE_MAX,keys,values);assert(!bad&&rc_has_error(ctx));rc_clear_error(ctx);
    rc_release(wrong);rc_context_free(foreign);
    for(size_t turn=0;turn<100;turn++) {
        RcValue *vector=rc_vector(ctx,1);assert(vector);
        RcValue *parent=rc_env_frame(ctx,rc_nil(),1,keys,&vector);assert(parent);
        RcValue *child=rc_env_frame(ctx,parent,10,keys,values);assert(child);
        RcValue *closure=rc_closure(ctx,0,captured,1,&child);assert(closure);
        assert(rc_vector_set(ctx,vector,0,closure));
        rc_release(parent);rc_release(child);rc_release(vector);
        rc_collect(ctx); /* The rooted closure must keep the entire cycle. */
        RcValue *frame=rc_apply(ctx,closure,0,NULL);assert(frame);
        RcValue *held=rc_env_lookup(frame,keys[0]);assert(held==values[0]);rc_release(frame);
        rc_release(closure);rc_collect(ctx);assert(rc_stats(ctx).live_objects==baseline);
    }
    for(size_t i=0;i<10;i++){rc_release(keys[i]);rc_release(values[i]);}
    rc_context_free(ctx);
}
static void eval_number(RcContext *ctx,const char *source,int64_t expected) {
    RcValue *form=rc_read(ctx,&source);assert(form);RcValue *value=rc_eval(ctx,form);rc_release(form);
    assert(value&&!rc_has_error(ctx)&&rc_integer_value(value)==expected);rc_release(value);
}
static void interpreted(void) {
    RcContext *ctx=rc_context_new();assert(ctx&&rc_install_primitives(ctx)&&rc_install_eval(ctx));
    eval_number(ctx,"(let X 7 (let F (freeze X) (let X 9 (F))))",7);
    eval_number(ctx,"(let X 7 (let F (lambda X (lambda Y (+ X Y))) (let X 99 ((F 20) 22))))",42);
    eval_number(ctx,"(let if (lambda X X) (if 42))",42);
    eval_number(ctx,"(type (let X 42 X) (unbound type term))",42);
    eval_number(ctx,"(let X 40 (trap-error (let X 99 (simple-error \"inner\")) (lambda E (+ X 2))))",42);
    const char *definition="(defun ten-params (A B C D E F G H I J) (+ A J))";
    RcValue *form=rc_read(ctx,&definition),*value=rc_eval(ctx,form);assert(value);rc_release(value);rc_release(form);
    eval_number(ctx,"(ten-params 20 1 2 3 4 5 6 7 8 22)",42);
    eval_number(ctx,"(let F (ten-params 20 1 2) (F 3 4 5 6 7 8 22))",42);
    eval_number(ctx,"(do (defun ten-params (A B C D E F G H I J) 42) (ten-params 0 0 0 0 0 0 0 0 0 0))",42);
    const char *source="(let V (absvector 1) (let X 40 (let F (lambda Y (+ X (<-address V 0))) (do (address-> V 0 F) V))))";
    form=rc_read(ctx,&source);assert(form);rc_collect(ctx);size_t baseline=rc_stats(ctx).live_objects;
    for(size_t i=0;i<100;i++){value=rc_eval(ctx,form);assert(value);rc_release(value);rc_collect(ctx);assert(rc_stats(ctx).live_objects==baseline);}
    rc_release(form);
    source="(let A 1 (let B 2 (let C (absvector 2) (simple-error \"unwind\"))))";
    form=rc_read(ctx,&source);assert(form);rc_collect(ctx);baseline=rc_stats(ctx).live_objects;
    for(size_t i=0;i<100;i++){assert(!rc_eval(ctx,form)&&rc_has_error(ctx));rc_clear_error(ctx);rc_collect(ctx);assert(rc_stats(ctx).live_objects==baseline);}
    rc_release(form);rc_context_free(ctx);
}
static void acyclic(void) {
    RcContext *ctx=rc_context_new_with_policy(RC_ACYCLIC_ONLY);assert(ctx);
    RcValue *key=rc_symbol(ctx,"acyclic-env"),*vector=rc_vector(ctx,1);assert(key&&vector);
    RcValue *frame=rc_env_frame(ctx,rc_nil(),1,&key,&vector);assert(frame);
    RcValue *closure=rc_closure(ctx,0,captured,1,&frame);assert(closure);
    assert(!rc_vector_set(ctx,vector,0,closure)&&rc_has_error(ctx));rc_clear_error(ctx);
    RcValue *item=rc_vector_get(ctx,vector,0);assert(rc_is_nil(item));rc_release(item);
    rc_release(closure);rc_release(frame);rc_release(vector);rc_release(key);rc_context_free(ctx);
}
int main(void) {direct();interpreted();acyclic();puts("RC managed evaluator environments: PASS");}
