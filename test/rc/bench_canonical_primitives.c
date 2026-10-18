/* Bounded runtime-only control. Compile with RC_BENCH_CANONICAL=1 for tiny APIs.
 * Usage: bench OP [CALLS]; OP=0 hd,1 tl,2 cons?,3 number?,4 string?,5 =,
 * 6 integer+ (unmodified dispatch control). No kernel dependency. */
#define _POSIX_C_SOURCE 200809L
#include "primitives.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef RC_BENCH_CANONICAL
#define RC_BENCH_CANONICAL 0
#endif
#if RC_BENCH_CANONICAL
typedef RcValue *(*Tiny)(RcContext *,RcValue *,RcValue **,bool);
static Tiny apis[]={rc_primitive_hd_owned,rc_primitive_tl_owned,rc_primitive_consp_owned,
 rc_primitive_numberp_owned,rc_primitive_stringp_owned,rc_primitive_eq_owned};
#endif
static double now(void) {
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;
}
int main(int argc,char **argv) {
 int op=argc>1?atoi(argv[1]):0;
 size_t n=argc>2?(size_t)strtoull(argv[2],NULL,10):3000000;
 if(op<0||op>6||!n||n>100000000)return 2;
 const char *names[]={"hd","tl","cons?","number?","string?","=","+"};
 RcContext *c=rc_context_new();if(!c||!rc_install_primitives(c))return 3;
 RcValue *symbol=rc_symbol(c,names[op]),*fn=rc_function_get(c,symbol);
 RcValue *text=rc_string(c,"head"),*pair=rc_cons(c,text,rc_nil()),*integer=rc_integer(c,7);
 RcValue *input=op==3||op==6?integer:op==4?text:pair;
 size_t arity=op>=5?2:1;RcStats before=rc_stats(c);double start=now();
#define RUN_CALL(call) \
 for(size_t i=0;i<n;i++) { \
  RcValue *args[]={rc_retain(input),arity==2?rc_retain(input):NULL}; \
  RcValue *out=(call); \
  if(!out||args[0]||(arity==2&&args[1]))return 4; \
  rc_release(out); \
 }
#if RC_BENCH_CANONICAL
 if(op<6) { RUN_CALL(apis[op](c,fn,args,false)); }
 else
#endif
 { RUN_CALL(rc_primitive_call_owned(c,fn,arity,args,false)); }
#undef RUN_CALL
 double elapsed=now()-start;RcStats after=rc_stats(c);
 printf("op=%d tiny=%d calls=%zu ns/call=%.3f allocations=%zu retains=%zu releases=%zu\n",
 op,RC_BENCH_CANONICAL,n,elapsed*1e9/n,after.total_allocations-before.total_allocations,
 after.retain_calls-before.retain_calls,after.release_calls-before.release_calls);
 rc_release(integer);rc_release(pair);rc_release(text);rc_release(fn);rc_release(symbol);rc_context_free(c);return 0;
}
