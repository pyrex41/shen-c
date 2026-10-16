#include "primitives.h"
#include "eval.h"
#include "read.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
static RcValue *replacement(RcContext *c,RcValue *self,size_t n,RcValue **args) {
 (void)self;(void)n;(void)args;return rc_integer(c,7);
}
static RcValue *self_replacing(RcContext *c,RcValue *self,size_t n,RcValue **args) {
 (void)args;assert(n==1);
 RcValue *symbol=rc_symbol(c,"hd"),*next=rc_closure_owned(c,1,replacement,0,NULL);
 assert(symbol&&next&&rc_function_set(c,symbol,next));rc_release(symbol);rc_release(next);
 rc_collect(c); /* Old binding is gone; dispatcher must still root self. */
 return rc_retain(rc_capture(self,0));
}
static void interpreted_snapshots(void) {
 RcContext *c=rc_context_new();assert(c&&rc_install_primitives(c)&&rc_install_eval(c));
 const char *cases[]={
  "(do (defun picked (X) 42) (picked (do (defun picked (X) 99) (shen.gc))))",
  "(do (defun picked (X) 42) (let F picked (F (do (defun picked (X) 99) (shen.gc)))))",
  "(do (defun picked (X) 42) ((if true picked picked) (do (defun picked (X) 99) (shen.gc))))",
  "(do (defun picked (X) (do (defun picked (X) 99) (do (shen.gc) 42))) (picked 0))",
  "(let F (lambda X (do (shen.gc) X)) (F 42))",
  "(trap-error (missing-callee (simple-error \"argument ran\")) (lambda E (if (= (error-to-string E) \"undefined function missing-callee\") 42 0)))",
  "(do (defun picked (X) X) (trap-error (picked (simple-error \"argument failed\")) (lambda E (do (shen.gc) 42))))"
 };
 for(size_t i=0;i<sizeof(cases)/sizeof(*cases);i++) {
  const char *source=cases[i];RcValue *form=rc_read(c,&source);assert(form);
  RcValue *value=rc_eval(c,form);assert(value&&!rc_has_error(c)&&rc_integer_value(value)==42);
  rc_release(value);rc_release(form);rc_collect(c);
 }
 rc_context_free(c);
}
int main(void) {
 RcContext *c=rc_context_new();assert(c&&rc_install_primitives(c));
 RcValue *symbol=rc_symbol(c,"hd"),*owned=rc_function_get(c,symbol);assert(owned);
 RcStats before=rc_stats(c);RcValue *borrowed=rc_function_get_borrowed(c,symbol);
 assert(borrowed==owned&&rc_stats(c).retain_calls==before.retain_calls);rc_release(owned);
 RcValue *marker=rc_string(c,"surviving self capture"),*fn=rc_closure_owned(c,1,self_replacing,1,&marker);
 assert(marker&&fn&&rc_function_set(c,symbol,fn));rc_release(marker);rc_release(fn);
 borrowed=rc_function_get_borrowed(c,symbol);assert(borrowed&&rc_refcount(borrowed)==1);
 RcValue *args[]={rc_nil()},*result=rc_primitive_call_owned(c,borrowed,1,args,false);
 assert(result&&!args[0]&&rc_type(result)==RC_STRING);rc_release(result);
 borrowed=rc_function_get_borrowed(c,symbol);args[0]=rc_nil();
 result=rc_primitive_call_owned(c,borrowed,1,args,false);assert(rc_integer_value(result)==7);rc_release(result);
 RcValue *missing=rc_symbol(c,"undefined-callee");assert(missing);
 assert(!rc_function_get_borrowed(c,missing)&&rc_has_error(c));rc_clear_error(c);
 assert(!rc_function_get_borrowed(c,rc_nil())&&rc_has_error(c));rc_clear_error(c);
 assert(!rc_function_get_borrowed(NULL,symbol));
 RcContext *other=rc_context_new();assert(other);
 assert(!rc_function_get_borrowed(other,symbol)&&rc_has_error(other));rc_clear_error(other);
 rc_release(missing);rc_release(symbol);rc_context_free(c);rc_context_free(other);
 interpreted_snapshots();
 puts("RC borrowed callees: PASS");
}
