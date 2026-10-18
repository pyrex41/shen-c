#include "primitives.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <string.h>

static RcValue *function(RcContext *ctx,const char *name) {
 RcValue *symbol=rc_symbol(ctx,name),*fn=rc_function_get(ctx,symbol);
 rc_release(symbol);assert(fn);return fn;
}
static RcValue *replacement(RcContext *ctx,RcValue *self,size_t n,RcValue *const *v) {
 (void)self;assert(n==2);return rc_cons(ctx,v[1],v[0]);
}
static void consuming_cons(void) {
 RcContext *ctx=rc_context_new();assert(ctx&&rc_install_primitives(ctx));
 RcValue *fn=function(ctx,"cons"),*text=rc_string(ctx,"aliased");
 RcValue *args[]={rc_retain(text),rc_retain(text)};
 RcStats before=rc_stats(ctx);
 RcValue *out=rc_primitive_call_owned(ctx,fn,2,args,false);
 assert(out&&!args[0]&&!args[1]&&rc_head_borrowed(out)==text&&rc_tail_borrowed(out)==text);
 RcStats after=rc_stats(ctx);
 assert(after.retain_calls==before.retain_calls&&after.release_calls==before.release_calls);
 assert(rc_refcount(text)==3);rc_release(out);assert(rc_refcount(text)==1);
 /* Partial primitives must prepend bound arguments; same native callback and
  * opcode alone cannot identify a complete exact call. */
 RcValue *part=rc_apply(ctx,fn,1,&text);assert(part&&rc_arity(part)==1);
 RcValue *one[]={rc_retain(text)};
 out=rc_primitive_call_owned(ctx,part,1,one,false);
 assert(out&&!one[0]&&rc_head_borrowed(out)==text&&rc_tail_borrowed(out)==text);
 rc_release(out);rc_release(part);
 RcValue *changed=rc_closure(ctx,2,replacement,0,NULL);
 RcValue *head=rc_string(ctx,"head"),*tail=rc_string(ctx,"tail");
 args[0]=rc_retain(head);args[1]=rc_retain(tail);
 out=rc_primitive_call_owned(ctx,changed,2,args,false);
 assert(out&&!args[0]&&!args[1]&&rc_head_borrowed(out)==tail&&rc_tail_borrowed(out)==head);
 rc_release(out);rc_release(changed);rc_release(head);rc_release(tail);
 /* Constructor transfers vector reachability to the collector. */
 RcValue *v=rc_vector(ctx,1);
 out=rc_cons_owned(ctx,rc_retain(v),rc_nil());assert(out&&rc_vector_set(ctx,v,0,out));
 rc_release(out);rc_release(v);size_t cycles=rc_stats(ctx).cycle_objects_freed;
 rc_collect(ctx);assert(rc_stats(ctx).cycle_objects_freed==cycles+2);
 RcContext *other=rc_context_new();RcValue *foreign=rc_string(other,"foreign");
 assert(!rc_cons_owned(ctx,rc_retain(text),foreign)&&rc_has_error(ctx));
 assert(rc_refcount(text)==1&&rc_stats(other).live_objects==0);rc_clear_error(ctx);
 assert(!rc_cons_owned(ctx,rc_retain(text),NULL)&&rc_has_error(ctx));
 assert(rc_refcount(text)==1);rc_clear_error(ctx);
 rc_error(ctx,"pending error");args[0]=rc_retain(text);args[1]=rc_nil();
 assert(!rc_primitive_call_owned(ctx,fn,2,args,false)&&!args[0]&&!args[1]);
 assert(rc_refcount(text)==1);rc_clear_error(ctx);
 rc_release(fn);rc_release(text);rc_context_free(other);rc_context_free(ctx);
}
static void tail_projections(void) {
 RcContext *ctx=rc_context_new();assert(ctx&&rc_install_primitives(ctx));
 RcValue *plus=function(ctx,"+");
 const char *names[]={"do","hd","tl"};
 for(int which=0;which<3;which++)for(int tail=0;tail<2;tail++) {
  RcValue *fn=function(ctx,names[which]),*nums[]={rc_integer(ctx,20),rc_integer(ctx,22)};
  RcValue *request=rc_tailcall_owned_args(ctx,plus,2,nums),*args[2];assert(request);
  if(which==0){args[0]=rc_nil();args[1]=request;}
  else {args[0]=which==1?rc_cons(ctx,request,rc_nil()):rc_cons(ctx,rc_nil(),request);rc_release(request);}
  RcValue *out=rc_primitive_call_owned(ctx,fn,which==0?2:1,args,tail!=0);
  assert(out&&!args[0]&&(which!=0||!args[1])&&!rc_has_error(ctx));
  if(tail)assert(rc_type(out)==RC_TAIL);else assert(rc_integer_value(out)==42);
  rc_release(out);rc_release(fn);
 }
 RcValue *cons=function(ctx,"cons"),*extra[]={rc_nil(),rc_nil(),rc_nil()};
 assert(!rc_primitive_call_owned(ctx,cons,3,extra,false)&&rc_has_error(ctx));
 assert(!extra[0]&&!extra[1]&&!extra[2]);rc_clear_error(ctx);
 rc_release(cons);rc_release(plus);rc_context_free(ctx);
}
static void numeric_paths(void) {
 RcContext *ctx=rc_context_new();assert(ctx&&rc_install_primitives(ctx));
 const char *names[]={"+","-","*","/",">","<",">=","<="};
 for(size_t op=0;op<8;op++)for(int mixed=0;mixed<2;mixed++) {
  RcValue *fn=function(ctx,names[op]);
  RcValue *args[]={rc_integer(ctx,7),mixed?rc_number(ctx,2.0):rc_integer(ctx,2)};
  RcValue *out=rc_primitive_call_owned(ctx,fn,2,args,false);
  assert(out&&!args[0]&&!args[1]&&!rc_has_error(ctx));
  const double expected[]={9,5,14,3.5};
  if(op<4){assert(rc_number_value(out)==expected[op]);
   assert(rc_number_is_exact_integer(out)==(!mixed&&op!=3));}
  else assert(rc_is_true(out)==(op==4||op==6));
  rc_release(out);rc_release(fn);
 }
 const char *ops[]={"+","-","*","/","/","/"};
 const int64_t left[]={INT64_MAX,INT64_MIN,INT64_MAX,INT64_MIN,7,7};
 const int64_t right[]={1,1,2,-1,0,1};
 for(size_t i=0;i<6;i++) {
  RcValue *fn=function(ctx,ops[i]),*args[]={rc_integer(ctx,left[i]),rc_integer(ctx,right[i])};
  RcValue *out=rc_primitive_call_owned(ctx,fn,2,args,false);
  assert(!args[0]&&!args[1]);
  if(i<5){assert(!out&&rc_has_error(ctx));rc_clear_error(ctx);}
  else {assert(out&&rc_integer_value(out)==7&&rc_number_is_exact_integer(out));rc_release(out);}
  rc_release(fn);
 }
 RcValue *fn=function(ctx,"/"),*args[]={rc_integer(ctx,7),rc_number(ctx,-0.0)};
 assert(!rc_primitive_call_owned(ctx,fn,2,args,false)&&rc_has_error(ctx));rc_clear_error(ctx);
 args[0]=rc_string(ctx,"bad");args[1]=rc_integer(ctx,2);
 assert(!rc_primitive_call_owned(ctx,fn,2,args,false)&&rc_has_error(ctx));rc_clear_error(ctx);
 args[0]=rc_number(ctx,INFINITY);args[1]=rc_integer(ctx,2);
 RcValue *out=rc_primitive_call_owned(ctx,fn,2,args,false);assert(out&&isinf(rc_number_value(out)));rc_release(out);
 args[0]=rc_number(ctx,NAN);args[1]=rc_integer(ctx,2);
 out=rc_primitive_call_owned(ctx,fn,2,args,false);assert(out&&isnan(rc_number_value(out)));rc_release(out);
 rc_release(fn);rc_context_free(ctx);
}
int main(void) {
 consuming_cons();numeric_paths();tail_projections();
 RcContext *a=rc_context_new(),*b=rc_context_new();assert(a&&b);
 assert(rc_install_primitives(a)&&rc_install_primitives(b));
 RcValue *s=rc_symbol(a,"+"),*f=rc_function_get(a,s);rc_release(s);
 RcValue *args[]={rc_integer(a,20),rc_integer(a,22)};
 RcValue *v=rc_primitive_call_owned(a,f,2,args,true);
 assert(v&&rc_integer_value(v)==42&&!args[0]&&!args[1]);rc_release(v);
 args[0]=rc_number(b,20.0);args[1]=rc_integer(a,22);
 assert(!rc_primitive_call_owned(a,f,2,args,false));
 assert(rc_has_error(a)&&!args[0]&&!args[1]);rc_clear_error(a);
 assert(!rc_primitive_call_owned(a,f,2,NULL,true));assert(rc_has_error(a));rc_clear_error(a);
 args[0]=NULL;args[1]=rc_integer(a,22);
 assert(!rc_primitive_call_owned(a,f,2,args,true));
 assert(rc_has_error(a)&&!args[1]);rc_clear_error(a);
 s=rc_symbol(b,"+");RcValue *foreign=rc_function_get(b,s);rc_release(s);
 args[0]=rc_integer(a,20);args[1]=rc_integer(a,22);
 assert(!rc_primitive_call_owned(a,foreign,2,args,false));
 assert(rc_has_error(a)&&!args[0]&&!args[1]);rc_clear_error(a);
 for(int which=0;which<3;which++) {
  const char *names[]={"do","hd","tl"};
  s=rc_symbol(a,names[which]);RcValue *projection=rc_function_get(a,s);rc_release(s);
  RcValue *nums[]={rc_integer(a,20),rc_integer(a,22)};
  RcValue *request=rc_tailcall_owned_args(a,f,2,nums);
  request=rc_require_boolean(a,request);assert(request);
  size_t n=which==0?2:1;
  if(which==0){args[0]=rc_nil();args[1]=request;}
  else {args[0]=which==1?rc_cons(a,request,rc_nil()):rc_cons(a,rc_nil(),request);rc_release(request);}
  assert(!rc_primitive_call_owned(a,projection,n,args,false));
  assert(rc_has_error(a)&&!args[0]&&(n==1||!args[1]));rc_clear_error(a);
  rc_release(projection);
 }
 rc_release(foreign);rc_release(f);rc_context_free(a);rc_context_free(b);
 puts("RC primitive fusion: guards, consuming cons, arithmetic and tail results PASS");
 return 0;
}
