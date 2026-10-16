#include "primitives.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
static RcValue *get(RcContext *c,const char *name) {
 RcValue *s=rc_symbol(c,name),*f=rc_function_get(c,s);rc_release(s);assert(f);return f;
}
static RcValue *first(RcContext *c,RcValue *self,size_t n,RcValue *const *a) {
 (void)c;(void)self;assert(n==2);return rc_retain(a[0]);
}
static void construct(RcContext *c,RcValue *fn,RcValue *a,RcValue *b) {
 for(int tail=0;tail<2;tail++) {
  RcValue *args[]={rc_retain(a),rc_retain(b)};
  RcValue *out=rc_primitive_cons_owned(c,fn,args,tail!=0);
  assert(out&&!args[0]&&!args[1]&&rc_type(out)==RC_PAIR);
  assert(rc_head_borrowed(out)==a&&rc_tail_borrowed(out)==b);rc_release(out);
 }
}
static void invalid_compare(RcContext *c,RcValue *fn,RcValue *head,RcValue *tail) {
 RcValue *a[]={rc_retain(head),rc_retain(tail)};
 assert(!rc_primitive_call_owned(c,fn,2,a,false)&&!a[0]&&!a[1]&&rc_has_error(c));
 char message[160];snprintf(message,sizeof(message),"%s",rc_error_message(c));rc_clear_error(c);
 a[0]=rc_retain(head);a[1]=rc_retain(tail);
 assert(!rc_primitive_cons_owned(c,fn,a,false)&&!a[0]&&!a[1]&&rc_has_error(c));
 assert(!strcmp(message,rc_error_message(c)));rc_clear_error(c);
}
static void reuse(RcContext *c,RcValue *cons,RcValue *tl,RcValue *head) {
 RcValue *p=rc_cons(c,rc_nil(),rc_nil());assert(p);
 RcStats before=rc_stats(c);
 RcValue *out=rc_cons_call_reuse(c,cons,p,head,rc_nil(),false);
 assert(out==p&&rc_head_borrowed(p)==head&&rc_stats(c).pair_reuses==before.pair_reuses+1);
 RcValue *saved=NULL,*tail=rc_tail_call_reuse(c,tl,p,&saved);
 assert(tail==rc_nil()&&saved==p&&rc_head_borrowed(p)==rc_nil());rc_release(tail);
 RcValue *shared=rc_retain(saved);before=rc_stats(c);
 out=rc_cons_call_reuse(c,cons,saved,head,rc_nil(),true);
 assert(out&&out!=shared&&rc_stats(c).pair_reuse_misses==before.pair_reuse_misses+1);
 rc_release(out);rc_release(shared);
 /* Passing the candidate as a child must preserve the old graph. */
 p=rc_cons(c,head,rc_nil());out=rc_cons_call_reuse(c,cons,p,p,rc_nil(),false);
 assert(out&&out!=p&&rc_head_borrowed(out)==p);rc_release(out);
}
int main(void) {
 RcContext *c=rc_context_new(),*other=rc_context_new();assert(c&&other&&rc_install_primitives(c)&&rc_install_primitives(other));
 RcValue *cons=get(c,"cons"),*tl=get(c,"tl"),*head=rc_string(c,"head");assert(head);
 construct(c,cons,head,head);reuse(c,cons,tl,head);
 RcValue *foreign_root=rc_string(other,"foreign root");assert(foreign_root);
 invalid_compare(c,cons,NULL,head);invalid_compare(c,cons,head,NULL);
 invalid_compare(c,cons,foreign_root,head);invalid_compare(c,cons,head,foreign_root);
 rc_release(foreign_root);
 assert(!rc_primitive_cons_owned(c,cons,NULL,false)&&rc_has_error(c));rc_clear_error(c);
 RcValue *args[]={rc_retain(head),NULL};assert(!rc_primitive_cons_owned(c,cons,args,false)&&!args[0]&&!args[1]&&rc_has_error(c));rc_clear_error(c);
 RcValue *foreign=rc_string(other,"foreign");args[0]=foreign;args[1]=rc_retain(head);
 assert(!rc_primitive_cons_owned(c,cons,args,false)&&!args[0]&&!args[1]&&rc_has_error(c));rc_clear_error(c);
 RcValue *foreignfn=get(other,"cons");invalid_compare(c,foreignfn,head,rc_nil());args[0]=rc_retain(head);args[1]=rc_nil();
 assert(!rc_primitive_cons_owned(c,foreignfn,args,false)&&!args[0]&&!args[1]&&rc_has_error(c));rc_clear_error(c);
 RcValue *p=rc_cons(other,rc_nil(),rc_nil());
 assert(!rc_cons_call_reuse(c,cons,p,head,rc_nil(),false)&&rc_has_error(c));rc_clear_error(c);
 p=rc_cons(c,head,rc_nil());RcValue *out=(RcValue *)1;
 assert(!rc_tail_call_reuse(c,tl,p,NULL)&&rc_has_error(c));rc_clear_error(c);
 p=rc_cons(other,rc_nil(),rc_nil());assert(!rc_tail_call_reuse(c,tl,p,&out)&&!out&&rc_has_error(c));rc_clear_error(c);
 /* Canonical misses preserve old snapshot reuse and pending-error behavior. */
 assert(rc_install_primitives(c));construct(c,cons,head,rc_nil());reuse(c,cons,tl,head);
 rc_error(c,"pending");p=rc_cons(c,rc_nil(),rc_nil());out=rc_cons_call_reuse(c,cons,p,head,rc_nil(),false);
 assert(out==p&&rc_has_error(c));rc_release(out);rc_clear_error(c);
 args[0]=rc_retain(head);args[1]=rc_nil();rc_error(c,"pending");
 assert(!rc_primitive_cons_owned(c,cons,args,false)&&!args[0]&&!args[1]&&rc_has_error(c));rc_clear_error(c);
 RcValue *part=rc_apply(c,cons,1,&head);assert(part);
 args[0]=rc_nil();args[1]=rc_nil();assert(!rc_primitive_cons_owned(c,part,args,false)&&!args[0]&&!args[1]&&rc_has_error(c));rc_clear_error(c);rc_release(part);
 RcValue *changed=rc_closure(c,2,first,0,NULL);assert(changed);
 args[0]=rc_retain(head);args[1]=rc_nil();out=rc_primitive_cons_owned(c,changed,args,false);
 assert(out==head&&!args[0]&&!args[1]);rc_release(out);
 p=rc_cons(c,rc_nil(),rc_nil());out=rc_cons_call_reuse(c,changed,p,head,rc_nil(),false);assert(out==head);rc_release(out);
 RcValue *plus=get(c,"+"),*nums[]={rc_integer(c,20),rc_integer(c,22)};
 RcValue *request=rc_tailcall_owned_args(c,plus,2,nums);assert(request);
 construct(c,cons,request,rc_nil());rc_release(request);rc_release(plus);
 RcValue *v=rc_vector(c,1);args[0]=rc_retain(v);args[1]=rc_nil();
 out=rc_primitive_cons_owned(c,cons,args,false);assert(out&&rc_vector_set(c,v,0,out));
 rc_release(v);rc_release(out);size_t cycles=rc_stats(c).cycle_objects_freed;rc_collect(c);assert(rc_stats(c).cycle_objects_freed==cycles+2);
 rc_release(changed);rc_release(foreignfn);rc_release(head);rc_release(cons);rc_release(tl);rc_context_free(c);rc_context_free(other);
 puts("canonical cons: ownership, reuse, snapshots, foreign values and errors PASS");
}
