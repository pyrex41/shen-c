#include "rc.h"
#include "eval.h"
#include "primitives.h"
#include "read.h"
#include "rc_internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static RcValue *captured(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)ctx;(void)argc;(void)args;return rc_retain(rc_capture(self,0));
}
static void classes(void) {
    RcContext *ctx=rc_context_new();assert(ctx);
    const char *names[]={"A","Z","a","z","+","","\303\251clair"};
    RcValue *keys[7],*values[7];
    for(size_t i=0;i<7;i++){keys[i]=rc_symbol(ctx,names[i]);values[i]=rc_integer(ctx,(int64_t)i);assert(keys[i]&&values[i]);}
    for(size_t turn=0;turn<100;turn++) {
        RcValue *env=rc_nil();
        for(size_t i=0;i<7;i++) {
            RcValue *next=rc_env_frame(ctx,env,1,keys+i,values+i);assert(next);rc_release(env);env=next;
            for(size_t j=0;j<7;j++)assert(rc_env_lookup(env,keys[j])==(j<=i?values[j]:NULL));
        }
        RcValue *shadow=rc_env_frame(ctx,env,1,keys+2,values+6);assert(shadow);
        assert(rc_env_lookup(shadow,keys[2])==values[6]&&rc_env_lookup(env,keys[2])==values[2]);
        for(size_t i=0;i<512;i++) {
            RcValue *next=rc_env_frame(ctx,shadow,1,keys,values);assert(next);rc_release(shadow);shadow=next;
        }
        for(size_t i=0;i<7;i++)assert(rc_env_lookup(shadow,keys[i])==(i==2?values[6]:values[i]));
        rc_release(shadow);rc_release(env);rc_collect(ctx);
        /* A pooled frame that used to contain all classes must reset to one. */
        env=rc_env_frame(ctx,rc_nil(),1,keys,values);assert(env);
        assert(!rc_env_lookup(env,keys[2])&&!rc_env_lookup(env,keys[4]));rc_release(env);
    }
    for(size_t i=0;i<7;i++){rc_release(keys[i]);rc_release(values[i]);}
    rc_context_free(ctx);
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
    bad=rc_env_frame(ctx,rc_nil(),1,values,values);assert(!bad&&rc_has_error(ctx));rc_clear_error(ctx);
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
    eval_number(ctx,"(let + (lambda X (lambda Y X)) ((+ 42) 0))",42);
    eval_number(ctx,"(let lower 42 (let Upper 0 lower))",42);
    eval_number(ctx,"(let + 42 (let Upper 0 +))",42);
    eval_number(ctx,"(type (let X 42 X) (unbound type term))",42);
    eval_number(ctx,"(let X 40 (trap-error (let X 99 (simple-error \"inner\")) (lambda E (+ X 2))))",42);
    const char *definition="(defun ten-params (A B C D E F G H I J) (+ A J))";
    RcValue *form=rc_read(ctx,&definition),*value=rc_eval(ctx,form);assert(value);rc_release(value);rc_release(form);
    eval_number(ctx,"(ten-params 20 1 2 3 4 5 6 7 8 22)",42);
    eval_number(ctx,"(let F (ten-params 20 1 2) (F 3 4 5 6 7 8 22))",42);
    eval_number(ctx,"(do (defun ten-params (A B C D E F G H I J) 42) (ten-params 0 0 0 0 0 0 0 0 0 0))",42);
    const char *source="(let V (absvector 1) (let X 40 (let F (lambda Y (+ X (<-address V 0))) (do (address-> V 0 F) V))))";
    form=rc_read(ctx,&source);assert(form);
    /* Prepare once; every subsequent evaluation must reclaim its whole graph. */
    for(size_t i=0;i<64;i++){value=rc_eval(ctx,form);assert(value);rc_release(value);}
    rc_collect(ctx);size_t baseline=rc_stats(ctx).live_objects;
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
static RcValue *eval_value(RcContext *ctx,const char *source) {
    RcValue *form=rc_read(ctx,&source);assert(form);RcValue *value=rc_eval(ctx,form);rc_release(form);
    assert(value&&!rc_has_error(ctx));return value;
}
static void flattened_parameters(void) {
    RcContext *ctx=rc_context_new();assert(ctx&&rc_install_primitives(ctx)&&rc_install_eval(ctx));
    size_t arities[]={0,1,8,9,32};
    for(size_t i=0;i<sizeof(arities)/sizeof(*arities);i++) {
        size_t n=arities[i];char definition[4096],call[4096];
        size_t d=(size_t)snprintf(definition,sizeof(definition),"(defun flat-%zu (",n);
        size_t c=(size_t)snprintf(call,sizeof(call),"(flat-%zu",n);
        for(size_t j=0;j<n;j++){d+=(size_t)snprintf(definition+d,sizeof(definition)-d," P%zu",j);c+=(size_t)snprintf(call+c,sizeof(call)-c," %d",j+1==n?42:0);}
        if(n)snprintf(definition+d,sizeof(definition)-d,") P%zu)",n-1);else snprintf(definition+d,sizeof(definition)-d,") 42)");
        snprintf(call+c,sizeof(call)-c,")");RcValue *value=eval_value(ctx,definition);rc_release(value);
        eval_number(ctx,call,42);
    }
    eval_number(ctx,"(let F (flat-9 0 0 0) (F 0 0 0 0 0 42))",42);
    eval_number(ctx,"(let X 40 (((lambda Y (lambda Z (+ X (+ Y Z)))) 1) 1))",42);
    const char *bad[]={"(defun flat-duplicate (X X) X)","(defun flat-invalid (X 1) X)"};
    const char *diagnostics[]={"duplicate parameter","parameter must be a symbol"};
    for(size_t i=0;i<2;i++){const char *source=bad[i];RcValue *form=rc_read(ctx,&source);assert(form);assert(!rc_eval(ctx,form)&&rc_has_error(ctx));assert(strstr(rc_error_message(ctx),diagnostics[i]));rc_clear_error(ctx);rc_release(form);}
    RcValue *value=eval_value(ctx,"(defun flat-over (A B C D E F G H I) (lambda Last (+ A Last)))");rc_release(value);
    eval_number(ctx,"(flat-over 20 0 0 0 0 0 0 0 0 22)",42);
    value=eval_value(ctx,"(defun flat-owned (X) (let F (freeze X) (do (defun flat-owned (X) 99) (do (shen.gc) (F)))))");rc_release(value);
    RcValue *name=rc_symbol(ctx,"flat-owned"),*args[]={rc_integer(ctx,42)};
    RcValue *fn=rc_function_get_borrowed(ctx,name);assert(fn);
    value=rc_apply_owned_args(ctx,fn,1,args);assert(value&&rc_integer_value(value)==42&&!args[0]);rc_release(value);
    eval_number(ctx,"(flat-owned 0)",99);rc_release(name);
    value=eval_value(ctx,"(defun flat-old (X) (+ X 1))");rc_release(value);
    name=rc_symbol(ctx,"flat-old");fn=rc_function_get(ctx,name);assert(fn);
    value=eval_value(ctx,"(defun flat-old (X) 99)");rc_release(value);rc_collect(ctx);
    args[0]=rc_integer(ctx,41);value=rc_apply_owned_args(ctx,fn,1,args);assert(value&&rc_integer_value(value)==42&&!args[0]);rc_release(value);rc_release(fn);rc_release(name);
    value=eval_value(ctx,"(defun flat-cycle (A B C D E F G H I) (lambda X A))");rc_release(value);
    const char *source="(let V (absvector 1) (let F (flat-cycle V 0 0 0 0 0 0 0 0) (do (address-> V 0 F) V)))";
    RcValue *form=rc_read(ctx,&source);assert(form);
    /* Prepare once; every subsequent evaluation must reclaim its whole graph. */
    for(size_t i=0;i<64;i++){value=rc_eval(ctx,form);assert(value);rc_release(value);}
    rc_collect(ctx);size_t baseline=rc_stats(ctx).live_objects;
    for(size_t i=0;i<100;i++){value=rc_eval(ctx,form);assert(value);rc_release(value);rc_collect(ctx);assert(rc_stats(ctx).live_objects==baseline);}
    rc_release(form);rc_context_free(ctx);
}
static void capture_span_guards(void) {
    RcContext *ctx=rc_context_new(),*foreign=rc_context_new();assert(ctx&&foreign);
    RcValue *key=rc_symbol(ctx,"span-key"),*caps[]={rc_nil(),rc_nil(),key};assert(key);
    RcValue *fn=rc_closure(ctx,1,captured,3,caps);assert(fn);
    RcValue *const *span=rc_eval_captures(ctx,fn,captured,1);assert(span&&span[2]==key);
    RcValue *argument=rc_integer(ctx,42),*body=NULL;
    RcValue *frame=rc_eval_env_frame(ctx,fn,captured,1,&argument,&body);assert(frame&&rc_env_lookup(frame,key)==argument&&rc_is_nil(body));rc_release(frame);
    assert(!rc_eval_env_frame(foreign,fn,captured,1,&argument,&body)&&!body&&rc_has_error(foreign));rc_clear_error(foreign);
    assert(!rc_eval_env_frame(ctx,fn,NULL,1,&argument,&body)&&!body&&rc_has_error(ctx));rc_clear_error(ctx);
    assert(!rc_eval_env_frame(ctx,fn,captured,0,NULL,&body)&&!body&&rc_has_error(ctx));rc_clear_error(ctx);
    assert(!rc_eval_env_frame(ctx,fn,captured,SIZE_MAX,&argument,&body)&&!body&&rc_has_error(ctx));rc_clear_error(ctx);
    assert(!rc_eval_env_frame(ctx,fn,captured,1,NULL,&body)&&!body&&rc_has_error(ctx));rc_clear_error(ctx);
    assert(!rc_eval_env_frame(ctx,fn,captured,1,&argument,NULL)&&rc_has_error(ctx));rc_clear_error(ctx);
    assert(!rc_eval_captures(foreign,fn,captured,1)&&rc_has_error(foreign));rc_clear_error(foreign);
    assert(!rc_eval_captures(ctx,fn,NULL,1)&&rc_has_error(ctx));rc_clear_error(ctx);
    assert(!rc_eval_captures(ctx,fn,captured,0)&&rc_has_error(ctx));rc_clear_error(ctx);
    assert(!rc_eval_captures(ctx,fn,captured,SIZE_MAX)&&rc_has_error(ctx));rc_clear_error(ctx);
    rc_release(fn);fn=rc_closure(ctx,1,captured,2,caps);assert(fn);
    assert(!rc_eval_captures(ctx,fn,captured,1)&&rc_has_error(ctx));rc_clear_error(ctx);
    assert(!rc_eval_env_frame(ctx,fn,captured,1,&argument,&body)&&!body&&rc_has_error(ctx));rc_clear_error(ctx);
    rc_release(argument);rc_release(fn);rc_release(key);rc_context_free(foreign);rc_context_free(ctx);
}
int main(void) {classes();direct();interpreted();acyclic();flattened_parameters();capture_span_guards();puts("RC managed evaluator environments: PASS");}
