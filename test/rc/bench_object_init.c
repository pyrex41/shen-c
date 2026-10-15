#define _POSIX_C_SOURCE 200809L
#include "rc.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
static RcValue *identity(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args){(void)ctx;(void)self;return argc?rc_retain(args[0]):rc_nil();}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (double)t.tv_sec+(double)t.tv_nsec*1e-9;}
int main(int argc,char **argv){
 if(argc!=3)return 2;size_t n=(size_t)strtoull(argv[2],NULL,10);RcContext *ctx=rc_context_new();if(!ctx)return 2;
 RcValue *value=rc_integer(ctx,42),*fn=rc_closure(ctx,1,identity,0,NULL);double start=seconds();
 if(!strcmp(argv[1],"pair"))for(size_t i=0;i<n;i++){RcValue *p=rc_cons(ctx,value,rc_nil());if(!p)return 3;rc_release(p);}
 else for(size_t i=0;i<n;i++){
  RcValue *number=rc_number(ctx,(double)(i&255)+0.5),*p=rc_cons(ctx,number,rc_nil());if(!number||!p)return 3;
  rc_release(number);RcValue *tail=rc_tailcall(ctx,fn,1,&p);if(!tail)return 3;rc_release(p);rc_release(tail);
 }
 double elapsed=seconds()-start;RcStats s=rc_stats(ctx);
 printf("{\"mode\":\"%s\",\"iterations\":%zu,\"seconds\":%.9f,\"allocations\":%zu,\"system_allocations\":%zu,\"cached_bytes\":%zu}\n",argv[1],n,elapsed,s.total_allocations,s.system_allocations,s.cached_bytes);
 rc_release(fn);rc_release(value);rc_context_free(ctx);return 0;
}
