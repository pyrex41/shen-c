#include "primitives.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef RcValue *(*Tiny)(RcContext *,RcValue *,RcValue **,bool);
static const char *names[]={"hd","tl","cons?","number?","string?","="};
static Tiny apis[]={rc_primitive_hd_owned,rc_primitive_tl_owned,rc_primitive_consp_owned,
                    rc_primitive_numberp_owned,rc_primitive_stringp_owned,rc_primitive_eq_owned};
static RcValue *function(RcContext *c,const char *name) {
 RcValue *s=rc_symbol(c,name),*f=rc_function_get(c,s);assert(f);rc_release(s);return f;
}
static RcValue *identity(RcContext *c,RcValue *self,size_t n,RcValue *const *args) {
 (void)c;(void)self;assert(n==1||n==2);return rc_retain(args[0]);
}
static void checked_calls(void) {
 RcContext *c=rc_context_new(),*foreign=rc_context_new();
 assert(c&&foreign&&rc_install_primitives(c)&&rc_install_primitives(foreign));
 RcValue *text=rc_string(c,"child"),*pair=rc_cons(c,text,rc_nil());
 for(size_t op=0;op<6;op++) {
  size_t n=op==5?2:1;RcValue *f=function(c,names[op]);
  for(int tail=0;tail<2;tail++) {
   RcValue *input=op<3?pair:op==3?rc_integer(c,42):text;
   RcValue *args[]={rc_retain(input),op==5?rc_retain(input):NULL};
   RcValue *out=apis[op](c,f,args,tail!=0);assert(out&&!args[0]&&(n==1||!args[1]));
   if(op==0)assert(out==text);else if(op==1)assert(rc_is_nil(out));else assert(rc_is_true(out));
   rc_release(out);if(op==3)rc_release(input);
  }
  RcValue *args[]={rc_string(foreign,"bad"),n==2?rc_retain(text):NULL};
  assert(!apis[op](c,f,args,false)&&rc_has_error(c)&&!args[0]&&(n==1||!args[1]));rc_clear_error(c);
  assert(!apis[op](c,f,NULL,false)&&rc_has_error(c));rc_clear_error(c);
  args[0]=NULL;args[1]=n==2?rc_retain(text):NULL;
  assert(!apis[op](c,f,args,true)&&rc_has_error(c)&&(n==1||!args[1]));rc_clear_error(c);
  RcValue *ff=function(foreign,names[op]);
  args[0]=rc_retain(pair);args[1]=n==2?rc_retain(pair):NULL;
  assert(!apis[op](c,ff,args,false)&&rc_has_error(c)&&!args[0]&&(n==1||!args[1]));rc_clear_error(c);rc_release(ff);
  /* Same-context redefinitions and canonical functions for other operations
   * execute the actual selected closure, preserving captured snapshots. */
  RcValue *replacement=rc_closure(c,n,identity,0,NULL);
  args[0]=rc_retain(text);args[1]=n==2?rc_retain(pair):NULL;
  RcValue *out=apis[op](c,replacement,args,false);
  assert(out==text&&!args[0]&&(n==1||!args[1]));rc_release(out);rc_release(replacement);
  rc_error(c,"pending");args[0]=rc_retain(pair);args[1]=n==2?rc_retain(pair):NULL;
  assert(!apis[op](c,f,args,false)&&rc_has_error(c)&&!args[0]&&(n==1||!args[1]));rc_clear_error(c);
  rc_release(f);
 }
 RcValue *tl=function(c,"tl"),*args[]={rc_retain(pair)};
 RcValue *out=rc_primitive_hd_owned(c,tl,args,false);assert(out&&rc_is_nil(out)&&!args[0]);rc_release(out);rc_release(tl);
 RcValue *cons=function(c,"cons"),*part=rc_apply(c,cons,1,&text);
 args[0]=rc_retain(pair);out=rc_primitive_hd_owned(c,part,args,false);
 assert(out&&rc_head_borrowed(out)==text&&rc_tail_borrowed(out)==pair&&!args[0]);
 rc_release(out);rc_release(part);rc_release(cons);
 RcValue *hd=function(c,"hd");args[0]=rc_nil();
 assert(!rc_primitive_hd_owned(c,hd,args,false)&&rc_has_error(c)&&!args[0]);rc_clear_error(c);
 rc_release(hd);rc_release(pair);rc_release(text);rc_context_free(foreign);rc_context_free(c);
}
static void reinstall_roots(void) {
 RcContext *c=rc_context_new();assert(c&&rc_install_primitives(c));
 RcValue *old=function(c,"hd"),*text=rc_string(c,"rooted"),*pair=rc_cons(c,text,rc_nil());
 RcValue *name=rc_symbol(c,"hd"),*replacement=rc_closure(c,1,identity,0,NULL);
 assert(rc_function_set(c,name,replacement));rc_release(replacement);
 rc_collect(c);RcValue *args[]={rc_retain(pair)};
 RcValue *out=rc_primitive_hd_owned(c,old,args,false);assert(out==text);rc_release(out);
 size_t objects=0;
 for(int i=0;i<10;i++) {
  assert(rc_install_primitives(c));rc_collect(c);
  if(i==0)objects=rc_stats(c).live_objects;else assert(rc_stats(c).live_objects==objects);
  args[0]=rc_retain(pair);out=rc_primitive_hd_owned(c,old,args,false);
  assert(out==text&&!args[0]);rc_release(out);
  RcValue *current=function(c,"hd");args[0]=rc_retain(pair);
  out=rc_primitive_hd_owned(c,current,args,false);assert(out==text&&!args[0]);rc_release(out);rc_release(current);
 }
 rc_release(name);rc_release(old);rc_release(pair);rc_release(text);rc_context_free(c);
}
static void tail_values(void) {
 RcContext *c=rc_context_new();assert(c&&rc_install_primitives(c));
 RcValue *plus=function(c,"+");
 for(int op=0;op<2;op++)for(int tail=0;tail<2;tail++)for(int guard=0;guard<2;guard++) {
  RcValue *f=function(c,names[op]),*nums[]={rc_integer(c,20),rc_integer(c,22)};
  RcValue *request=rc_tailcall_owned_args(c,plus,2,nums);assert(request);
  if(guard)request=rc_require_boolean(c,request);
  RcValue *args[]={op==0?rc_cons(c,request,rc_nil()):rc_cons(c,rc_nil(),request)};rc_release(request);
  RcValue *out=apis[op](c,f,args,tail!=0);assert(!args[0]);
  if(tail){assert(out&&rc_type(out)==RC_TAIL);rc_release(out);}
  else if(guard){assert(!out&&rc_has_error(c));rc_clear_error(c);}
  else {assert(out&&rc_integer_value(out)==42);rc_release(out);}
  rc_release(f);
 }
 rc_release(plus);rc_context_free(c);
}
int main(void) {
 checked_calls();reinstall_roots();tail_values();
 puts("canonical primitive identities: ownership, fallbacks, bounded roots and tail results PASS");
 return 0;
}
