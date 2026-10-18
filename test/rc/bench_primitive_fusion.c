/* Bounded release microbenchmark. No kernel or full-suite dependency.
 * mode 0: primitive hd, 1: generated-like lookup/hd/drop,
 * mode 2: primitive cons, 3: integer addition. Optional iteration argument. */
#define _POSIX_C_SOURCE 200809L
#include "primitives.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void) {
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
 return t.tv_sec+t.tv_nsec*1e-9;
}
int main(int argc,char **argv) {
 int mode=argc>1?atoi(argv[1]):0;
 size_t n=argc>2?(size_t)strtoull(argv[2],NULL,10):3000000;
 if(mode<0||mode>3||!n||n>100000000)return 2;
 RcContext *ctx=rc_context_new();if(!ctx||!rc_install_primitives(ctx))return 3;
 RcValue *symbol=rc_symbol(ctx,mode==2?"cons":mode==3?"+":"hd");
 RcValue *fn=rc_function_get(ctx,symbol),*head=rc_string(ctx,"head");
 RcValue *pair=rc_cons(ctx,head,rc_nil());if(!fn||!head||!pair)return 4;
 size_t arity=mode>=2?2:1;
 RcStats before=rc_stats(ctx);double start=now();
 for(size_t i=0;i<n;i++) {
  RcValue *callee=mode==1?rc_function_get(ctx,symbol):fn;
  RcValue *args[2];
  if(mode==3){args[0]=rc_integer(ctx,7);args[1]=rc_integer(ctx,2);}
  else {args[0]=rc_retain(mode==2?head:pair);args[1]=mode==2?rc_retain(pair):NULL;}
  RcValue *out=rc_primitive_call_owned(ctx,callee,arity,args,false);
  if(mode==1)rc_release(callee);
  if(!out||args[0]||(arity==2&&args[1]))return 5;
  rc_release(out);
 }
 double seconds=now()-start;RcStats after=rc_stats(ctx);
 printf("mode=%d calls=%zu ns/call=%.3f allocations=%zu retains=%zu releases=%zu\n",
        mode,n,seconds*1e9/n,after.total_allocations-before.total_allocations,
        after.retain_calls-before.retain_calls,after.release_calls-before.release_calls);
 rc_release(pair);rc_release(head);rc_release(fn);rc_release(symbol);rc_context_free(ctx);return 0;
}
