/* OP=0..7 arithmetic/comparisons, OP=8 unmodified generic hd control.
 * Compile RC_BENCH_NUMERIC=1 for tiny APIs, 0 for identical generic controls. */
#define _POSIX_C_SOURCE 200809L
#include "primitives.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef RC_BENCH_NUMERIC
#define RC_BENCH_NUMERIC 0
#endif
#if RC_BENCH_NUMERIC
typedef RcValue *(*Tiny)(RcContext *,RcValue *,RcValue **,bool);
static Tiny apis[]={rc_primitive_add_owned,rc_primitive_sub_owned,rc_primitive_mul_owned,
 rc_primitive_div_owned,rc_primitive_gt_owned,rc_primitive_lt_owned,rc_primitive_ge_owned,rc_primitive_le_owned};
#endif
static double now(void) {
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;
}
int main(int argc,char **argv) {
 int op=argc>1?atoi(argv[1]):0;size_t n=argc>2?(size_t)strtoull(argv[2],NULL,10):3000000;
 if(op<0||op>8||!n||n>100000000)return 2;
 const char *names[]={"+","-","*","/",">","<",">=","<=","hd"};
 RcContext *c=rc_context_new();if(!c||!rc_install_primitives(c))return 3;
 RcValue *symbol=rc_symbol(c,names[op]),*fn=rc_function_get(c,symbol);
 RcValue *left=rc_integer(c,6),*right=rc_integer(c,3),*pair=rc_cons(c,left,rc_nil());
 RcValue *input=op==8?pair:left;size_t arity=op==8?1:2;
 RcStats before=rc_stats(c);double start=now();
#define RUN_CALL(call) \
 for(size_t i=0;i<n;i++) { \
  RcValue *args[]={rc_retain(input),arity==2?rc_retain(right):NULL}; \
  RcValue *out=(call); \
  if(!out||args[0]||(arity==2&&args[1]))return 4; \
  rc_release(out); \
 }
#if RC_BENCH_NUMERIC
 if(op<8) { RUN_CALL(apis[op](c,fn,args,false)); }
 else
#endif
 { RUN_CALL(rc_primitive_call_owned(c,fn,arity,args,false)); }
#undef RUN_CALL
 double seconds=now()-start;RcStats after=rc_stats(c);
 printf("op=%d tiny=%d calls=%zu ns/call=%.3f allocations=%zu retains=%zu releases=%zu\n",
 op,RC_BENCH_NUMERIC,n,seconds*1e9/n,after.total_allocations-before.total_allocations,
 after.retain_calls-before.retain_calls,after.release_calls-before.release_calls);
 rc_release(pair);rc_release(right);rc_release(left);rc_release(fn);rc_release(symbol);rc_context_free(c);return 0;
}
