#include "primitives.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef RC_CONS_BASELINE
#define rc_primitive_cons_owned(c,f,a,t) rc_primitive_call_owned(c,f,2,a,t)
#endif
static RcValue *get(RcContext *c,const char *name) {
 RcValue *s=rc_symbol(c,name),*f=rc_function_get(c,s);rc_release(s);if(!f)abort();return f;
}
int main(int argc,char **argv) {
 if(argc<2)return 2;size_t n=argc>2?(size_t)strtoull(argv[2],NULL,10):2000000;
 RcContext *c=rc_context_new();if(!c||!rc_install_primitives(c))return 3;
 RcValue *fn=get(c,"cons"),*tl=get(c,"tl"),*head=rc_string(c,"head");
 if(!strcmp(argv[1],"old")){if(!rc_install_primitives(c))return 4;}
 RcValue *candidate=rc_cons(c,rc_nil(),rc_nil()),*shared=rc_cons(c,head,rc_nil());
 int mode=!strcmp(argv[1],"owned")?0:!strcmp(argv[1],"aliased")?1:!strcmp(argv[1],"tail")?2:3;
 for(size_t i=0;i<n;i++) {
  if(mode==0) {
   RcValue *a[]={rc_retain(head),rc_nil()},*out=rc_primitive_cons_owned(c,fn,a,false);
   if(!out||a[0]||a[1])abort();rc_release(out);
  } else if(mode==1) {
   RcValue *out=rc_cons_call_reuse(c,fn,rc_retain(shared),head,rc_nil(),false);
   if(!out||out==shared)abort();rc_release(out);
  } else if(mode==2) {
   RcValue *out=NULL,*value=rc_tail_call_reuse(c,tl,candidate,&out);
   if(value!=rc_nil()||!out)abort();rc_release(value);
   candidate=rc_cons_reuse(c,out,head,rc_nil());if(!candidate)abort();
  } else {
   RcValue *out=rc_cons_call_reuse(c,fn,candidate,head,rc_nil(),false);
   if(out!=candidate)abort();candidate=out;
  }
 }
 if(rc_has_error(c))abort();RcStats s=rc_stats(c);
 printf("%s %zu reuses=%zu misses=%zu retains=%zu releases=%zu\n",argv[1],n,s.pair_reuses,s.pair_reuse_misses,s.retain_calls,s.release_calls);
 rc_release(candidate);rc_release(shared);rc_release(head);rc_release(fn);rc_release(tl);rc_context_free(c);
}
