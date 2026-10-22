#include "primitives.h"
#include "rc_internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
static RcValue *function(RcContext *c,const char *name) {
 RcValue *s=rc_symbol(c,name),*f=rc_function_get(c,s);assert(f);rc_release(s);return f;
}
static RcValue *user_identity(RcContext *c,RcValue *self,size_t n,RcValue *const *a) {
 (void)c;(void)self;assert(n==1);return rc_retain(a[0]);
}
static void reject_arguments(void) {
 RcContext *c=rc_context_new(),*other=rc_context_new();
 assert(c&&other&&rc_install_primitives(c)&&rc_install_primitives(other));
 RcValue *f=function(c,"+"),*foreign=function(other,"+");
 RcValue *first=rc_string(c,"held");
 for(int tail=0;tail<2;tail++) {
  RcValue *args[]={rc_number(other,1.0),rc_retain(first)};
  assert(rc_builtin_call_slot(c,f,2,args)==-1);
  assert(!rc_primitive_call_owned(c,f,2,args,tail!=0)&&rc_has_error(c)&&!args[0]&&!args[1]);rc_clear_error(c);
  assert(!rc_primitive_call_owned(c,f,2,NULL,tail!=0)&&rc_has_error(c));rc_clear_error(c);
  args[0]=NULL;args[1]=rc_retain(first);
  assert(!rc_primitive_call_owned(c,f,2,args,tail!=0)&&rc_has_error(c)&&!args[1]);rc_clear_error(c);
  args[0]=rc_integer(c,20);args[1]=rc_integer(c,22);
  assert(!rc_primitive_call_owned(c,foreign,2,args,tail!=0)&&rc_has_error(c)&&!args[0]&&!args[1]);rc_clear_error(c);
 }
 assert(rc_refcount(first)==1);rc_release(first);rc_release(foreign);rc_release(f);
 rc_context_free(other);rc_context_free(c);
}
static void tags_roots_and_reuse(void) {
 RcContext *c=rc_context_new();assert(c&&rc_install_primitives(c));
 const char *names[]={"hd","tl","cons?","number?","string?","=","+","-","*","/",">","<",">=","<=","cons"};
 _Static_assert(sizeof(names)/sizeof(*names)==RC_BUILTIN_COUNT,
                "extend the generic tag cases for new canonical slots");
 RcValue *text=rc_string(c,"held"),*pair=rc_cons(c,text,rc_nil());
 for(int slot=0;slot<RC_BUILTIN_COUNT;slot++) {
  RcValue *f=function(c,names[slot]);size_t arity=slot>=RC_BUILTIN_EQ?2:1;
  RcValue *arg=slot<2?pair:slot==RC_BUILTIN_STRINGP?text:rc_integer(c,6);
  RcValue *args[]={rc_retain(arg),arity==2?rc_retain(arg):NULL};
  assert(rc_builtin_call_slot(c,f,arity,args)==slot);
  RcValue *out=rc_primitive_call_owned(c,f,arity,args,false);
  assert(out&&!args[0]&&(arity==1||!args[1])&&!rc_has_error(c));rc_release(out);
  args[0]=arg;args[1]=arity==2?arg:NULL;
  assert(rc_builtin_call_slot(c,f,arity+1,args)==-1);
  if(slot>=2&&slot!=RC_BUILTIN_STRINGP)rc_release(arg);
  rc_release(f);
 }
 RcValue *old=function(c,"hd");assert(rc_install_primitives(c));
 RcValue *args[]={rc_retain(pair)},*out;
 if(rc_refcount(old)==SIZE_MAX) {
  /* Immortal builtins (immediate opcode captures) are shared, so reinstalling
   * keeps the same canonical closure and no tagged header is ever freed.
   * Boxed builds keep mortal builtins and run the displacement path below. */
  RcValue *current=function(c,"hd");assert(current==old);rc_release(current);
  assert(rc_builtin_call_slot(c,old,1,args)==RC_BUILTIN_HD);
  out=rc_primitive_call_owned(c,old,1,args,false);assert(out==text&&!args[0]);rc_release(out);
  rc_release(old);
 } else {
 assert(rc_builtin_call_slot(c,old,1,args)==-1);
 out=rc_primitive_call_owned(c,old,1,args,false);assert(out==text&&!args[0]);rc_release(out);
 /* Releasing the displaced snapshot puts this header at the top of the LIFO
  * object cache. A different closure must not inherit its builtin identity. */
 RcValue *address=old;rc_release(old);
 RcValue *spares[16],*user=NULL;size_t used=0;
 /* Boxed builds also release the old opcode capture, which can occupy the
  * top cache entry. Pin intervening headers while finding the old closure. */
 while(used<16) {
  RcValue *next=rc_closure(c,1,user_identity,0,NULL);assert(next);
  if(next==address){user=next;break;}
  spares[used++]=next;
 }
 assert(user);
 args[0]=rc_retain(text);assert(rc_builtin_call_slot(c,user,1,args)==-1);
 out=rc_primitive_call_owned(c,user,1,args,false);assert(out==text&&!args[0]);rc_release(out);
 while(used)rc_release(spares[--used]);rc_release(user);
 /* Slab cells segregate pairs from other values, so the freed header is
  * reused by a non-closure value of the closure's size class. */
 RcValue *nonfunction=rc_number(c,0.5);assert(nonfunction==address&&rc_type(nonfunction)!=RC_CLOSURE);
 args[0]=rc_retain(text);assert(rc_builtin_call_slot(c,nonfunction,1,args)==-1);
 assert(!rc_primitive_call_owned(c,nonfunction,1,args,false)&&rc_has_error(c)&&!args[0]);rc_clear_error(c);
 rc_release(nonfunction);
 }
 RcValue *plus=function(c,"+"),*number=rc_integer(c,20),*part=rc_apply(c,plus,1,&number);
 assert(part&&rc_arity(part)==1);args[0]=rc_integer(c,22);
 assert(rc_builtin_call_slot(c,part,1,args)==-1);
 out=rc_primitive_call_owned(c,part,1,args,false);assert(out&&rc_integer_value(out)==42&&!args[0]);rc_release(out);
 rc_release(part);rc_release(number);rc_release(plus);rc_release(pair);rc_release(text);rc_context_free(c);
}
int main(void) {
 reject_arguments();tags_roots_and_reuse();
 puts("generic builtin tags: all15 slots, foreign/null, stale roots, user/partial and pool reuse PASS");
 return 0;
}
