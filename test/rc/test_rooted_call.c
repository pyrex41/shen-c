#include "rc.h"
#include "primitives.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
static RcValue *outside;
static RcValue *owned(RcContext *c,RcValue *self,size_t n,RcValue **args){
 assert(n==2);
 rc_release(outside);outside=NULL;rc_collect(c);
 assert(!strcmp(rc_text(rc_capture(self,0)),"active callee"));
 RcValue *result=args[1];args[1]=NULL;return result;
}
static RcValue *borrowed(RcContext *c,RcValue *self,size_t n,RcValue *const *args){
 (void)self;assert(n==2);rc_collect(c);return rc_retain(args[1]);
}
static RcValue *many(RcContext *c,RcValue *self,size_t n,RcValue **args){
 (void)c;(void)self;assert(n==17);RcValue *out=args[16];args[16]=NULL;return out;
}
static RcValue *fail(RcContext *c,RcValue *self,size_t n,RcValue **args){
 (void)self;assert(n==2);rc_release(args[0]);args[0]=NULL;return rc_error(c,"rooted failure");
}
static RcValue *over(RcContext *c,RcValue *self,size_t n,RcValue **args){
 (void)self;assert(n==1);return rc_closure(c,2,borrowed,1,args);
}
int main(void){
 RcContext *c=rc_context_new(),*foreign=rc_context_new();assert(c&&foreign);
 RcValue *capture=rc_string(c,"active callee"),*fn=rc_closure_owned(c,2,owned,1,&capture);rc_release(capture);
 RcValue *args[]={rc_string(c,"discard"),rc_string(c,"kept")};outside=rc_retain(fn);
 RcValue *out=rc_apply_rooted_owned_args(c,fn,2,args);assert(out&&!outside&&!strcmp(rc_text(out),"kept")&&!args[0]&&!args[1]);rc_release(out);rc_release(fn);
 fn=rc_closure(c,2,borrowed,0,NULL);args[0]=rc_string(c,"discard");args[1]=rc_string(c,"borrowed kept");
 out=rc_apply_rooted_owned_args(c,fn,2,args);assert(out&&!strcmp(rc_text(out),"borrowed kept")&&!args[0]&&!args[1]);rc_release(out);
 args[0]=rc_string(c,"bound");RcValue *part=rc_apply_rooted_owned_args(c,fn,1,args);assert(part&&!args[0]);
 args[0]=rc_integer(c,42);out=rc_apply_rooted_owned_args(c,part,1,args);assert(out&&rc_integer_value(out)==42&&!args[0]);rc_release(out);rc_release(part);rc_release(fn);
 fn=rc_closure_owned(c,1,over,0,NULL);RcValue *extra[]={rc_string(c,"captured"),rc_integer(c,0),rc_integer(c,42)};
 out=rc_apply_rooted_owned_args(c,fn,3,extra);assert(out&&rc_integer_value(out)==42&&!extra[0]&&!extra[1]&&!extra[2]);rc_release(out);rc_release(fn);
 fn=rc_closure_owned(c,17,many,0,NULL);RcValue *large[17];for(size_t i=0;i<17;i++)large[i]=rc_string(c,"argument");
 out=rc_apply_rooted_owned_args(c,fn,17,large);assert(out&&!strcmp(rc_text(out),"argument"));for(size_t i=0;i<17;i++)assert(!large[i]);rc_release(out);rc_release(fn);
 fn=rc_closure_owned(c,2,fail,0,NULL);args[0]=rc_string(c,"first");args[1]=rc_string(c,"second");
 assert(!rc_apply_rooted_owned_args(c,fn,2,args)&&rc_has_error(c)&&!strcmp(rc_error_message(c),"rooted failure")&&!args[0]&&!args[1]);rc_clear_error(c);
 args[0]=rc_string(foreign,"foreign");args[1]=rc_string(c,"same context");
 assert(!rc_apply_rooted_owned_args(c,fn,2,args)&&rc_has_error(c)&&!args[0]&&!args[1]);rc_clear_error(c);rc_release(fn);
 assert(!rc_stats(foreign).live_objects);rc_context_free(foreign);rc_context_free(c);
 puts("RC private rooted calls: active callee, borrowed/owned/large callbacks, partial/overapply, failure and foreign cleanup PASS");return 0;
}
