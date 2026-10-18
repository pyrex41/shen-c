#include "rc.h"
#include "rc_internal.h"
#include "primitives.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static RcValue *tail(RcContext *c,RcValue *fn,RcValue *value) {
 RcValue *args[]={value};RcValue *out=rc_apply(c,fn,1,args);assert(out);return out;
}
static void bytes(RcValue *value,const char *expected,size_t n) {
 assert(rc_type(value)==RC_STRING&&rc_text_length(value)==n);
 assert(!memcmp(rc_text(value),expected,n)&&rc_text(value)[n]==0);
}
static RcValue *capture(RcContext *c,RcValue *self,size_t n,RcValue *const *a) {
 (void)c;(void)n;(void)a;return rc_retain(rc_capture(self,0));
}
int main(void) {
 RcContext *c=rc_context_new();assert(c&&rc_install_primitives(c));rc_set_collection_interval(c,1);
 RcValue *symbol=rc_symbol(c,"tlstr"),*fn=rc_function_get(c,symbol);rc_release(symbol);assert(fn);
 size_t base=rc_stats(c).live_objects,base_bytes=rc_stats(c).live_bytes;
 char data[4096];for(size_t i=0;i<sizeof(data);i++)data[i]=(char)(i%251);
 RcValue *value=rc_string_n(c,data,sizeof(data));assert(value);
 for(size_t offset=1;offset<=sizeof(data);offset++) {
  RcValue *next=tail(c,fn,value);rc_release(value);value=next;
  if(offset%17==0)rc_collect(c);
  size_t n=sizeof(data)-offset;bytes(value,data+offset,n);
  assert(rc_stats(c).live_bytes-base_bytes<=4*(n+1)+3*128);
 }
 rc_release(value);rc_collect(c);assert(rc_stats(c).live_objects==base);

 /* Borrowed text survives source release, closure escape, and collection. */
 RcValue *source=rc_string_n(c,data,256),*view=tail(c,fn,source);rc_release(source);
 RcValue *captured[]={view};RcValue *closure=rc_closure(c,0,capture,1,captured);assert(closure);
 RcValue *cycle=rc_vector(c,2);assert(cycle&&rc_vector_set(c,cycle,0,cycle)&&rc_vector_set(c,cycle,1,view));
 rc_release(view);rc_release(cycle);rc_collect(c);
 value=rc_apply(c,closure,0,NULL);assert(value);bytes(value,data+1,255);
 rc_release(closure);rc_collect(c);bytes(value,data+1,255);rc_release(value);
 rc_collect(c);assert(rc_stats(c).live_objects==base);

 /* An unreachable cycle must reclaim its view AND its hidden backing owner. */
 source=rc_string_n(c,data,256);view=tail(c,fn,source);rc_release(source);
 cycle=rc_vector(c,2);assert(cycle&&rc_vector_set(c,cycle,0,cycle)&&rc_vector_set(c,cycle,1,view));
 rc_release(view);rc_release(cycle);size_t freed=rc_stats(c).cycle_objects_freed;rc_collect(c);
 assert(rc_stats(c).cycle_objects_freed>freed&&rc_stats(c).live_objects==base);

 /* Exercise pooled storage changing between owning strings, views and vectors. */
 for(size_t i=0;i<100;i++) {
  source=rc_string_n(c,data,256);view=tail(c,fn,source);rc_release(source);
  RcValue *nested=tail(c,fn,view);rc_release(view);rc_collect(c);bytes(nested,data+2,254);rc_release(nested);
  RcValue *v=rc_vector(c,2);assert(v);rc_release(v);
 }
 source=rc_string(c,"");RcValue *args[]={source};assert(!rc_apply(c,fn,1,args));
 assert(!strcmp(rc_error_message(c),"tlstr requires nonempty string"));rc_clear_error(c);rc_release(source);
 rc_collect(c);assert(rc_stats(c).live_objects==base);
#if SHEN_RC_STRING_SLICES && defined(SHEN_RC_FUSION_DIAGNOSTICS)
 assert(rc_fusion_stats(c).count[RC_FUSION_STRING_VIEWS]>0);
#endif
 rc_release(fn);rc_context_free(c);
 puts("RC string suffixes: bytes/NUL, retention bound, closure escape, mixed cycles and pooled reuse PASS");return 0;
}
