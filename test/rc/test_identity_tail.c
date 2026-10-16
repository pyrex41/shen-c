#define _POSIX_C_SOURCE 200809L
#include "eval.h"
#include "primitives.h"
#include "read.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static RcValue *run(RcContext *c,const char *source) {
 const char *original=source;RcValue *form=rc_read(c,&source);
 if(!form||rc_has_error(c))fprintf(stderr,"parse failure: %s: %s\n",original,rc_error_message(c));
 assert(form&&!rc_has_error(c));
 assert(!rc_read(c,&source)&&!rc_has_error(c));
 RcValue *out=rc_eval(c,form);rc_release(form);
 if(!out)fprintf(stderr,"%s\n",rc_error_message(c));
 assert(out&&!rc_has_error(c));return out;
}
static void number(RcContext *c,const char *source,int expected) {
 RcValue *out=run(c,source);assert(rc_integer_value(out)==expected);rc_release(out);
}
static void definition(RcContext *c,const char *source) {rc_release(run(c,source));}
static RcValue *second(RcContext *c,RcValue *self,size_t n,RcValue *const *args) {
 (void)c;(void)self;assert(n==2);return rc_retain(args[1]);
}
static RcValue *second_owned(RcContext *c,RcValue *self,size_t n,RcValue **args) {
 (void)c;(void)self;assert(n==2);RcValue *out=args[1];args[1]=NULL;return out;
}
static RcValue *binding(RcContext *c,const char *text) {
 RcValue *s=rc_symbol(c,text),*fn=rc_function_get(c,s);rc_release(s);assert(fn);return fn;
}
static int stream_fd=-1;
static RcValue *make_stream(RcContext *c,RcValue *self,size_t n,RcValue *const *args) {
 (void)self;(void)args;assert(!n);FILE *f=tmpfile();assert(f);stream_fd=fileno(f);return rc_stream(c,f,true);
}
static RcValue *check_stream(RcContext *c,RcValue *self,size_t n,RcValue *const *args) {
 (void)self;assert(n==1&&rc_type(args[0])==RC_STREAM);
 rc_collect(c);FILE *f=rc_stream_file(args[0]);assert(f&&fcntl(fileno(f),F_GETFD)!=-1);
 assert(fputs("still owned",f)>=0);return rc_integer(c,42);
}
static RcValue *collect(RcContext *c,RcValue *self,size_t n,RcValue *const *args) {
 (void)self;(void)args;assert(!n);rc_collect(c);return rc_integer(c,42);
}
static RcValue *check_vector(RcContext *c,RcValue *self,size_t n,RcValue *const *args) {
 (void)self;assert(n==1&&rc_type(args[0])==RC_VECTOR);
 assert(rc_vector_set(c,args[0],0,args[0]));rc_collect(c);
 assert(rc_vector_length(args[0])==1);return rc_integer(c,42);
}
static RcValue *foreign_value;
static size_t recorded_effects;
static RcValue *foreign_result(RcContext *c,RcValue *self,size_t n,RcValue *const *args) {
 (void)c;(void)self;(void)args;assert(!n);return rc_retain(foreign_value);
}
static RcValue *record_effect(RcContext *c,RcValue *self,size_t n,RcValue *const *args) {
 (void)self;(void)args;assert(!n);recorded_effects++;return rc_integer(c,42);
}
static RcValue *identity_native(RcContext *c,RcValue *self,size_t n,RcValue *const *args) {
 (void)c;(void)self;assert(n==1);return rc_retain(args[0]);
}
static void fails_exactly(RcContext *c,const char *source,const char *message) {
 RcValue *form=rc_read(c,&source);assert(form);RcValue *out=rc_eval(c,form);rc_release(form);
 assert(!out&&rc_has_error(c)&&!strcmp(rc_error_message(c),message));rc_clear_error(c);
}
static size_t tail_callbacks;
static RcValue *tail_boolean(RcContext *c,RcValue *self,size_t n,RcValue *const *args) {
 (void)args;assert(!n);tail_callbacks++;
 return rc_integer_value(rc_capture(self,0)) ? rc_true(c) : rc_integer(c,7);
}
static void install(RcContext *c,const char *name,size_t arity,RcNative native) {
 RcValue *s=rc_symbol(c,name),*f=rc_closure(c,arity,native,0,NULL);
 assert(s&&f&&rc_function_set(c,s,f));rc_release(s);rc_release(f);
}
int main(void) {
 RcContext *c=rc_context_new(),*foreign=rc_context_new();assert(c&&foreign&&rc_install_primitives(c)&&rc_install_eval(c));
 RcValue *f=rc_closure_return_second(c,second,0,NULL),*g=rc_closure_return_second_owned(c,second_owned,0,NULL);
 assert(f&&g&&rc_closure_returns_second(c,f)&&rc_closure_returns_second(c,g));
 assert(!rc_closure_returns_second(foreign,f)&&!rc_closure_returns_second(NULL,f));
 assert(!rc_closure_returns_second(c,NULL)&&!rc_closure_returns_second(c,rc_nil()));
 RcValue *arg=rc_integer(c,1),*partial=rc_apply(c,f,1,&arg);assert(partial&&!rc_closure_returns_second(c,partial));
 RcValue *args[]={arg,rc_integer(c,42)},*out=rc_apply(c,g,2,args);assert(out&&rc_integer_value(out)==42);rc_release(out);
 out=rc_apply(c,partial,1,args+1);assert(out&&rc_integer_value(out)==42);rc_release(out);
 RcValue *zero=rc_apply(c,f,0,NULL);assert(zero&&!rc_closure_returns_second(c,zero));rc_release(zero);
 rc_release(partial);rc_release(f);rc_release(g);rc_release(args[0]);rc_release(args[1]);
 for(size_t i=0;i<1000;i++) {
  f=rc_closure_return_second(c,second,0,NULL);assert(f&&rc_closure_returns_second(c,f));rc_release(f);
  f=rc_closure(c,2,second,0,NULL);assert(f&&!rc_closure_returns_second(c,f));rc_release(f);
 }
 f=binding(c,"do");assert(rc_closure_returns_second(c,f));rc_release(f);
 definition(c,"(defun identity (X) X)");definition(c,"(defun pick2 (A B) B)");
 f=binding(c,"pick2");assert(rc_closure_returns_second(c,f));rc_release(f);
 definition(c,"(defun pick1 (A B) A)");f=binding(c,"pick1");assert(!rc_closure_returns_second(c,f));rc_release(f);
 definition(c,"(defun wrapped (A B) (type B T))");f=binding(c,"wrapped");assert(!rc_closure_returns_second(c,f));rc_release(f);
 definition(c,"(defun three (A B C) C)");f=binding(c,"three");assert(!rc_closure_returns_second(c,f));
 arg=rc_integer(c,1);partial=rc_apply(c,f,1,&arg);assert(partial&&rc_arity(partial)==2&&!rc_closure_returns_second(c,partial));rc_release(partial);rc_release(arg);rc_release(f);
 definition(c,"(defun snap (X) (pick2 (do (defun pick2 (A B) A) (shen.gc)) (identity X)))");
 number(c,"(snap 42)",42);number(c,"(pick2 1 42)",1);
 definition(c,"(defun pick2 (A B) B)");
 definition(c,"(defun alias (X) (let F pick2 (F (do (defun pick2 (A B) A) (shen.gc)) (identity X))))");number(c,"(alias 42)",42);
 definition(c,"(defun pick2 (A B) B)");
 definition(c,"(defun computed (X) ((if true pick2 missing) (do (defun pick2 (A B) A) (shen.gc)) (identity X)))");number(c,"(computed 42)",42);
 definition(c,"(defun pick2 (A B) B)");number(c,"(let F (pick2 1) (F 42))",42);
 definition(c,"(defun effectful (A B) (+ A B))");
 definition(c,"(defun old-effect (X) (effectful (do (defun effectful (A B) B) 1) (identity X)))");number(c,"(old-effect 41)",42);
 definition(c,"(defun down-do (N) (if (= N 0) 42 (do 0 (down-do (- N 1)))))");number(c,"(down-do 100000)",42);
 definition(c,"(defun bool-do (N) (and true (do 0 (if (= N 0) true (bool-do (- N 1))))))");out=run(c,"(bool-do 30000)");assert(rc_is_true(out));rc_release(out);
 definition(c,"(defun bad-do (N) (and true (do 0 (if (= N 0) 7 (bad-do (- N 1))))))");number(c,"(trap-error (bad-do 30000) (lambda E 42))",42);
 definition(c,"(defun fails-do (X) (do 0 (simple-error \"deferred error\")))");number(c,"(trap-error (fails-do 0) (lambda E 42))",42);
 number(c,"(trap-error (do (simple-error \"first error\") (simple-error \"unreached\")) (lambda E 42))",42);
 install(c,"make-owned-stream",0,make_stream);install(c,"check-owned-stream",1,check_stream);install(c,"collect-now",0,collect);install(c,"check-owned-vector",1,check_vector);
 definition(c,"(defun stream-shared () (let S (make-owned-stream) (do S (check-owned-stream S))))");number(c,"(stream-shared)",42);assert(fcntl(stream_fd,F_GETFD)==-1&&errno==EBADF);
 definition(c,"(defun stream-capture () (let S (make-owned-stream) (let F (freeze (check-owned-stream S)) (do S (F)))))");number(c,"(stream-capture)",42);assert(fcntl(stream_fd,F_GETFD)==-1&&errno==EBADF);
 definition(c,"(defun stream-dead () (do (make-owned-stream) (collect-now)))");number(c,"(stream-dead)",42);assert(fcntl(stream_fd,F_GETFD)==-1&&errno==EBADF);
 definition(c,"(defun vector-owned () (let V (absvector 1) (do V (check-owned-vector V))))");number(c,"(vector-owned)",42);rc_collect(c);
 /* A shared managed tail value in a lexical slot must remain reusable, keep
  * its boolean continuation, and execute only when the result is forwarded. */
 definition(c,"(defun forward-tail-data (X) (do 0 X))");
 definition(c,"(defun ignore-tail-data (X) (do X 42))");
 RcValue *forward=binding(c,"forward-tail-data"),*ignore=binding(c,"ignore-tail-data");
 for(int valid=0;valid<2;valid++) {
  RcValue *marker=rc_integer(c,valid),*callback=rc_closure(c,0,tail_boolean,1,&marker);rc_release(marker);assert(callback);
  RcValue *request=rc_require_boolean(c,rc_tailcall(c,callback,0,NULL));rc_release(callback);assert(request);
  size_t before=tail_callbacks;
  out=rc_apply(c,ignore,1,&request);assert(out&&rc_integer_value(out)==42&&tail_callbacks==before);rc_release(out);
  for(size_t repeat=0;repeat<2;repeat++) {
   out=rc_apply(c,forward,1,&request);
   if(valid){assert(out&&rc_is_true(out));rc_release(out);}
   else{assert(!out&&rc_has_error(c)&&strstr(rc_error_message(c),"boolean"));rc_clear_error(c);}
  }
  assert(tail_callbacks==before+2);rc_release(request);
 }
 rc_release(forward);rc_release(ignore);
 /* Reinstall replaces the current do binding; old snapshots/certificates stay
  * immutable and partial closures retain their normal prepend semantics. */
 definition(c,"(defun do (A B) B)");definition(c,"(defun dynamic-down (N) (if (= N 0) 42 (do 0 (dynamic-down (- N 1)))))");number(c,"(dynamic-down 100000)",42);
 assert(rc_install_primitives(c));number(c,"(dynamic-down 100000)",42);
 /* Preserve do's operand-context validation even when its second expression
  * becomes a deferred request, and keep error ordering across continuations. */
 foreign_value=rc_string(foreign,"foreign result");assert(foreign_value);
 install(c,"foreign-out",0,foreign_result);install(c,"record-effect",0,record_effect);
 definition(c,"(defun foreign-right () (do 0 (foreign-out)))");
 definition(c,"(defun foreign-left () (do (foreign-out) (record-effect)))");
 definition(c,"(defun foreign-inner-bool () (do 0 (and true (foreign-out))))");
 definition(c,"(defun foreign-outer-bool () (and true (do 0 (foreign-out))))");
 definition(c,"(defun foreign-right-error () (do (foreign-out) (simple-error \"right wins\")))");
 fails_exactly(c,"(foreign-right)","invalid owned argument/context");
 size_t effects_before=recorded_effects;fails_exactly(c,"(foreign-left)","invalid owned argument/context");assert(recorded_effects==effects_before+1);
 fails_exactly(c,"(foreign-inner-bool)","boolean expected");fails_exactly(c,"(foreign-outer-bool)","invalid owned argument/context");
 fails_exactly(c,"(foreign-right-error)","right wins");
 fails_exactly(c,"((lambda X (do 0 (foreign-out))) 0 1)","invalid owned argument/context");
 definition(c,"(defun foreign-inner-overapply () (do 0 ((lambda X foreign-out) 0 1)))");
 fails_exactly(c,"(foreign-inner-overapply)","attempt to apply a non-function");
 /* Adding a deferred check must copy a shared request rather than change the
  * reusable C-owned request's original, unguarded completion behavior. */
 RcValue *foreign_fn=binding(c,"foreign-out"),*shared=rc_tailcall(c,foreign_fn,0,NULL);rc_release(foreign_fn);assert(shared);
 forward=binding(c,"forward-tail-data");out=rc_apply(c,forward,1,&shared);assert(!out&&rc_has_error(c)&&!strcmp(rc_error_message(c),"invalid owned argument/context"));rc_clear_error(c);rc_release(forward);
 RcValue *id=rc_closure(c,1,identity_native,0,NULL);assert(id);out=rc_apply(c,id,1,&shared);assert(out==foreign_value);rc_release(out);rc_release(id);rc_release(shared);
 rc_release(foreign_value);foreign_value=NULL;
 rc_context_free(foreign);rc_collect(c);rc_context_free(c);puts("RC identity tail forwarding: PASS");return 0;
}
