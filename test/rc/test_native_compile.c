#define _POSIX_C_SOURCE 200809L
#include "eval.h"
#include "primitives.h"
#include "read.h"
#include "rc_internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static RcValue *run(RcContext *c,const char *s){
 RcValue *x=rc_read(c,&s);assert(x);RcValue *r=rc_eval(c,x);rc_release(x);
 if(!r)fprintf(stderr,"native test: %s\n",rc_error_message(c));assert(r&&!rc_has_error(c));return r;
}
static void number(RcContext *c,const char *s,int n){RcValue *r=run(c,s);assert(rc_integer_value(r)==n);rc_release(r);}
static void discard(RcContext *c,const char *s){rc_release(run(c,s));}
static void literals(RcContext *c,int value){
 RcValue *v=rc_vector(c,1),*n=rc_integer(c,value);assert(v&&n&&rc_vector_set(c,v,0,n));rc_release(n);
 /* Opaque literal is captured by two escaping lambda levels. */
 const char *s="(lambda X (lambda Y PLACEHOLDER))";RcValue *ast=rc_read(c,&s);
 RcValue *inner=rc_head_borrowed(rc_tail_borrowed(rc_tail_borrowed(ast)));
 RcValue *bodyargs=rc_tail_borrowed(rc_tail_borrowed(inner));
 /* Rebuild immutable pairs with the live vector in the body. */
 RcValue *body=rc_cons(c,v,rc_nil()),*arg=rc_cons(c,rc_head_borrowed(rc_tail_borrowed(inner)),body);rc_release(body);
 RcValue *lambda=rc_cons(c,rc_head_borrowed(inner),arg);rc_release(arg);
 body=rc_cons(c,lambda,rc_nil());rc_release(lambda);
 arg=rc_cons(c,rc_head_borrowed(rc_tail_borrowed(ast)),body);rc_release(body);
 RcValue *form=rc_cons(c,rc_head_borrowed(ast),arg);rc_release(arg);rc_release(ast);(void)bodyargs;
 RcValue *f=rc_eval(c,form);assert(f);rc_release(form);
 RcValue *zero=rc_integer(c,0),*g=rc_apply(c,f,1,&zero);assert(g);rc_release(f);
 RcValue *got=rc_apply(c,g,1,&zero);assert(got==v);rc_release(got);
 /* Close an exact mutable cycle through the native closure's literal roots. */
#ifndef SHEN_RC_ACYCLIC_ONLY
 assert(rc_vector_set(c,v,0,g));
#endif
 rc_release(g);rc_release(zero);rc_release(v);rc_collect(c);
}
int main(int argc,char **argv){
 setenv("SHEN_RC_EXECUTION","native",1);setenv("SHEN_RC_VERIFY_NATIVE","1",1);
 RcContext *c=rc_context_new();assert(c&&rc_install_primitives(c)&&rc_install_eval(c));
 if(argc==2){
  const char *source=argv[1];RcValue *form=rc_read(c,&source),*value=form?rc_eval(c,form):NULL;rc_release(form);
  if(value){rc_print(stdout,value);putchar('\n');rc_release(value);}else fprintf(stderr,"%s\n",rc_error_message(c));
  int status=value?0:1;assert(rc_interpreter_entries(c)==0);rc_context_free(c);return status;
 }
 number(c,"(+ 20 22)",42);number(c,"(+ 20 22)",42);
 discard(c,"(defun count-down (N A) (if (= N 0) A (count-down (- N 1) (+ A 1))))");
 number(c,"(count-down 100000 0)",100000);
 discard(c,"(defun outer () (defun inner () 7))");discard(c,"(outer)");number(c,"(inner)",7);
 number(c,"(((lambda X (lambda Y (+ X Y))) 20) 22)",42);
 discard(c,"(defun take-first (X Y) X)");
 number(c,"(take-first 7 (defun take-first (X Y) 99))",7);number(c,"(take-first 7 8)",99);
 number(c,"(trap-error (hd 4) (lambda E 42))",42);
 RcValue *q=run(c,"(quote (a b))");assert(rc_type(q)==RC_PAIR);rc_release(q);
 literals(c,1);literals(c,2);assert(rc_interpreter_entries(c)==0);rc_context_free(c);
 /* The same cached machine code binds a different context's literal roots. */
 c=rc_context_new();assert(c&&rc_install_primitives(c)&&rc_install_eval(c));literals(c,3);assert(rc_interpreter_entries(c)==0);rc_context_free(c);
 puts("RC compile-first expressions, redefinition, closures, literals and cycles: PASS");return 0;
}
