/* Request-path microbenchmark, not a language-suite performance claim. */
#define _POSIX_C_SOURCE 200809L
#include "rc.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static RcValue *last(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)ctx;(void)self;return rc_retain(args[argc-1]);
}
static RcValue *last_owned(RcContext *ctx,RcValue *self,size_t argc,RcValue **args) {
    (void)ctx;(void)self;RcValue *out=args[argc-1];args[argc-1]=NULL;return out;
}
static RcValue *shared(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)ctx;(void)args;assert(!argc);return rc_retain(rc_capture(self,0));
}
static RcValue *fresh(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)args;assert(!argc);size_t n=(size_t)rc_integer_value(rc_capture(self,2));
    RcValue *actual[32];for(size_t i=0;i<n;i++)actual[i]=rc_capture(self,1);
    return rc_tailcall(ctx,rc_capture(self,0),n,actual);
}
static double elapsed(struct timespec a,struct timespec b) {
    return (double)(b.tv_sec-a.tv_sec)+(double)(b.tv_nsec-a.tv_nsec)/1e9;
}
int main(int argc,char **argv) {
    assert(argc==4);const char *mode=argv[1];size_t n=(size_t)strtoul(argv[2],NULL,10);
    size_t iterations=(size_t)strtoul(argv[3],NULL,10);assert(n>=1&&n<=32&&iterations);
    bool owned=!strcmp(mode,"owned"),bound=!strcmp(mode,"bound"),new_request=!strcmp(mode,"fresh");
    assert(owned||bound||new_request||!strcmp(mode,"shared"));
    RcContext *ctx=rc_context_new();assert(ctx);
    RcValue *value=rc_cons(ctx,rc_nil(),rc_nil());
    RcValue *fn=owned?rc_closure_owned(ctx,n,last_owned,0,NULL):rc_closure(ctx,n+(bound?1:0),last,0,NULL);
    if(bound){RcValue *p=rc_apply(ctx,fn,1,&value);assert(p);rc_release(fn);fn=p;}
    RcValue *actual[32];for(size_t i=0;i<n;i++)actual[i]=value;
    RcValue *request=rc_tailcall(ctx,fn,n,actual);assert(request);
    RcValue *count=rc_integer(ctx,(int64_t)n),*captures[]={fn,value,count};
    RcValue *wrapper=new_request?rc_closure(ctx,0,fresh,3,captures):rc_closure(ctx,0,shared,1,&request);
    assert(wrapper);RcStats before=rc_stats(ctx);struct timespec start,end,cpu_start,cpu_end;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&cpu_start);clock_gettime(CLOCK_MONOTONIC,&start);
    for(size_t i=0;i<iterations;i++) {
        RcValue *out=rc_apply(ctx,wrapper,0,NULL);assert(out==value&&!rc_has_error(ctx));rc_release(out);
    }
    clock_gettime(CLOCK_MONOTONIC,&end);clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&cpu_end);RcStats after=rc_stats(ctx);
    printf("{\"mode\":\"%s\",\"arity\":%zu,\"iterations\":%zu,\"seconds\":%.9f,\"cpu_seconds\":%.9f,\"allocations\":%zu,\"frees\":%zu,\"retains\":%zu,\"releases\":%zu,\"collections\":%zu}\n",
           mode,n,iterations,elapsed(start,end),elapsed(cpu_start,cpu_end),after.total_allocations-before.total_allocations,
           after.total_frees-before.total_frees,after.retain_calls-before.retain_calls,
           after.release_calls-before.release_calls,after.collections-before.collections);
    rc_release(wrapper);rc_release(count);rc_release(request);rc_release(fn);rc_release(value);rc_collect(ctx);
    assert(!rc_stats(ctx).live_objects);rc_context_free(ctx);
}
