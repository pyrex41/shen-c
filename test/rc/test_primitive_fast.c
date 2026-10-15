#include "primitives.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
int main(void) {
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
 return 0;
}
