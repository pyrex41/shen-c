#include "rc.h"
#include "primitives.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static RcValue *replacement(RcContext *c,RcValue *self,size_t n,RcValue **args){
 assert(n==1);
 RcValue *key=rc_symbol(c,"cons?"),*next=rc_closure_owned(c,1,replacement,0,NULL);
 assert(key&&next&&rc_function_set(c,key,next));rc_release(next);rc_release(key);
 RcValue *arg=args[0];args[0]=NULL;rc_release(arg);rc_collect(c);
 /* The callback remains rooted even after replacing its only global owner. */
 assert(rc_vector_length(rc_capture(self,0))==1);
 return rc_true(c);
}
static RcValue *fail(RcContext *c,RcValue *self,size_t n,RcValue **args){
 (void)self;assert(n==1);rc_release(args[0]);args[0]=NULL;return rc_error(c,"predicate failure");
}
static RcValue *not_boolean(RcContext *c,RcValue *self,size_t n,RcValue **args){
 (void)self;(void)args;assert(n==1);return rc_integer(c,7);
}
int main(void){
 RcContext *c=rc_context_new(),*foreign=rc_context_new();assert(c&&foreign&&rc_install_primitives(c));
 RcValue *key=rc_symbol(c,"cons?"),*fn=rc_function_get(c,key),*one=rc_integer(c,1);
 RcValue *pair=rc_cons(c,one,rc_nil());rc_release(one);assert(key&&fn&&pair);
 RcValue *args[]={pair};size_t refs=rc_refcount(pair);RcStats before=rc_stats(c);
 assert(rc_condition_consp_borrowed(c,fn,args)==1&&args[0]==pair&&rc_refcount(pair)==refs);
 RcStats after=rc_stats(c);assert(after.retain_calls==before.retain_calls&&after.release_calls==before.release_calls);
 rc_release(fn);
 RcValue *capture=rc_vector(c,1);fn=rc_closure_owned(c,1,replacement,1,&capture);rc_release(capture);
 assert(fn&&rc_function_set(c,key,fn));rc_release(fn);fn=rc_function_get_borrowed(c,key);
 assert(rc_condition_consp_borrowed(c,fn,args)==1&&args[0]==pair&&rc_refcount(pair)==refs);
 fn=rc_closure_owned(c,1,fail,0,NULL);assert(fn);
 assert(rc_condition_consp_borrowed(c,fn,args)==-1&&rc_has_error(c)&&args[0]==pair&&rc_refcount(pair)==refs);
 rc_clear_error(c);rc_release(fn);
 fn=rc_closure_owned(c,1,not_boolean,0,NULL);assert(fn);
 assert(rc_condition_consp_borrowed(c,fn,args)==-2&&!rc_has_error(c)&&args[0]==pair&&rc_refcount(pair)==refs);
 RcValue *other=rc_vector(foreign,1);args[0]=other;
 assert(rc_condition_consp_borrowed(c,fn,args)==-1&&rc_has_error(c)&&args[0]==other);rc_clear_error(c);
 rc_release(other);rc_release(fn);rc_release(pair);rc_release(key);rc_context_free(foreign);rc_context_free(c);
 puts("RC predicate borrows: roots, snapshots, consuming replacements, collection, failures and foreign values PASS");return 0;
}
