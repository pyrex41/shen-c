#include "eval.h"
#include "primitives.h"
#include "read.h"
#include "rc_internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
static RcValue *read_form(RcContext *c,const char *s){RcValue *v=rc_read(c,&s);assert(v&&!rc_has_error(c));return v;}
static RcValue *run(RcContext *c,const char *s){RcValue *e=read_form(c,s),*v=rc_eval(c,e);rc_release(e);if(!v)fprintf(stderr,"%s: %s\n",s,rc_error_message(c));assert(v&&!rc_has_error(c));return v;}
static void number(RcContext *c,const char *s,int n){RcValue *v=run(c,s);assert(rc_type(v)==RC_NUMBER&&rc_integer_value(v)==n);rc_release(v);}
static void define(RcContext *c,const char *s){
 RcValue *name=run(c,s),*fn=rc_function_get(c,name);assert(fn);
 RcValue *body=rc_capture(fn,0);
 /* Exercise the first prepared invocation even for effectful controls that
  * cannot be called repeatedly just to cross the preparation threshold. */
 if(rc_type(body)==RC_PAIR)for(size_t i=0;i<31;i++)rc_eval_plan_heat(body);
 rc_release(fn);rc_release(name);
}
static RcValue *at(RcValue *e,size_t n){while(n--)e=rc_tail_borrowed(e);return rc_head_borrowed(e);}
static RcValue *definition(RcContext *c,RcValue *name,RcValue *params,RcValue *body){
 RcValue *defun=rc_symbol(c,"defun"),*tail=rc_cons(c,body,rc_nil());
 tail=rc_cons_owned(c,rc_retain(params),tail);tail=rc_cons_owned(c,rc_retain(name),tail);
 return rc_cons_owned(c,defun,tail);
}
static void shared_body(RcContext *c){
 RcValue *body=read_form(c,"(- X Y)"),*xy=read_form(c,"(X Y)"),*yx=read_form(c,"(Y X)");
 RcValue *a=rc_symbol(c,"plan-a"),*b=rc_symbol(c,"plan-b"),*e=definition(c,a,xy,body);
 rc_release(rc_eval(c,e));rc_release(e);assert(!rc_has_error(c));
 RcValue *first=rc_function_get(c,a);for(size_t i=0;i<64;i++)number(c,"(plan-a 43 1)",42);
 RcValue *old=rc_eval_plan_get(body);assert(old&&rc_capture(first,0)==old);
 size_t refs=rc_refcount(body),allocs=rc_stats(c).total_allocations;
 e=definition(c,b,xy,body);rc_release(rc_eval(c,e));rc_release(e);
 /* Four syntax pairs plus one function, and no second plan. */
 assert(rc_stats(c).total_allocations==allocs+5&&rc_eval_plan_get(body)==old&&rc_refcount(body)==refs);
 e=definition(c,b,yx,body);rc_release(rc_eval(c,e));rc_release(e);
 for(size_t i=0;i<64;i++)number(c,"(plan-b 43 1)",-42);
 assert(rc_eval_plan_get(body)!=old&&rc_capture(first,0)==old&&rc_refcount(body)==refs);
 number(c,"(plan-a 43 1)",42);number(c,"(plan-b 43 1)",-42);
 rc_release(body);rc_release(xy);rc_release(yx);rc_release(first);rc_release(a);rc_release(b);
 /* A plan may outlive its complete source, including cache replacement. */
 rc_collect(c);number(c,"(plan-a 43 1)",42);number(c,"(plan-b 43 1)",-42);
}
static void semantics(RcContext *c){
 define(c,"(defun plan-locals (X Y) (let Z (+ X 1) (let X (+ Y 1) (- Z X))))");number(c,"(plan-locals 43 1)",42);
 define(c,"(defun plan-shadow (if) (if 42))");number(c,"(plan-shadow (lambda X X))",42);
 define(c,"(defun plan-let-shadow () (let if (lambda X X) (if 42)))");number(c,"(plan-let-shadow)",42);
 number(c,"(let if (lambda X X) ((freeze (if 42))))",42);
 number(c,"(let X 40 ((lambda Y (let Z (+ X Y) (+ Z 1))) 1))",42);
 number(c,"(let + (lambda X (lambda Y X)) ((lambda Z ((+ Z) 0)) 42))",42);
 define(c,"(defun plan-lazy (X) (if X 42 (if 1)))");number(c,"(plan-lazy true)",42);
 define(c,"(defun plan-lazy-cond () (cond (true 42) (false)))");number(c,"(plan-lazy-cond)",42);
 define(c,"(defun plan-quote (X) (quote (X)))");RcValue *v=run(c,"(plan-quote 42)");assert(rc_type(at(v,0))==RC_SYMBOL&&!strcmp(rc_text(at(v,0)),"X"));rc_release(v);
 define(c,"(defun plan-type (X) (type X (simple-error \"not evaluated\")))");number(c,"(plan-type 42)",42);
 define(c,"(defun plan-many (A B C D E F G H I J) (let K (+ A J) (+ K I)))");number(c,"(plan-many 20 0 0 0 0 0 0 0 1 21)",42);
 number(c,"(let F (plan-many 20 0) (F 0 0 0 0 0 0 1 21))",42);
 define(c,"(defun plan-over (A) (let X A (lambda B (+ X B))))");number(c,"(plan-over 20 22)",42);
 define(c,"(defun plan-error (X) (+ X (simple-error \"planned failure\")))");number(c,"(trap-error (plan-error 0) (lambda E (if (= (error-to-string E) \"planned failure\") 42 0)))",42);
 define(c,"(defun plan-pre-args () (missing-plan-function (set *plan-effect* 1)))");
 number(c,"(do (set *plan-effect* 0) (do (trap-error (plan-pre-args) (lambda E 42)) (value *plan-effect*)))",0);
 define(c,"(defun plan-callee (X) 42)");
 define(c,"(defun plan-snapshot () (let F plan-callee (F (do (defun plan-callee (X) 99) 0))))");number(c,"(plan-snapshot)",42);number(c,"(plan-callee 0)",99);
 define(c,"(defun plan-callee (X) 42)");
 define(c,"(defun plan-computed () ((if true plan-callee missing-plan) (do (defun plan-callee (X) 99) 0)))");number(c,"(plan-computed)",42);
 /* Leave this definition cold: preparation must happen inside an existing
  * non-tail recursive stack, whose older syntax borrows stay live. */
 rc_release(run(c,"(defun plan-rec (N) (if (= N 0) 0 (+ 1 (plan-rec (- N 1)))))"));number(c,"(plan-rec 100)",100);
 define(c,"(defun plan-escape (X) (let Y 2 (lambda Z (+ (+ X Y) Z))))");
 v=run(c,"(plan-escape 39)");rc_collect(c);RcValue *arg=rc_integer(c,1),*out=rc_apply(c,v,1,&arg);assert(out&&rc_integer_value(out)==42);rc_release(out);rc_release(arg);rc_release(v);
}
static void cache_reuse(RcContext *c){
 RcValue *body=read_form(c,"(+ X 1)"),*params=read_form(c,"(X)"),*name=rc_symbol(c,"plan-reuse");
 RcValue *e=definition(c,name,params,body);rc_release(rc_eval(c,e));rc_release(e);
 for(size_t i=0;i<64;i++)number(c,"(plan-reuse 41)",42);
 assert(rc_eval_plan_get(body)&&rc_refcount(body)==1);
 body=rc_cons_reuse(c,body,rc_integer(c,42),rc_nil());assert(body&&!rc_eval_plan_get(body));
 RcValue *reuse=NULL,*tail=rc_pair_take_tail_reuse(c,body,&reuse);assert(rc_is_nil(tail)&&reuse&&!rc_eval_plan_get(reuse));rc_release(tail);rc_release(reuse);
 /* Reset stale cache pointers when an object is returned to the pool. */
 for(size_t i=0;i<1000;i++){body=rc_cons(c,rc_nil(),rc_nil());assert(body&&!rc_eval_plan_get(body));rc_release(body);}
 rc_release(params);rc_release(name);rc_collect(c);number(c,"(plan-reuse 41)",42);
}
static void cyclic_literal(RcContext *c){
 /* A quoted descendant vector already reaches the AST. Caching must preserve
  * exact reclamation, including the extra pair->plan edge. */
 RcValue *body=read_form(c,"(quote ())"),*vector=rc_vector(c,1),*quote=rc_head_borrowed(body);
 RcValue *args=rc_cons(c,vector,rc_nil()),*root=rc_cons(c,quote,args);rc_release(args);rc_release(body);
 RcValue *name=rc_symbol(c,"plan-cycle"),*e=definition(c,name,rc_nil(),root);
 assert(rc_vector_set(c,vector,0,root));rc_release(rc_eval(c,e));rc_release(e);
 RcValue *fn=rc_function_get(c,name),*v;
 for(size_t i=0;i<64;i++){v=rc_apply(c,fn,0,NULL);assert(v==vector);rc_release(v);}
 assert(rc_eval_plan_get(root));rc_release(fn);
 /* Drop the definition too, leaving an entirely unreachable cycle. */
 define(c,"(defun plan-cycle () 0)");rc_release(root);rc_release(vector);rc_release(name);
 size_t before=rc_stats(c).cycle_objects_freed;rc_collect(c);assert(rc_stats(c).cycle_objects_freed>before);
}
static RcValue *external_argument;
static RcValue *drop_argument(RcContext *c,RcValue *self,size_t argc,RcValue *const *argv){
 (void)self;(void)argv;assert(!argc&&external_argument);rc_release(external_argument);external_argument=NULL;rc_collect(c);return rc_nil();
}
static void lazy_roots(RcContext *c){
 RcValue *name=rc_symbol(c,"plan-drop-argument"),*fn=rc_closure(c,0,drop_argument,0,NULL);
 assert(rc_function_set(c,name,fn));rc_release(fn);rc_release(name);
 define(c,"(defun plan-rooted (X) (do (plan-drop-argument) (<-address X 0)))");
 name=rc_symbol(c,"plan-rooted");fn=rc_function_get(c,name);RcValue *out;
 for(size_t i=0;i<64;i++){
  external_argument=rc_vector(c,1);RcValue *n=rc_integer(c,42);assert(rc_vector_set(c,external_argument,0,n));rc_release(n);
  RcValue *arg=external_argument;out=rc_apply(c,fn,1,&arg);
  assert(!external_argument&&out&&rc_integer_value(out)==42);rc_release(out);
 }
 rc_release(fn);rc_release(name);
 /* Optional preparation limits must preserve arbitrarily deep valid lets. */
 char source[8192];size_t used=(size_t)snprintf(source,sizeof(source),"(freeze ");
 for(size_t i=0;i<160;i++)used+=(size_t)snprintf(source+used,sizeof(source)-used,"(let X 0 ");
 used+=(size_t)snprintf(source+used,sizeof(source)-used,"42");
 for(size_t i=0;i<161;i++)used+=(size_t)snprintf(source+used,sizeof(source)-used,")");
 fn=run(c,source);assert(rc_type(rc_capture(fn,0))==RC_PAIR);
 out=rc_apply(c,fn,0,NULL);assert(out&&rc_integer_value(out)==42);rc_release(out);rc_release(fn);
}
static unsigned random_state=1729;
static unsigned next_random(void){random_state=random_state*1664525u+1013904223u;return random_state;}
static size_t random_expression(char *out,size_t capacity,unsigned depth){
 if(!depth)return (size_t)snprintf(out,capacity,next_random()%2 ? "X" : "%u",next_random()%11);
 const char *prefix,*middle,*suffix;
 switch(next_random()%5){
 case 0:prefix="(+ ";middle=" ";suffix=")";break;
 case 1:prefix="(- ";middle=" ";suffix=")";break;
 case 2:prefix="(let X ";middle=" ";suffix=")";break;
 case 3:prefix="(if (= X 0) ";middle=" ";suffix=")";break;
 default:prefix="(cond ((< X 3) ";middle=") (true ";suffix="))";break;
 }
 size_t n=(size_t)snprintf(out,capacity,"%s",prefix);
 n+=random_expression(out+n,capacity-n,depth-1);n+=(size_t)snprintf(out+n,capacity-n,"%s",middle);
 n+=random_expression(out+n,capacity-n,depth-1);n+=(size_t)snprintf(out+n,capacity-n,"%s",suffix);return n;
}
static void differential(RcContext *c){
 /* An unused lowercase outer binding forces the original evaluator while
  * leaving these expressions unchanged. This is an independent execution
  * oracle for generated combinations of shadowing, branches and arithmetic. */
 for(size_t i=0;i<400;i++){
  char body[8192],source[9000];random_expression(body,sizeof(body),4);
  snprintf(source,sizeof(source),"(lambda X %s)",body);RcValue *prepared=run(c,source);
  snprintf(source,sizeof(source),"(let unused.plan 0 (lambda X %s))",body);RcValue *generic=run(c,source);
  assert(rc_type(rc_capture(generic,0))==RC_PAIR);
  RcValue *arg=rc_integer(c,(int)(next_random()%17)-8),*a;
  for(size_t j=0;j<64;j++){a=rc_apply(c,prepared,1,&arg);assert(a);rc_release(a);}
  assert(rc_type(rc_capture(prepared,0))==RC_CLOSURE);
  a=rc_apply(c,prepared,1,&arg);RcValue *b=rc_apply(c,generic,1,&arg);
  assert(a&&b&&!rc_has_error(c)&&rc_equal(a,b));rc_release(a);rc_release(b);rc_release(arg);rc_release(prepared);rc_release(generic);
 }
}
int main(void){
 RcContext *c=rc_context_new();assert(c&&rc_install_primitives(c)&&rc_install_eval(c));
 shared_body(c);semantics(c);cache_reuse(c);cyclic_literal(c);lazy_roots(c);differential(c);
 rc_context_free(c);puts("RC prepared evaluator: lexical coordinates, snapshots, lazy fallbacks, cache lifetime, reuse and cycles PASS");return 0;
}
