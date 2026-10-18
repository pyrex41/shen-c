#include "rc_internal.h"
#include "eval.h"
#include "read.h"
#include "primitives.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
#include <stdio.h>
static void length(RcValue*x,size_t expected){size_t n=999;assert(rc_list_length_cached(x,&n)==RC_LIST_LENGTH_OK&&n==expected);assert(rc_list_length_cached(x,&n)==RC_LIST_LENGTH_OK&&n==expected);}
static void improper(RcValue*x){size_t n=999;assert(rc_list_length_cached(x,&n)==RC_LIST_LENGTH_IMPROPER&&n==999);assert(rc_list_length_cached(x,&n)==RC_LIST_LENGTH_IMPROPER&&n==999);}
int main(void){RcContext*c=rc_context_new();assert(c);assert(rc_install_primitives(c));length(rc_nil(),0);improper(rc_integer(c,42));
RcValue *one=rc_integer(c,1),*tail=rc_cons(c,one,rc_nil()),*head=rc_cons(c,one,tail);length(tail,1);length(head,2);
/* Warm ancestor and shared tail: a consumed tail owner cannot mutate it. */
RcValue*old=tail;tail=rc_cons_reuse(c,tail,one,rc_nil());assert(tail!=old);length(head,2);length(old,1);length(tail,1);
rc_release(tail);tail=rc_retain(old);RcValue*token=NULL,*projected=rc_pair_take_tail_reuse(c,tail,&token);length(head,2);length(token,1);length(projected,0);rc_release(token);rc_release(projected);
/* Sole-owned pair reset by both reuse mutations, including cached improper. */
RcValue*x=rc_cons(c,one,rc_nil());length(x,1);x=rc_cons_reuse(c,x,one,head);length(x,3);projected=rc_pair_take_tail_reuse(c,x,&token);length(projected,2);length(token,1);rc_release(projected);x=rc_cons_reuse(c,token,one,one);improper(x);x=rc_cons_reuse(c,x,one,rc_nil());length(x,1);rc_release(x);
RcValue*deep=rc_retain(one);for(size_t i=0;i<1000;i++){RcValue*n=rc_cons_owned(c,rc_retain(one),deep);deep=n;}improper(deep);rc_release(deep);
deep=rc_nil();for(size_t i=0;i<10000;i++)deep=rc_cons_owned(c,rc_retain(one),deep);length(deep,10000);rc_release(deep);
/* Recycled pair storage and cross-type cached blocks must not inherit metadata. */
for(size_t i=0;i<100;i++){x=rc_cons(c,one,rc_nil());length(x,1);rc_release(x);x=rc_string(c,"pooled");rc_release(x);x=rc_cons_owned(c,rc_retain(one),rc_retain(head));length(x,3);rc_release(x);}
const char*s="(+ 1 . 2)"; /* Construct improper application explicitly: reader syntax is proper-only. */
(void)s;RcValue*plus=rc_symbol(c,"+"),*args=rc_cons(c,one,one),*form=rc_cons(c,plus,args);for(size_t i=0;i<100;i++){assert(!rc_eval(c,form));assert(!strcmp(rc_error_message(c),"eval-kl: improper expression list"));rc_clear_error(c);}rc_release(form);rc_release(args);rc_release(plus);
rc_release(head);rc_release(one);rc_collect(c);rc_context_free(c);puts("RC list length cache: warmed aliases, both reuse paths, pooled cross-type, deep improper, long/nil, repeated malformed eval PASS");return 0;}
