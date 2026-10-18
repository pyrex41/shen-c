#include "rc.h"
#include "read.h"
#include "eval.h"
#include "primitives.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static RcValue *get(RcContext *c,const char *name) {
 RcValue *symbol=rc_symbol(c,name);assert(symbol);RcValue *fn=rc_function_get(c,symbol);
 rc_release(symbol);assert(fn);return fn;
}
static void bytes(RcValue *value,const char *expected,size_t n) {
 assert(value&&rc_type(value)==RC_STRING&&rc_text_length(value)==n);
 assert(!memcmp(rc_text(value),expected,n)&&rc_text(value)[n]==0);
}
static RcValue *evaluate(RcContext *c,const char *source) {
 RcValue *form=rc_read(c,&source);assert(form);RcValue *result=rc_eval(c,form);
 rc_release(form);return result;
}
int main(void) {
 RcContext *c=rc_context_new();assert(c&&rc_install_primitives(c)&&rc_install_eval(c));
 rc_set_collection_interval(c,1);RcValue *fn=get(c,"cn");
 const char left[]={'a',0,'b'},right[]={'c',0,'d'},joined[]={'a',0,'b','c',0,'d'};
 RcValue *a=rc_string_n(c,left,3),*b=rc_string_n(c,right,3);assert(a&&b);
 RcValue *args[]={a,b};RcValue *r=rc_apply(c,fn,2,args);bytes(r,joined,6);rc_release(r);
 bytes(a,left,3);bytes(b,right,3);assert(rc_refcount(a)==1&&rc_refcount(b)==1);
 RcValue *owned[]={rc_retain(a),rc_retain(b)};
 r=rc_apply_owned_args(c,fn,2,owned);assert(!owned[0]&&!owned[1]);bytes(r,joined,6);rc_release(r);
 RcValue *same[]={a,a};r=rc_apply(c,fn,2,same);bytes(r,"a\0ba\0b",6);rc_release(r);
 RcValue *empty=rc_string(c,"");assert(empty);
 RcValue *lhs[]={empty,a},*rhs[]={a,empty},*both[]={empty,empty};
 r=rc_apply(c,fn,2,lhs);bytes(r,left,3);rc_release(r);
 r=rc_apply(c,fn,2,rhs);bytes(r,left,3);rc_release(r);
 r=rc_apply(c,fn,2,both);bytes(r,"",0);rc_release(r);
 RcValue *partial=rc_apply(c,fn,1,&a);assert(partial);
 r=rc_apply(c,partial,1,&b);bytes(r,joined,6);rc_release(r);rc_release(partial);
 RcValue *bad[]={rc_integer(c,1),b};assert(!rc_apply(c,fn,2,bad));
 assert(!strcmp(rc_error_message(c),"cn requires strings"));rc_clear_error(c);
 RcContext *other=rc_context_new();assert(other);RcValue *foreign=rc_string(other,"foreign");
 RcValue *foreign_args[]={a,foreign};assert(!rc_apply(c,fn,2,foreign_args));assert(rc_has_error(c));
 bytes(foreign,"foreign",7);rc_clear_error(c);rc_release(foreign);rc_context_free(other);
 /* The callee must be snapshotted before argument effects. */
 r=evaluate(c,"(cn \"a\" (do (defun cn (X Y) \"changed\") \"b\"))");bytes(r,"ab",2);rc_release(r);
 r=evaluate(c,"(cn \"a\" \"b\")");bytes(r,"changed",7);rc_release(r);
 /* An earlier captured original function remains callable after redefinition. */
 r=rc_apply(c,fn,2,args);bytes(r,joined,6);rc_release(r);
 assert(rc_install_primitives(c));
 r=evaluate(c,"(cn 0 (simple-error \"inner\"))");assert(!r&&!strcmp(rc_error_message(c),"inner"));rc_clear_error(c);
 for(size_t n=0;n<128;n++) {
  char x[128],y[128],expected[256];
  for(size_t i=0;i<n;i++){x[i]=(char)(i*17);y[i]=(char)(255-i);}
  memcpy(expected,x,n);memcpy(expected+n,y,n);
  RcValue *v=rc_string_n(c,x,n),*w=rc_string_n(c,y,n);assert(v&&w);
  RcValue *pair[]={v,w};r=rc_apply(c,fn,2,pair);bytes(r,expected,2*n);
  rc_release(v);rc_release(w);rc_collect(c);bytes(r,expected,2*n);rc_release(r);
 }
 rc_release(empty);rc_release(a);rc_release(b);rc_release(fn);rc_collect(c);
 rc_context_free(c);puts("RC string copy: bytes, NUL, aliases, ownership, partial calls, redefinition and errors PASS");return 0;
}
