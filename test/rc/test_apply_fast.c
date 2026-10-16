/* Exact-call dispatch ownership, rooting, fallback, and allocation regression. */
#include "rc.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

static RcValue **caller_slots;
static RcValue *borrowed_id(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)ctx;(void)self;assert(argc==1);
    if(caller_slots)assert(!caller_slots[0]);
    return rc_retain(args[0]);
}
static RcValue *owned_id(RcContext *ctx,RcValue *self,size_t argc,RcValue **args) {
    (void)self;assert(argc==1);
    if(caller_slots)assert(!caller_slots[0]);
    rc_collect(ctx);
    RcValue *out=args[0];args[0]=NULL;return out;
}
static RcValue *replace_roots(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    assert(argc==1);
    assert(rc_function_set(ctx,rc_capture(self,0),rc_capture(self,2)));
    assert(rc_global_set(ctx,rc_capture(self,1),rc_nil()));
    rc_collect(ctx);
    assert(rc_type(rc_capture(self,0))==RC_SYMBOL);
    return rc_retain(args[0]);
}
static RcValue *error_result(RcContext *ctx,RcValue *self,size_t argc,RcValue **args) {
    (void)self;assert(argc==1);assert(!caller_slots[0]);
    rc_error(ctx,"intentional callback error with result");
    return rc_retain(args[0]);
}
static RcValue *guarded_zero(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)args;assert(!argc);
    RcValue *arg=rc_capture(self,1);
    return rc_require_boolean(ctx,rc_tailcall(ctx,rc_capture(self,0),1,&arg));
}
static RcValue *many(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)ctx;(void)self;assert(argc==9);return rc_retain(args[8]);
}
static RcValue *return_request(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)ctx;(void)args;assert(!argc);return rc_retain(rc_capture(self,0));
}
static RcValue *last_owned(RcContext *ctx,RcValue *self,size_t argc,RcValue **args) {
    rc_collect(ctx);
    if(!argc)return rc_retain(rc_capture(self,0));
    RcValue *out=args[argc-1];args[argc-1]=NULL;return out;
}
static void inline_tail_cases(void) {
    RcContext *ctx=rc_context_new();assert(ctx);
    RcValue *value=rc_integer(ctx,42);
    const size_t arities[]={0,1,3,4,9};size_t header_bytes=0;
    for(size_t k=0;k<sizeof(arities)/sizeof(arities[0]);k++) {
        size_t n=arities[k];RcValue *fn=rc_closure_owned(ctx,n,last_owned,1,&value);
        for(int owned=0;owned<2;owned++) {
            RcValue *args[9];for(size_t i=0;i<n;i++)args[i]=owned?rc_retain(value):value;
            RcStats before=rc_stats(ctx);
            RcValue *request=owned?rc_tailcall_owned_args(ctx,fn,n,args):rc_tailcall(ctx,fn,n,args);
            assert(request&&!rc_has_error(ctx));
            if(owned)for(size_t i=0;i<n;i++)assert(!args[i]);
            size_t bytes=rc_stats(ctx).live_bytes-before.live_bytes;
            if(!n)header_bytes=bytes;
            assert(bytes==header_bytes+(n>3?n*sizeof(RcValue *):0));
            RcValue *wrapper=rc_closure(ctx,0,return_request,1,&request);
            /* The same shared request remains executable after an owned
             * callback and a collection; its argument edges cannot be stolen. */
            for(int repeat=0;repeat<2;repeat++) {
                RcValue *out=rc_apply(ctx,wrapper,0,NULL);
                assert(out==value&&!rc_has_error(ctx));rc_release(out);
            }
            rc_release(wrapper);rc_release(request);
        }
        rc_release(fn);
    }
    /* A vector/request cycle checks inline and heap payload destruction. */
    for(size_t n=3;n<=4;n++) {
        RcValue *fn=rc_closure_owned(ctx,n,last_owned,1,&value);
        size_t baseline=rc_stats(ctx).live_objects;
        RcValue *vector=rc_vector(ctx,1),*args[4];
        for(size_t i=0;i<n;i++)args[i]=vector;
        RcValue *request=rc_tailcall(ctx,fn,n,args);
        assert(rc_vector_set(ctx,vector,0,request));
        rc_release(vector);rc_release(request);rc_collect(ctx);
        assert(rc_stats(ctx).live_objects==baseline&&!rc_has_error(ctx));rc_release(fn);
    }
    rc_release(value);rc_collect(ctx);assert(rc_stats(ctx).live_objects==0);
    rc_context_free(ctx);
}
static void compiled_call_cases(void) {
    RcContext *ctx=rc_context_new();assert(ctx);
    rc_release(rc_true(ctx));size_t roots=rc_stats(ctx).live_objects;
    RcValue *value=rc_cons(ctx,rc_nil(),rc_nil());
    RcValue *owned=rc_closure_owned(ctx,1,owned_id,0,NULL);
    RcValue *borrowed=rc_closure(ctx,1,borrowed_id,0,NULL);
    RcValue *captured=rc_closure_owned(ctx,1,owned_id,1,&value);
    assert(rc_compiled_call_matches(owned,owned_id,1));
    assert(!rc_compiled_call_matches(owned,last_owned,1));
    assert(!rc_compiled_call_matches(owned,owned_id,2));
    assert(!rc_compiled_call_matches(borrowed,owned_id,1));
    assert(!rc_compiled_call_matches(captured,owned_id,1));
    RcValue *full=rc_closure_owned(ctx,2,last_owned,0,NULL);
    RcValue *partial=rc_apply(ctx,full,1,&value);assert(partial);
    assert(!rc_compiled_call_matches(partial,last_owned,1));
    assert(!rc_compiled_call_matches(partial,last_owned,2));
    RcValue *args[]={rc_retain(value)};
    RcValue *out=owned_id(ctx,owned,1,args);
    out=rc_compiled_call_complete(ctx,out,1,args);
    assert(out==value&&!args[0]&&!rc_has_error(ctx));rc_release(out);
    /* Completion must drop leftovers even when the body fails before taking
     * arguments, or returns a value together with an active error. */
    size_t refs=rc_refcount(value);args[0]=rc_retain(value);
    assert(!rc_compiled_call_complete(ctx,NULL,1,args));
    assert(!args[0]&&rc_refcount(value)==refs);
    args[0]=rc_retain(value);rc_error(ctx,"compiled callback error");
    assert(!rc_compiled_call_complete(ctx,rc_retain(value),1,args));
    assert(!args[0]&&rc_refcount(value)==refs);rc_clear_error(ctx);
    RcValue *yes=rc_true(ctx),*request=rc_tailcall(ctx,borrowed,1,&yes);
    request=rc_require_boolean(ctx,request);args[0]=rc_retain(value);
    out=rc_compiled_call_complete(ctx,request,1,args);
    assert(out==yes&&!args[0]&&!rc_has_error(ctx));rc_release(out);
    request=rc_require_boolean(ctx,rc_tailcall(ctx,borrowed,1,&value));
    args[0]=rc_retain(value);
    assert(!rc_compiled_call_complete(ctx,request,1,args));
    assert(!args[0]&&rc_has_error(ctx));rc_clear_error(ctx);
    rc_release(yes);rc_release(partial);rc_release(full);
    rc_release(captured);rc_release(borrowed);rc_release(owned);rc_release(value);
    rc_collect(ctx);assert(rc_stats(ctx).live_objects==roots);rc_context_free(ctx);
}
int main(void) {
    inline_tail_cases();
    compiled_call_cases();
    RcContext *ctx=rc_context_new();assert(ctx);
    RcValue *value=rc_integer(ctx,42);
    RcValue *borrowed=rc_closure(ctx,1,borrowed_id,0,NULL);
    RcValue *owned=rc_closure_owned(ctx,1,owned_id,0,NULL);
    for(int api=0;api<2;api++)for(int callback=0;callback<2;callback++) {
        RcValue *args[]={rc_retain(value)};caller_slots=api?args:NULL;
        RcStats before=rc_stats(ctx);
        RcValue *out=api?rc_apply_owned_args(ctx,callback?owned:borrowed,1,args):rc_apply(ctx,callback?owned:borrowed,1,args);
        assert(out==value&&!rc_has_error(ctx));
        assert(rc_stats(ctx).total_allocations==before.total_allocations);
        if(api)assert(!args[0]);else rc_release(args[0]);
        rc_release(out);
    }
    caller_slots=NULL;
    RcValue *name=rc_symbol(ctx,"fast-root"),*global=rc_symbol(ctx,"fast-arg");
    RcValue *captures[]={name,global,borrowed};
    RcValue *replacer=rc_closure(ctx,1,replace_roots,3,captures);
    assert(rc_function_set(ctx,name,replacer));rc_release(replacer);
    RcValue *only_global=rc_cons(ctx,value,rc_nil());
    assert(rc_global_set(ctx,global,only_global));rc_release(only_global);
    /* Both inputs are borrowed solely through context roots, which the call
     * removes. Dispatch must root the old closure and argument across collect. */
    RcValue *out=rc_apply(ctx,name,1,&only_global);
    assert(out==only_global&&rc_head_borrowed(out)==value&&!rc_has_error(ctx));rc_release(out);
    RcValue *failing=rc_closure_owned(ctx,1,error_result,0,NULL);
    RcValue *args[]={rc_retain(value)};caller_slots=args;
    size_t live=rc_stats(ctx).live_objects;
    assert(!rc_apply_owned_args(ctx,failing,1,args)&&!args[0]&&rc_has_error(ctx));
    assert(rc_stats(ctx).live_objects==live);rc_clear_error(ctx);caller_slots=NULL;
    /* A partial closure and a nine-argument call exercise both fallbacks. */
    RcValue *large=rc_closure(ctx,9,many,0,NULL),*nine[9];
    for(size_t i=0;i<9;i++)nine[i]=value;
    out=rc_apply(ctx,large,9,nine);assert(out==value);rc_release(out);
    RcValue *part=rc_apply(ctx,large,8,nine);assert(part);
    out=rc_apply(ctx,part,1,&value);assert(out==value);rc_release(out);rc_release(part);
    RcValue *yes=rc_true(ctx),*guard_caps[]={borrowed,yes};
    RcValue *guard=rc_closure(ctx,0,guarded_zero,2,guard_caps);
    out=rc_apply_owned_args(ctx,guard,0,NULL);assert(out==yes&&!rc_has_error(ctx));
    rc_release(out);rc_release(guard);
    guard_caps[1]=value;guard=rc_closure(ctx,0,guarded_zero,2,guard_caps);
    assert(!rc_apply(ctx,guard,0,NULL)&&rc_has_error(ctx));
    rc_clear_error(ctx);rc_release(guard);rc_release(yes);
    RcContext *other=rc_context_new();RcValue *foreign=rc_number(other,1.0);
    RcValue *invalid[]={foreign};
    assert(!rc_apply_owned_args(ctx,owned,1,invalid)&&!invalid[0]&&rc_has_error(ctx));rc_clear_error(ctx);
    rc_context_free(other);
    rc_release(large);rc_release(failing);rc_release(name);rc_release(global);
    rc_release(owned);rc_release(borrowed);rc_release(value);rc_context_free(ctx);
    puts("small exact apply: ownership, roots, errors, allocation and fallbacks PASS");
    return 0;
}
