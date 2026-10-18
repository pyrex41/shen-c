#include "eval.h"
#include "primitives.h"
#include "read.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static RcValue *run(RcContext *c,const char *source) {
 RcValue *form=rc_read(c,&source);if(!form||rc_has_error(c))abort();
 RcValue *out=rc_eval(c,form);rc_release(form);if(!out||rc_has_error(c))abort();return out;
}
/* Exact effect-free owned-native identity, matching the emitted identity body.
 * Its certificate constructor is the only conditional source between builds. */
static RcValue *second_owned(RcContext *c,RcValue *self,size_t argc,RcValue **args) {
 (void)c;(void)self;if(argc!=2)abort();RcValue *out=args[1];args[1]=NULL;return out;
}
static RcValue *third_owned(RcContext *c,RcValue *self,size_t argc,RcValue **args) {
 (void)c;(void)self;if(argc!=3)abort();RcValue *out=args[2];args[2]=NULL;return out;
}
int main(int argc,char **argv) {
 if(argc<2)return 2;
 size_t total=argc>2?(size_t)strtoull(argv[2],NULL,10):300000,depth=500;
 RcContext *c=rc_context_new();if(!c||!rc_install_primitives(c)||!rc_install_eval(c))abort();
 if(!strcmp(argv[1],"interpreted"))rc_release(run(c,"(defun do (A B) B)"));
 else if(!strcmp(argv[1],"owned")||!strcmp(argv[1],"partial")) {
  RcValue *fn;
  if(!strcmp(argv[1],"partial")) {
   RcValue *base=rc_closure_owned(c,3,third_owned,0,NULL),*arg=rc_nil();
   fn=rc_apply(c,base,1,&arg);rc_release(base);
  } else {
#if SHEN_RC_IDENTITY_TAIL_OPT
   fn=rc_closure_return_second_owned(c,second_owned,0,NULL);
#else
   fn=rc_closure_owned(c,2,second_owned,0,NULL);
#endif
  }
  RcValue *s=rc_symbol(c,"do");if(!fn||!s||!rc_function_set(c,s,fn))abort();rc_release(s);rc_release(fn);
 } else if(!strcmp(argv[1],"effectful"))rc_release(run(c,"(defun do (A B) (+ A B))"));
 int plain=!strcmp(argv[1],"plain"),ordinary=!strcmp(argv[1],"ordinary");
 int boolean=!strcmp(argv[1],"boolean"),boolean_do=!strcmp(argv[1],"boolean-do");
 rc_release(run(c,boolean?"(defun walk (N A) (and true (if (= N 0) true (walk (- N 1) A))))":
  boolean_do?"(defun walk (N A) (and true (do 0 (if (= N 0) true (walk (- N 1) A)))))":
  plain?"(defun walk (N A) (if (= N 0) A (walk (- N 1) A)))":
  ordinary?"(defun walk (N A) (if (= N 0) A (walk (- N 1) (do 0 A))))":
  "(defun walk (N A) (if (= N 0) A (do 0 (walk (- N 1) A))))"));
 char text[100];snprintf(text,sizeof(text),"(walk %zu 42)",depth);const char *source=text;
 RcValue *form=rc_read(c,&source);if(!form||rc_has_error(c))abort();RcStats before=rc_stats(c);
 for(size_t i=0;i<total/depth;i++) {
  RcValue *out=rc_eval(c,form);if(!out||rc_has_error(c)||((boolean||boolean_do)?!rc_is_true(out):rc_integer_value(out)!=42))abort();rc_release(out);
 }
 RcStats after=rc_stats(c);
 printf("%s %zu retains=%zu releases=%zu allocations=%zu peak=%zu\n",argv[1],total,
  after.retain_calls-before.retain_calls,after.release_calls-before.release_calls,
  after.total_allocations-before.total_allocations,after.peak_objects);
 rc_release(form);rc_context_free(c);return 0;
}
