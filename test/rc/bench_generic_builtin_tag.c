/* Modes0..13 are generic canonical primitives. Controls14=user,15=cons,
 * 16=partial+,17=old-hd snapshot,18=direct tiny hd. All bounded runtime-only. */
#define _POSIX_C_SOURCE 200809L
#include "primitives.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static RcValue *identity(RcContext *c,RcValue *s,size_t n,RcValue *const *args) {
 (void)c;(void)s;(void)n;return rc_retain(args[0]);
}
static double now(void) {
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;
}
int main(int argc,char **argv) {
 int mode=argc>1?atoi(argv[1]):0;size_t n=argc>2?(size_t)strtoull(argv[2],NULL,10):1000000;
 if(mode<0||mode>18||!n||n>100000000)return 2;
 const char *names[]={"hd","tl","cons?","number?","string?","=","+","-","*","/",">","<",">=","<=","hd","cons","+","hd","hd"};
 RcContext *c=rc_context_new();if(!c||!rc_install_primitives(c))return 3;
 RcValue *symbol=rc_symbol(c,names[mode]),*fn=rc_function_get(c,symbol);
 RcValue *text=rc_string(c,"heap"),*left=rc_integer(c,6),*right=rc_integer(c,3),*pair=rc_cons(c,text,rc_nil());
 size_t arity=mode>=5&&mode<=13?2:mode==15?2:1;
 RcValue *input=mode==3||(mode>=6&&mode<=13)||mode==16?left:mode==4||mode==14?text:pair;
 RcValue *second=mode==5||mode==15?pair:right;
 if(mode==14){rc_release(fn);fn=rc_closure(c,1,identity,0,NULL);}
 if(mode==16){RcValue *p=rc_apply(c,fn,1,&right);rc_release(fn);fn=p;}
 if(mode==17&&!rc_install_primitives(c))return 4;
 RcStats before=rc_stats(c);double start=now();
 for(size_t i=0;i<n;i++) {
  RcValue *args[]={rc_retain(input),arity==2?rc_retain(second):NULL};
  RcValue *out=mode==18?rc_primitive_hd_owned(c,fn,args,false):rc_primitive_call_owned(c,fn,arity,args,false);
  if(!out||args[0]||(arity==2&&args[1]))return 5;
  rc_release(out);
 }
 double seconds=now()-start;RcStats after=rc_stats(c);
 printf("mode=%d calls=%zu ns/call=%.3f allocations=%zu retains=%zu releases=%zu\n",mode,n,seconds*1e9/n,
 after.total_allocations-before.total_allocations,after.retain_calls-before.retain_calls,after.release_calls-before.release_calls);
 rc_release(pair);rc_release(right);rc_release(left);rc_release(text);rc_release(fn);rc_release(symbol);rc_context_free(c);return 0;
}
