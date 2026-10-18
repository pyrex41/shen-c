/* 0=cached pair churn,1=deep-list allocation/release,2=exact cycles,
 * 3=allocation-free numeric control. Optional bounded iteration argument. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "primitives.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
static size_t usable(void *p){return malloc_size(p);}
#elif defined(__GLIBC__)
#include <malloc.h>
static size_t usable(void *p){return malloc_usable_size(p);}
#else
static size_t usable(void *p){(void)p;return 0;}
#endif
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char **argv) {
 int mode=argc>1?atoi(argv[1]):0;
 size_t n=argc>2?(size_t)strtoull(argv[2],NULL,10):(mode==1?16:mode==2?200000:1000000);
 if(mode<0||mode>3||!n||n>10000000||(mode==1&&n>128))return 2;
 RcContext *c=rc_context_new();if(!c)return 3;
 RcValue *probe=rc_cons(c,rc_nil(),rc_nil());if(!probe)return 4;
 size_t header=rc_stats(c).live_bytes,block=usable(probe);rc_release(probe);
 RcValue *fn=NULL;
 if(mode==3){if(!rc_install_primitives(c))return 5;RcValue *s=rc_symbol(c,"+");fn=rc_function_get(c,s);rc_release(s);if(!fn)return 6;}
 rc_set_collection_interval(c,mode==2?128:0);
 RcStats before=rc_stats(c);double start=now();
 for(size_t i=0;i<n;i++) {
  if(mode==0){RcValue *v=rc_cons_owned(c,rc_nil(),rc_nil());if(!v)return 7;rc_release(v);}
  else if(mode==1){
   RcValue *list=rc_nil();
   for(size_t j=0;j<16384;j++){list=rc_cons_owned(c,rc_integer(c,(int64_t)j),list);if(!list)return 8;}
   rc_release(list);
  } else if(mode==2){
   RcValue *v=rc_vector(c,1);if(!v||!rc_vector_set(c,v,0,v))return 9;rc_release(v);rc_safepoint(c);
  } else {
   RcValue *args[]={rc_integer(c,6),rc_integer(c,3)},*v=rc_primitive_add_owned(c,fn,args,false);
   if(!v||rc_integer_value(v)!=9||args[0]||args[1])return 10;rc_release(v);
  }
 }
 if(mode==2)rc_collect(c);
 double seconds=now()-start;RcStats after=rc_stats(c);
 printf("mode=%d iterations=%zu seconds=%.6f header=%zu usable=%zu allocations=%zu frees=%zu retains=%zu releases=%zu collections=%zu cycles=%zu live=%zu cached=%zu cached_bytes=%zu\n",
 mode,n,seconds,header,block,after.total_allocations-before.total_allocations,after.total_frees-before.total_frees,
 after.retain_calls-before.retain_calls,after.release_calls-before.release_calls,after.collections-before.collections,
 after.cycle_objects_freed-before.cycle_objects_freed,after.live_objects,after.cached_objects,after.cached_bytes);
 rc_release(fn);rc_context_free(c);return 0;
}
