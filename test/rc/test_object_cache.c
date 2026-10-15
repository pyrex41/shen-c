/* Physical object-cache bounds, logical accounting, payloads, and poisoning. */
#include "rc.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
static RcValue *identity(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)ctx;(void)self;return argc?rc_retain(args[0]):rc_nil();
}
static RcValue *captured(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)ctx;(void)argc;(void)args;return rc_retain(rc_capture(self,0));
}
static void recycled_defaults(void) {
    RcContext *ctx=rc_context_new();assert(ctx);
    RcValue *integer=rc_integer(ctx,INT64_MAX);rc_release(integer);
    RcValue *value=rc_number(ctx,2.5);
    assert(!rc_number_is_exact_integer(value)&&rc_number_value(value)==2.5);
    RcValue *fn=rc_closure(ctx,3,identity,0,NULL),*args[]={value,value,value};
    RcValue *tail=rc_tailcall(ctx,fn,3,args);rc_release(tail);
    /* The old tail's inline argv occupies the new symbol's slot offsets. */
    RcValue *symbol=rc_symbol(ctx,"fresh-pooled-symbol");
    assert(!rc_function_lookup(ctx,symbol));
    assert(!rc_global_get(ctx,symbol)&&rc_has_error(ctx));rc_clear_error(ctx);
    RcValue *part=rc_apply(ctx,fn,1,&value);assert(part);rc_release(part);
    RcValue *zero=rc_closure(ctx,0,identity,0,NULL);
    assert(rc_arity(zero)==0);RcValue *out=rc_apply(ctx,zero,0,NULL);
    assert(rc_is_nil(out));rc_release(out);rc_release(zero);
    rc_release(fn);fn=rc_closure(ctx,1,identity,0,NULL);
    tail=rc_require_boolean(ctx,rc_tailcall(ctx,fn,1,&value));assert(tail);rc_release(tail);
    tail=rc_tailcall(ctx,fn,1,&value);
    RcValue *wrapper=rc_closure(ctx,0,captured,1,&tail);
    out=rc_apply(ctx,wrapper,0,NULL);assert(out==value&&!rc_has_error(ctx));rc_release(out);
    rc_release(wrapper);rc_release(tail);rc_release(fn);rc_release(value);rc_release(symbol);
    rc_context_free(ctx);
}
int main(int argc,char **argv) {
    RcContext *ctx=rc_context_new();assert(ctx);
    if(argc==2&&!strcmp(argv[1],"--uaf")) {
        RcValue *dead=rc_number(ctx,42.0);rc_release(dead);
        /* No reuse intervenes: ASan must still diagnose the stale handle. */
        printf("stale type=%d\n",(int)rc_type(dead));
        rc_context_free(ctx);return 0;
    }
    recycled_defaults();
    enum { LIMIT=1024, TOTAL=LIMIT*2+9 };
    RcValue *values[TOTAL];
    for(size_t i=0;i<TOTAL;i++)values[i]=rc_number(ctx,(double)i);
    for(size_t i=0;i<TOTAL;i++)rc_release(values[i]);
    RcStats s=rc_stats(ctx);
    assert(!s.live_objects&&!s.live_bytes&&s.total_allocations==s.total_frees);
    assert(s.cached_objects==LIMIT&&s.system_allocations==TOTAL&&s.system_frees==TOTAL-LIMIT);
    size_t block=s.cached_bytes/LIMIT;
    assert(block&&s.cached_bytes==LIMIT*block&&s.peak_cached_bytes==s.cached_bytes);
    for(size_t i=0;i<LIMIT;i++)values[i]=rc_number(ctx,(double)i);
    assert(rc_stats(ctx).system_allocations==s.system_allocations);
    assert(!rc_stats(ctx).cached_objects&&!rc_stats(ctx).cached_bytes);
    for(size_t i=0;i<LIMIT;i++){assert(rc_number_value(values[i])==(double)i);rc_release(values[i]);}
    RcContext *other=rc_context_new();RcValue *independent=rc_number(other,7.0);
    assert(rc_stats(other).system_allocations==1&&!rc_stats(other).cached_objects);
    rc_release(independent);rc_context_free(other);
    /* Exercise freeing/reusing every separately allocated payload kind. */
    for(size_t n=0;n<8;n++) {
        RcValue *text=rc_string(ctx,"payload"),*vector=rc_vector(ctx,n);
        for(size_t i=0;i<n;i++)assert(rc_vector_set(ctx,vector,i,text));
        RcValue *captures[]={vector,text};
        RcValue *fn=rc_closure(ctx,n,identity,2,captures),*args[8];
        for(size_t i=0;i<n;i++)args[i]=text;
        RcValue *tail=rc_tailcall(ctx,fn,n,args);
        rc_release(text);rc_release(vector);rc_release(fn);rc_release(tail);
    }
    /* Collector-freed values enter the same bounded cache, never the graph. */
    for(size_t i=0;i<10;i++) {
        RcValue *vector=rc_vector(ctx,1),*fn=rc_closure(ctx,1,identity,1,&vector);
        assert(rc_vector_set(ctx,vector,0,fn));rc_release(fn);rc_release(vector);
    }
    rc_collect(ctx);s=rc_stats(ctx);
    assert(!s.live_objects&&!s.live_bytes&&s.total_allocations==s.total_frees);
    assert(s.cached_objects<=LIMIT&&s.cached_bytes==s.cached_objects*block);
    assert(s.system_allocations-s.system_frees==s.cached_objects);
    assert(s.cycle_objects_freed==20);
    printf("bounded object cache: PASS; block=%zu cached=%zu reserved=%zu index=%zu bytes\n",
           block,s.cached_objects,s.cached_bytes,LIMIT*sizeof(RcValue *));
    /* Teardown drains a full dead cache plus live values with payloads. */
    RcValue *live=rc_vector(ctx,1),*text=rc_string(ctx,"teardown");
    assert(rc_vector_set(ctx,live,0,text));rc_release(text);
    rc_context_free(ctx);return 0;
}
