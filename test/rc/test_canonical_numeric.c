#include "primitives.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef RcValue *(*Tiny)(RcContext *,RcValue *,RcValue **,bool);
static const char *names[]={"+","-","*","/",">","<",">=","<="};
static Tiny apis[]={rc_primitive_add_owned,rc_primitive_sub_owned,rc_primitive_mul_owned,
 rc_primitive_div_owned,rc_primitive_gt_owned,rc_primitive_lt_owned,rc_primitive_ge_owned,rc_primitive_le_owned};
static RcValue *function(RcContext *c,const char *name) {
 RcValue *s=rc_symbol(c,name),*f=rc_function_get(c,s);assert(f);rc_release(s);return f;
}
static RcValue *identity(RcContext *c,RcValue *s,size_t n,RcValue *const *a) {
 (void)c;(void)s;assert(n==2);return rc_retain(a[0]);
}
static RcValue *request_result(RcContext *c,RcValue *self,size_t n,RcValue *const *args) {
 (void)c;(void)args;assert(n==0);return rc_retain(rc_capture(self,0));
}
static void compare(RcContext *c,size_t op,RcValue *fn,RcValue *a,RcValue *b) {
 for(int tail=0;tail<2;tail++) {
  RcValue *borrowed[]={a,b},*expected=rc_apply(c,fn,2,borrowed);char error[160];
  snprintf(error,sizeof(error),"%s",rc_error_message(c));bool failed=rc_has_error(c);rc_clear_error(c);
  RcValue *owned[]={rc_retain(a),rc_retain(b)},*out=apis[op](c,fn,owned,tail!=0);
  assert(!owned[0]&&!owned[1]);
  /* A replacement selected in tail position must remain a stack-safe request.
   * Drive it through an ordinary caller before comparing final semantics. */
  if(tail&&rc_type(out)==RC_TAIL) {
   RcValue *wrapper=rc_closure(c,0,request_result,1,&out);assert(wrapper);
   RcValue *driven=rc_apply(c,wrapper,0,NULL);rc_release(out);rc_release(wrapper);out=driven;
  }
  assert(rc_has_error(c)==failed);
  if(failed){assert(!out&&!expected&&!strcmp(error,rc_error_message(c)));rc_clear_error(c);}
  else {
   assert(out&&expected&&rc_type(out)==rc_type(expected));
   if(rc_type(out)==RC_NUMBER) {
    double x=rc_number_value(out),y=rc_number_value(expected);
    assert((isnan(x)&&isnan(y))||(x==y&&(x!=0||signbit(x)==signbit(y))));
    assert(rc_number_is_exact_integer(out)==rc_number_is_exact_integer(expected));
    if(rc_number_is_exact_integer(out))assert(rc_integer_value(out)==rc_integer_value(expected));
   } else assert(out==expected);
  }
  rc_release(out);rc_release(expected);
 }
}
int main(void) {
 RcContext *c=rc_context_new(),*foreign=rc_context_new();
 assert(c&&foreign&&rc_install_primitives(c)&&rc_install_primitives(foreign));
 RcValue *values[]={rc_integer(c,7),rc_integer(c,2),rc_integer(c,0),rc_integer(c,-1),
 rc_integer(c,INT64_MAX),rc_integer(c,INT64_MIN),rc_number(c,2.0),rc_number(c,-0.0),
 rc_number(c,NAN),rc_number(c,INFINITY),rc_number(c,-INFINITY),rc_string(c,"bad")};
 for(size_t op=0;op<8;op++) {
  RcValue *f=function(c,names[op]);
  for(size_t a=0;a<12;a++)for(size_t b=0;b<12;b++)compare(c,op,f,values[a],values[b]);
  RcValue *args[]={rc_number(foreign,1.0),rc_retain(values[0])};
  assert(!apis[op](c,f,args,false)&&rc_has_error(c)&&!args[0]&&!args[1]);rc_clear_error(c);
  assert(!apis[op](c,f,NULL,false)&&rc_has_error(c));rc_clear_error(c);
  args[0]=rc_retain(values[0]);args[1]=NULL;
  assert(!apis[op](c,f,args,true)&&rc_has_error(c)&&!args[0]);rc_clear_error(c);
  RcValue *ff=function(foreign,names[op]);args[0]=rc_retain(values[0]);args[1]=rc_retain(values[1]);
  assert(!apis[op](c,ff,args,false)&&rc_has_error(c)&&!args[0]&&!args[1]);rc_clear_error(c);rc_release(ff);
  RcValue *replacement=rc_closure(c,2,identity,0,NULL);
  compare(c,op,replacement,values[0],values[1]);rc_release(replacement);
  /* A canonical function for another operation and a bound partial also
   * take ordinary dispatch rather than substituting the expected operation. */
  RcValue *cross=function(c,names[(op+1)%8]);compare(c,op,cross,values[0],values[1]);rc_release(cross);
  RcValue *partial=rc_apply(c,f,1,&values[0]);assert(partial&&rc_arity(partial)==1);
  args[0]=rc_retain(values[1]);args[1]=rc_nil();
  assert(!apis[op](c,partial,args,false)&&rc_has_error(c)&&!args[0]&&!args[1]);rc_clear_error(c);rc_release(partial);
  rc_error(c,"pending numeric error");args[0]=rc_retain(values[0]);args[1]=rc_retain(values[1]);
  assert(!apis[op](c,f,args,false)&&rc_has_error(c)&&!args[0]&&!args[1]);rc_clear_error(c);
  /* Reinstall replaces the bounded canonical root; an old owned snapshot
   * keeps its exact numeric semantics through ordinary fallback. */
  assert(rc_install_primitives(c));compare(c,op,f,values[0],values[1]);rc_release(f);
 }
 for(size_t i=0;i<12;i++)rc_release(values[i]);
 rc_context_free(foreign);rc_context_free(c);
 puts("canonical numeric primitives: 2304 differential cases, guards, fallbacks and error order PASS");
 return 0;
}
