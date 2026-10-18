/* Bounded interpreted-call and closure-creation controls; not a suite benchmark. */
#include "rc.h"
#include "eval.h"
#include "primitives.h"
#include "read.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(int argc,char **argv) {
    assert(argc==3);bool creation=!strcmp(argv[1],"create"),public_env=!strcmp(argv[1],"env");size_t n=(size_t)strtoul(argv[2],NULL,10);assert(n>=1&&n<=32);
    RcContext *ctx=rc_context_new();assert(ctx&&rc_install_primitives(ctx)&&rc_install_eval(ctx));
    char definition[4096];size_t used=(size_t)snprintf(definition,sizeof(definition),"(defun bench-flat (");
    for(size_t i=0;i<n;i++)used+=(size_t)snprintf(definition+used,sizeof(definition)-used," P%zu",i);
    snprintf(definition+used,sizeof(definition)-used,") P%zu)",n-1);
    const char *source=creation&&n==1?"(lambda X X)":definition;
    RcValue *form=rc_read(ctx,&source);assert(form);RcValue *fn=NULL,*name=NULL;
    RcValue *args[32],*keys[32];for(size_t i=0;i<n;i++){char key[20];snprintf(key,sizeof(key),"P%zu",i);keys[i]=rc_symbol(ctx,key);args[i]=rc_integer(ctx,42);assert(args[i]&&keys[i]);}
    if(!creation&&!public_env){RcValue *value=rc_eval(ctx,form);assert(value);rc_release(value);name=rc_symbol(ctx,"bench-flat");fn=rc_function_get(ctx,name);assert(fn);}
    size_t iterations=creation?40000:80000;RcStats before=rc_stats(ctx);struct timespec begin,end,cpu_begin,cpu_end;clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&cpu_begin);clock_gettime(CLOCK_MONOTONIC,&begin);
    for(size_t i=0;i<iterations;i++){RcValue *value=creation?rc_eval(ctx,form):public_env?rc_env_frame(ctx,rc_nil(),n,keys,args):rc_apply(ctx,fn,n,args);assert(value&&!rc_has_error(ctx));if(public_env)assert(rc_env_lookup(value,keys[n-1])==args[n-1]);else if(!creation)assert(rc_integer_value(value)==42);rc_release(value);}
    clock_gettime(CLOCK_MONOTONIC,&end);clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&cpu_end);RcStats after=rc_stats(ctx);
    double seconds=(double)(end.tv_sec-begin.tv_sec)+(end.tv_nsec-begin.tv_nsec)/1e9;
    double cpu=(double)(cpu_end.tv_sec-cpu_begin.tv_sec)+(cpu_end.tv_nsec-cpu_begin.tv_nsec)/1e9;
    printf("{\"mode\":\"%s\",\"arity\":%zu,\"iterations\":%zu,\"seconds\":%.9f,\"cpu_seconds\":%.9f,\"allocations\":%zu,\"capture_edges\":%zu,\"peak_objects\":%zu}\n",creation?"create":public_env?"env":"call",n,iterations,seconds,cpu,after.total_allocations-before.total_allocations,after.capture_edges_created-before.capture_edges_created,after.peak_objects);
    for(size_t i=0;i<n;i++){rc_release(args[i]);rc_release(keys[i]);}rc_release(fn);rc_release(name);rc_release(form);rc_context_free(ctx);
}
