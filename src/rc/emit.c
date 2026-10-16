#include "emit.h"
#include "read.h"
#include <inttypes.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Scoped owned slots, borrowed inputs, and owned return values. Every
 * expression result has one owner. Consumed temporaries drop immediately;
 * the common exit drops everything still live, including error paths.
 * Optimized mode computes lexical liveness, narrows captures, and transfers
 * owned arguments. The conservative option retains the original baseline. */
typedef struct Program Program;
typedef struct { Program *owner; char *p; size_t n, cap; } Buf;
typedef struct { const char *name; int slot; } Binding;
typedef struct Allocation { struct Allocation *prev,*next; max_align_t align; unsigned char data[]; } Allocation;
struct Program { RcEmitOptions options; RcContext *ctx; jmp_buf failure; Allocation *allocations; Buf functions,literals; const char **symbols; size_t nsymbols,symbol_capacity; bool user_phase,drop_range_used; RcValue *metadata_root; int next,next_literal; long defuns, lambdas, tops; };
static _Noreturn void bad(Program *p,const char *message) { rc_error(p->ctx,"%s",message); longjmp(p->failure,1); }
static void *resize(Program *p,void *old,size_t bytes) {
  if(bytes>SIZE_MAX-sizeof(Allocation)) bad(p,"compiler allocation overflow");
  Allocation *block=old ? (Allocation *)((unsigned char *)old - offsetof(Allocation,data)) : NULL;
  Allocation *prev=block ? block->prev : NULL,*next=block ? block->next : p->allocations;
  Allocation *grown=realloc(block,sizeof(*grown)+bytes);
  if(!grown) bad(p,"compiler out of memory");
  grown->prev=prev; grown->next=next;
  if(prev) prev->next=grown; else p->allocations=grown;
  if(next) next->prev=grown;
  return grown->data;
}
static void *alloc(Program *p,size_t bytes) { return resize(p,NULL,bytes); }
/* All compiler scratch belongs to one emission arena, including temporary
 * buffers abandoned by a syntax/OOM error. Reclamation happens at API exit. */
static void discard(void *unused) { (void)unused; }
static void clear_arena(Program *p) {
  while(p->allocations) { Allocation *next=p->allocations->next; free(p->allocations); p->allocations=next; }
}
static int is_symbol(const RcValue *x) { return rc_type(x)==RC_SYMBOL; }
static int is_list(const RcValue *x) { return rc_is_nil(x)||rc_type(x)==RC_PAIR; }
static int is_pair(const RcValue *x) { return rc_type(x)==RC_PAIR; }
static RcValue *first(RcValue *x) { return rc_head_borrowed(x); }
static RcValue *rest(RcValue *x) { return rc_tail_borrowed(x); }
static RcValue *second(RcValue *x) { return first(rest(x)); }
struct Recycle;
typedef struct {
  Program *p; Buf code; Binding *bind; int nb, cap, slots, label, max_args; bool symbol_used;
  int arity, boolean_tail; bool self_loop;
  int *physical,*free_slots,slot_cap,frame_slots,nfree; unsigned char *active;
  char fail[64]; struct Recycle *recycle;
} RcEmitFn;
static void put(Buf *b, const char *fmt, ...) {
  Program *p=b->owner;
  va_list ap; int n;
  va_start(ap,fmt); n=vsnprintf(NULL,0,fmt,ap); va_end(ap);
  if(n<0) bad(p,"rc emitter formatting failure");
  if(b->n+(size_t)n+1>b->cap) {
    b->cap=(b->n+(size_t)n+1)*2+256; b->p=resize(p,b->p,b->cap);
    if(!b->p) bad(p,"rc emitter allocation failure");
  }
  va_start(ap,fmt); vsnprintf(b->p+b->n,b->cap-b->n,fmt,ap); va_end(ap); b->n+=(size_t)n;
}
static void quoted_n(Buf *b,const char *s,size_t length) {
  put(b,"\"");
  for(size_t i=0;i<length;i++) { unsigned char c=(unsigned char)s[i];
    if(c=='"'||c=='\\') put(b,"\\%c",c);
    else if(c<32||c>=127) put(b,"\\%03o",c);
    else put(b,"%c",c);
  }
  put(b,"\"");
}
static void quoted(Buf *b,const char *s) { quoted_n(b,s,strlen(s)); }
static const char *name(RcValue *x) { return rc_text(x); }
static int named(RcValue *x,const char *s) { return is_symbol(x)&&!strcmp(name(x),s); }
static int binding(RcEmitFn *f,const char *s) { for(int i=f->nb-1;i>=0;i--) if(!strcmp(f->bind[i].name,s)) return f->bind[i].slot; return -1; }
static void bind(RcEmitFn *f,const char *s,int slot) {
  Program *p=f->p;
  if(f->nb==f->cap) { f->cap=f->cap?f->cap*2:32; f->bind=resize(p,f->bind,(size_t)f->cap*sizeof(*f->bind)); if(!f->bind) bad(p,"rc emitter binding allocation"); }
  f->bind[f->nb++]=(Binding){s,slot};
}
static int fresh(RcEmitFn *f) {
  int slot=f->slots++;
  if(f->slots>f->slot_cap) {
    f->slot_cap=f->slot_cap?f->slot_cap*2:32;
    f->physical=resize(f->p,f->physical,(size_t)f->slot_cap*sizeof(*f->physical));
    f->free_slots=resize(f->p,f->free_slots,(size_t)f->slot_cap*sizeof(*f->free_slots));
    f->active=resize(f->p,f->active,(size_t)f->slot_cap*sizeof(*f->active));
  }
  f->physical[slot]=f->nfree?f->free_slots[--f->nfree]:f->frame_slots++;
  f->active[slot]=1;return slot;
}
/* Logical slots remain unique for bindings, liveness, and trap boundaries.
 * Only private expression temporaries become available for frame reuse. In
 * particular, an ancestor binding stays pinned even after a last-use drop:
 * later branch paths and drop_dead may still refer to that logical slot. */
static void retire_scope(RcEmitFn *f,int mark,int result) {
  if(f->p->options.conservative_ownership)return;
  for(int i=mark;i<f->slots;i++)if(i!=result&&f->active[i]) {
    f->active[i]=0;f->free_slots[f->nfree++]=f->physical[i];
  }
}
/* Rewrite only C identifiers, leaving source strings (including escaped
 * quotes and text such as "t[123]") byte-for-byte intact. */
static void frame_code(Buf *out,const RcEmitFn *f) {
  const char *s=f->code.p?f->code.p:"";size_t start=0,i=0;
  while(s[i]) {
    if(s[i]=='\"'||s[i]=='\'') {
      char quote=s[i++];while(s[i]){if(s[i]=='\\'&&s[i+1])i+=2;else if(s[i++]==quote)break;}
    }else if(s[i]=='t'&&s[i+1]=='['&&(i==0||!((s[i-1]>='a'&&s[i-1]<='z')||(s[i-1]>='A'&&s[i-1]<='Z')||(s[i-1]>='0'&&s[i-1]<='9')||s[i-1]=='_'))) {
      size_t end=i+2;int slot=0;while(s[end]>='0'&&s[end]<='9'){slot=slot*10+s[end]-'0';end++;}
      if(end>i+2&&s[end]==']'&&slot<f->slots) {
        put(out,"%.*st[%d]",(int)(i-start),s+start,f->physical[slot]);i=end+1;start=i;
      }else i++;
    }else i++;
  }
  put(out,"%s",s+start);
}
static void drop(RcEmitFn *f,int x) { put(&f->code,"  if(t[%d]) rc_release(t[%d]); t[%d]=NULL;\n",x,x,x); }
static void check(RcEmitFn *f,int x) { put(&f->code,"  if(!t[%d]) goto %s;\n",x,f->fail); }
static void move(RcEmitFn *f,int dst,int src) { put(&f->code,"  t[%d]=t[%d]; t[%d]=NULL;\n",dst,src,src); }
/* The runtime borrows this scratch storage only for the duration of a call;
 * tail requests and closures retain/copy values before returning. Reusing it
 * avoids one function-lifetime compound array for every generated callsite. */
static void argument(RcEmitFn *f,int index,int slot) {
  if(index+1>f->max_args) f->max_args=index+1;
  put(&f->code,"  a[%d]=t[%d];\n",index,slot);
}
typedef struct { int *slot; size_t n,cap; } Live;
typedef struct Shadow { const char *name; const struct Shadow *next; } Shadow;
typedef struct Recycle { int binding,token; struct Recycle *outer; } Recycle;
static bool optimize(const RcEmitFn *f) { return !f->p->options.conservative_ownership; }
static bool has(const Live *s,int slot) { if(s)for(size_t i=0;i<s->n;i++)if(s->slot[i]==slot)return true;return false; }
static void add(Program *p,Live *s,int slot) {
 if(slot<0||has(s,slot))return;
 if(s->n==s->cap){s->cap=s->cap?s->cap*2:8;s->slot=resize(p,s->slot,s->cap*sizeof(*s->slot));}
 s->slot[s->n++]=slot;
}
static void merge(Program *p,Live *dst,const Live *src) { if(src)for(size_t i=0;i<src->n;i++)add(p,dst,src->slot[i]); }
static Live copy_live(Program *p,const Live *src) { Live s={0};merge(p,&s,src);return s; }
static void forget(Program *p,void *data) {
 if(!data)return;
 Allocation *a=(Allocation *)((unsigned char *)data - offsetof(Allocation,data));
 if(a->prev)a->prev->next=a->next;else p->allocations=a->next;
 if(a->next)a->next->prev=a->prev;free(a);
}
static void clear_live(Program *p,Live *s){forget(p,s->slot);*s=(Live){0};}
static bool shadowed(const Shadow *s,const char *n){for(;s;s=s->next)if(!strcmp(s->name,n))return true;return false;}
static int count(Program *p,RcValue *xs) {
 int n=0;while(!rc_is_nil(xs)){if(!is_pair(xs))bad(p,"rc emit: improper argument list");n++;xs=rest(xs);}return n;
}
static RcValue *at(Program *p,RcValue *xs,int n) {
 while(n--&&is_pair(xs))xs=rest(xs);
 if(!is_pair(xs))bad(p,"rc emit: missing special form argument");return first(xs);
}
/* Find the current function's free lexical slots. New binders are shadows;
 * annotations/defun bodies are not evaluated in the enclosing lexical scope. */
static void scan(RcEmitFn *f,RcValue *x,const Shadow *scope,Live *out) {
 again:
 if(!f->nb)return;
 if(is_symbol(x)){if(!shadowed(scope,name(x)))add(f->p,out,binding(f,name(x)));return;}
 if(!is_pair(x))return;
 RcValue *h=first(x),*args=rest(x);
 bool special=is_symbol(h)&&binding(f,name(h))<0&&!shadowed(scope,name(h));
 if(special&&named(h,"defun"))return;
 if(special&&named(h,"type")){scan(f,at(f->p,args,0),scope,out);return;}
 if(special&&named(h,"let")&&count(f->p,args)==3&&is_symbol(first(args))){
  scan(f,second(args),scope,out);Shadow local={name(first(args)),scope};scan(f,at(f->p,args,2),&local,out);return;
 }
 if(special&&named(h,"lambda")&&count(f->p,args)==2&&is_symbol(first(args))){
  Shadow local={name(first(args)),scope};scan(f,second(args),&local,out);return;
 }
 /* Flatten ordinary do spines here as in emission, to bound scanner stack. */
 if(special&&named(h,"do")&&count(f->p,args)==2){
  scan(f,first(args),scope,out);x=second(args);goto again;
 }
 scan(f,h,scope,out);
 for(RcValue *p=args;!rc_is_nil(p);p=rest(p)){if(!is_pair(p))bad(f->p,"rc emit: improper expression");scan(f,first(p),scope,out);}
}
static Live uses(RcEmitFn *f,RcValue *x){Live s={0};if(optimize(f))scan(f,x,NULL,&s);return s;}
static Live sequence_live(RcEmitFn *f,RcValue *expressions,const Live *after){
 Live s=copy_live(f->p,after);
 if(optimize(f))for(RcValue *p=expressions;!rc_is_nil(p);p=rest(p)){if(!is_pair(p))bad(f->p,"rc emit: improper sequence");scan(f,first(p),NULL,&s);}return s;
}
static void dead_range(RcEmitFn *f,int first,int end){
 if(end==first+1){drop(f,first);return;}
 f->p->drop_range_used=true;
 /* Logical neighbors need not be physical neighbors after scope reuse. */
 int begin=f->physical[first],limit=begin+1;
 for(int i=first+1;i<end;i++) {
  if(f->physical[i]==limit){limit++;continue;}
  put(&f->code,"  rc_drop_range(t,%d,%d);\n",begin,limit);
  begin=f->physical[i];limit=begin+1;
 }
 put(&f->code,"  rc_drop_range(t,%d,%d);\n",begin,limit);
}
static void drop_dead(RcEmitFn *f,const Live *live){
 if(!optimize(f))return;
 int first=-1,end=-1;
 for(int i=0;i<f->nb;i++){
  int slot=f->bind[i].slot;
  if(has(live,slot)){if(first>=0)dead_range(f,first,end);first=end=-1;continue;}
  if(first<0){first=slot;end=slot+1;}
  else if(slot==end)end++;
  else{dead_range(f,first,end);first=slot;end=slot+1;}
 }
 if(first>=0)dead_range(f,first,end);
}
static const char *closure_api(Program *p){return p->options.conservative_ownership?"rc_closure":"rc_closure_owned";}
static const char *call_api(RcEmitFn *f,int tail){return optimize(f)?(tail?"rc_tailcall_owned_args":"rc_apply_owned_args"):(tail?"rc_tailcall":"rc_apply");}
static void call_argument(RcEmitFn *f,int index,int slot){argument(f,index,slot);if(optimize(f))put(&f->code,"  t[%d]=NULL;\n",slot);}
static bool leaf_primitive(RcValue *head) {
 static const char *names[]={"hd","tl","cons?","cons","=","number?","string?",
  "symbol?","boolean?","integer?","variable?","absvector?","not",
  "+","-","*","/",">","<",">=","<="};
 for(size_t i=0;i<sizeof(names)/sizeof(*names);i++)if(named(head,names[i]))return true;
 return false;
}
static int expr(RcEmitFn *,RcValue *,int,const Live *);
static int compile(Program *,RcValue *,RcValue *,Binding *,int,int);
static int literal(Program *,RcValue *);
/* Assign one dense index per distinct static symbol in this emitted module. */
static size_t symbol_index(RcEmitFn *f,const char *name) {
 Program *p=f->p;f->symbol_used=true;
 for(size_t i=0;i<p->nsymbols;i++)if(!strcmp(p->symbols[i],name))return i;
 if(p->nsymbols==p->symbol_capacity){
  size_t cap=p->symbol_capacity?p->symbol_capacity*2:64;
  if(cap<p->symbol_capacity||cap>SIZE_MAX/sizeof(*p->symbols))bad(p,"compiled symbol table overflow");
  p->symbols=resize(p,p->symbols,cap*sizeof(*p->symbols));p->symbol_capacity=cap;
 }
 return p->symbols[p->nsymbols]=name,p->nsymbols++;
}
static void symbol_slot(RcEmitFn *f,int slot,const char *name) {
 size_t index=symbol_index(f,name);
 put(&f->code,"  t[%d]=rc_retain(symbols[%zu]);\n",slot,index);
}
static void symbol_frame(Buf *out,bool used) {
 if(used)put(out,"  RcValue *const *symbols=rc_symbol_table(ctx,&rc_module_symbols);\n  if(!symbols) return NULL;\n");
}
static int atom(RcEmitFn *f,RcValue *x,int tail,const Live *after) {
 Program *p=f->p;int t=fresh(f);
 if(rc_is_nil(x))put(&f->code,"  t[%d]=rc_nil();\n",t);
 else if(rc_type(x)==RC_NUMBER&&rc_number_is_exact_integer(x)){
  int64_t n=rc_integer_value(x);if(n==INT64_MIN)put(&f->code,"  t[%d]=rc_integer(ctx,INT64_MIN);\n",t);
  else put(&f->code,"  t[%d]=rc_integer(ctx,INT64_C(%" PRId64 "));\n",t,n);
 }else if(rc_type(x)==RC_NUMBER){double n=rc_number_value(x);
  if(isnan(n))put(&f->code,"  t[%d]=rc_number(ctx,NAN);\n",t);
  else if(isinf(n))put(&f->code,"  t[%d]=rc_number(ctx,%sINFINITY);\n",t,n<0?"-":"");
  else put(&f->code,"  t[%d]=rc_number(ctx,%a);\n",t,n);
 }else if(rc_type(x)==RC_STRING){put(&f->code,"  t[%d]=rc_string_n(ctx,",t);quoted_n(&f->code,rc_text(x),rc_text_length(x));put(&f->code,",%zu);\n",rc_text_length(x));}
 else if(is_symbol(x)){
  int b=binding(f,name(x));if(b>=0){if(optimize(f)?!has(after,b):tail)move(f,t,b);else put(&f->code,"  t[%d]=rc_retain(t[%d]);\n",t,b);}
  else symbol_slot(f,t,name(x));
 }else bad(p,"rc emit: unsupported atom");check(f,t);return t;
}
static void truth(RcEmitFn *f,int t){put(&f->code,"  if(!rc_is_boolean(t[%d])) { rc_error(ctx,\"condition is not a boolean\"); goto %s; }\n",t,f->fail);}
static int branch(RcEmitFn *f,RcValue *args,int tail,const Live *after){
 Program *p=f->p;if(count(p,args)!=3)bad(p,"rc emit: if arity");
 Live left=uses(f,at(p,args,1)),right=uses(f,at(p,args,2));merge(p,&left,after);merge(p,&right,after);
 Live tests=copy_live(p,&left);merge(p,&tests,&right);
 int dst=fresh(f),test=expr(f,first(args),0,&tests);clear_live(p,&tests);truth(f,test);
 put(&f->code,"  if(rc_is_true(t[%d])) {\n",test);drop(f,test);drop_dead(f,&left);
 int a=expr(f,at(p,args,1),tail,after);move(f,dst,a);
 put(&f->code,"  } else {\n");drop(f,test);drop_dead(f,&right);
 int b=expr(f,at(p,args,2),tail,after);move(f,dst,b);put(&f->code,"  }\n");
 clear_live(p,&left);clear_live(p,&right);return dst;
}
static int cond(RcEmitFn *f,RcValue *clauses,int tail,const Live *after){
 Program *p=f->p;int dst=fresh(f),done=f->label++;
 for(RcValue *q=clauses;!rc_is_nil(q);q=rest(q)){
  if(!is_pair(q))bad(p,"rc emit: cond clauses");RcValue *cl=first(q);if(count(p,cl)!=2)bad(p,"rc emit: cond clause");
  Live needed=sequence_live(f,rest(q),after),value=uses(f,second(cl));merge(p,&needed,&value);
  int test=expr(f,first(cl),0,&needed);clear_live(p,&needed);truth(f,test);
  put(&f->code,"  if(rc_is_true(t[%d])) {\n",test);drop(f,test);merge(p,&value,after);drop_dead(f,&value);clear_live(p,&value);
  int val=expr(f,second(cl),tail,after);move(f,dst,val);put(&f->code,"  goto cond_done_%d;\n  }\n",done);drop(f,test);
  Live remaining=sequence_live(f,rest(q),after);drop_dead(f,&remaining);clear_live(p,&remaining);
 }
 put(&f->code,"  rc_error(ctx,\"cond has no true branch\"); goto %s;\ncond_done_%d:;\n",f->fail,done);return dst;
}
static int closure(RcEmitFn *f,RcValue *params,RcValue *body,int isfreeze,const Live *after){
 Program *p=f->p;int t=fresh(f),n=0;Binding *caps=alloc(p,(size_t)(f->nb+1)*sizeof(*caps));Live freevars={0};
 if(optimize(f)){Shadow param={isfreeze?"":name(params),NULL};scan(f,body,isfreeze?NULL:&param,&freevars);}
 for(int i=0;i<f->nb;i++)if(binding(f,f->bind[i].name)==f->bind[i].slot&&(!optimize(f)||has(&freevars,f->bind[i].slot)))caps[n++]=f->bind[i];
 clear_live(p,&freevars);int id=compile(p,params,body,caps,n,isfreeze?1:2);
 for(int i=0;i<n;i++)argument(f,i,caps[i].slot);
 put(&f->code,"  t[%d]=%s(ctx,%d,rc_native_%d,%d,%s);\n",t,closure_api(p),isfreeze?0:1,id,n,n?"a":"NULL");
 check(f,t);drop_dead(f,after);discard(caps);p->lambdas++;return t;
}
static int trap(RcEmitFn *f,RcValue *args,int tail,const Live *after){
 Program *p=f->p;if(count(p,args)!=2)bad(p,"rc emit: trap-error arity");
 Live protected_after=uses(f,second(args));merge(p,&protected_after,after);
 int dst=fresh(f),mark=f->slots,id=f->label++;char saved[64];snprintf(saved,sizeof saved,"%s",f->fail);snprintf(f->fail,sizeof f->fail,"trap_%d",id);
 int val=expr(f,first(args),0,&protected_after);move(f,dst,val);clear_live(p,&protected_after);snprintf(f->fail,sizeof f->fail,"%s",saved);
 drop_dead(f,after);put(&f->code,"  goto trap_done_%d;\ntrap_%d:;\n",id,id);
 for(int i=mark;i<f->slots;i++)drop(f,i);
 int err=fresh(f);put(&f->code,"  t[%d]=rc_string(ctx,rc_error_message(ctx));\n",err);check(f,err);put(&f->code,"  rc_clear_error(ctx);\n");
 int h=expr(f,second(args),0,after);call_argument(f,0,err);
 put(&f->code,"  t[%d]=%s(ctx,t[%d],1,a);\n",dst,call_api(f,tail),h);drop(f,h);if(!optimize(f))drop(f,err);check(f,dst);drop_dead(f,after);
 put(&f->code,"trap_done_%d:;\n",id);return dst;
}
static int function_head(RcEmitFn *f,RcValue *head,const Live *after){
 if(is_symbol(head)&&binding(f,name(head))<0){
  int fn=fresh(f);size_t index=symbol_index(f,name(head));
  /* Resolve the current binding before evaluating any argument. The table
   * caches symbol identity only; redefining functions remains observable. */
  put(&f->code,"  t[%d]=rc_function_get(ctx,symbols[%zu]);\n",fn,index);check(f,fn);return fn;
 }
 int fn=expr(f,head,0,after);put(&f->code,"  if(rc_type(t[%d])==RC_SYMBOL) { RcValue *resolved=rc_function_get(ctx,t[%d]); rc_release(t[%d]); t[%d]=resolved; }\n",fn,fn,fn,fn);check(f,fn);return fn;
}
static int expr_inner(RcEmitFn *f,RcValue *x,int tail,const Live *after);
static int expr(RcEmitFn *f,RcValue *x,int tail,const Live *after){
 int mark=f->slots,result=expr_inner(f,x,tail,after);retire_scope(f,mark,result);return result;
}
static int expr_inner(RcEmitFn *f,RcValue *x,int tail,const Live *after){
 Program *p=f->p;if(!is_list(x)||rc_is_nil(x))return atom(f,x,tail,after);
 RcValue *head=first(x),*args=rest(x);bool special=is_symbol(head)&&binding(f,name(head))<0;
 if(special&&named(head,"if"))return branch(f,args,tail,after);
 if(special&&named(head,"cond"))return cond(f,args,tail,after);
 if(special&&named(head,"let")){
  if(count(p,args)!=3||!is_symbol(first(args)))bad(p,"rc emit: let shape");
  Live init_after=copy_live(p,after);if(optimize(f)){Shadow local={name(first(args)),NULL};scan(f,at(p,args,2),&local,&init_after);}
  int a=expr(f,second(args),0,&init_after),old=f->nb;clear_live(p,&init_after);bind(f,name(first(args)),a);
  Live needed=uses(f,at(p,args,2));merge(p,&needed,after);drop_dead(f,&needed);clear_live(p,&needed);
  int result=expr(f,at(p,args,2),tail,after);f->nb=old;drop(f,a);return result;
 }
 if(special&&named(head,"do")&&count(p,args)==2){
  int n=0;RcValue *end=x;for(;is_pair(end)&&named(first(end),"do")&&binding(f,"do")<0&&count(p,rest(end))==2;end=second(rest(end)))n++;
  int *functions=alloc(p,(size_t)n*sizeof(*functions)),*left=alloc(p,(size_t)n*sizeof(*left));
  RcValue **nodes=alloc(p,(size_t)n*sizeof(*nodes));Live *suffix=alloc(p,(size_t)(n+1)*sizeof(*suffix));memset(suffix,0,(size_t)(n+1)*sizeof(*suffix));
  RcValue *q=x;for(int i=0;i<n;i++){nodes[i]=q;q=second(rest(q));}
  suffix[n]=uses(f,end);merge(p,&suffix[n],after);
  for(int i=n-1;i>=0;i--){suffix[i]=copy_live(p,&suffix[i+1]);if(optimize(f))scan(f,first(rest(nodes[i])),NULL,&suffix[i]);}
  for(int i=0;i<n;i++){functions[i]=function_head(f,first(nodes[i]),&suffix[i]);left[i]=expr(f,first(rest(nodes[i])),0,&suffix[i+1]);}
  int result=expr(f,end,0,after);
  for(int i=n-1;i>=0;i--){int next=fresh(f);call_argument(f,0,left[i]);call_argument(f,1,result);
   if(optimize(f))put(&f->code,"  t[%d]=rc_primitive_call_owned(ctx,t[%d],2,a,%s);\n",next,functions[i],tail&&i==0?"true":"false");else put(&f->code,"  t[%d]=%s(ctx,t[%d],2,a);\n",next,call_api(f,tail&&i==0),functions[i]);drop(f,functions[i]);if(!optimize(f)){drop(f,left[i]);drop(f,result);}check(f,next);result=next;}
  for(int i=0;i<=n;i++)clear_live(p,&suffix[i]);forget(p,suffix);forget(p,nodes);forget(p,functions);forget(p,left);drop_dead(f,after);return result;
 }
 if(special&&named(head,"lambda")){if(count(p,args)!=2||!is_symbol(first(args)))bad(p,"rc emit: lambda shape");return closure(f,first(args),second(args),0,after);}
 if(special&&named(head,"freeze")){if(count(p,args)!=1)bad(p,"rc emit: freeze arity");return closure(f,rc_nil(),first(args),1,after);}
 if(special&&named(head,"trap-error"))return trap(f,args,tail,after);
 if(special&&named(head,"type")){if(count(p,args)!=2)bad(p,"rc emit: type arity");return expr(f,first(args),tail,after);}
 if(special&&(named(head,"and")||named(head,"or"))){
  if(count(p,args)!=2)bad(p,"rc emit: boolean form arity");Live rhs=uses(f,second(args));merge(p,&rhs,after);
  int dst=fresh(f),a=expr(f,first(args),0,&rhs);truth(f,a);put(&f->code,"  if(%src_is_true(t[%d])) {\n",named(head,"and")?"":"!",a);drop(f,a);drop_dead(f,&rhs);clear_live(p,&rhs);
  f->boolean_tail++;int b=expr(f,second(args),tail,after);f->boolean_tail--;put(&f->code,"  t[%d]=rc_require_boolean(ctx,t[%d]);\n",b,b);check(f,b);move(f,dst,b);
  put(&f->code,"  } else {\n");move(f,dst,a);drop_dead(f,after);put(&f->code,"  }\n");return dst;
 }
 if(special&&named(head,"defun")){
  if(count(p,args)!=3||!is_symbol(first(args)))bad(p,"rc emit: defun shape");
  int id=compile(p,second(args),at(p,args,2),NULL,0,0),t=fresh(f),cl=fresh(f);
  symbol_slot(f,t,name(first(args)));check(f,t);
  put(&f->code,"  t[%d]=%s(ctx,%d,rc_native_%d,0,NULL);\n",cl,closure_api(p),count(p,second(args)),id);check(f,cl);
  if(p->options.source_metadata&&p->user_phase&&x==p->metadata_root){
   int quoted=fresh(f),data=literal(p,x);
   put(&f->code,"  t[%d]=rc_literal_%d(ctx);\n",quoted,data);check(f,quoted);
   put(&f->code,"  if(!rc_source_definition(ctx,t[%d],t[%d],%d)) goto %s;\n",t,quoted,count(p,second(args)),f->fail);drop(f,quoted);
  }
  put(&f->code,"  if(!rc_function_set(ctx,t[%d],t[%d])) goto %s;\n",t,cl,f->fail);drop(f,cl);p->defuns++;drop_dead(f,after);return t;
 }
 int n=count(p,args);Live head_after=sequence_live(f,args,after);int fn=function_head(f,head,&head_after);clear_live(p,&head_after);
 Recycle recycle={-1,-1,f->recycle};
 if(optimize(f)&&special&&named(head,"cons")&&n==2){Live used=sequence_live(f,args,NULL);
  for(int i=f->nb-1;i>=0;i--)if(binding(f,f->bind[i].name)==f->bind[i].slot&&has(&used,f->bind[i].slot)&&!has(after,f->bind[i].slot)){recycle.binding=f->bind[i].slot;recycle.token=fresh(f);break;}
  clear_live(p,&used);if(recycle.binding>=0)f->recycle=&recycle;
 }
 /* Only a direct final lexical argument can be detached. A handler/future
  * read keeps its slot in after, disabling projection. */
 Recycle *project=NULL;
 if(optimize(f)&&special&&named(head,"tl")&&n==1&&is_symbol(first(args))){int b=binding(f,name(first(args)));
  if(b>=0&&!has(after,b))for(Recycle *r=f->recycle;r;r=r->outer)if(r->binding==b){project=r;break;}}
 int *av=alloc(p,(size_t)(n+1)*sizeof(*av));RcValue *q=args;
 for(int i=0;i<n;i++){Live next=sequence_live(f,rest(q),after);av[i]=expr(f,first(q),0,&next);clear_live(p,&next);q=rest(q);}
 int result=fresh(f);
 /* Resolve the callee before argument effects, and only bypass dispatch for
  * this exact, fully applied closure. A spelling/native-code comparison would
  * mis-handle redefinition, captures, or a partially applied self closure.
  * Boolean tail continuations must still pass through the runtime guard. */
 if(optimize(f)&&tail&&!f->boolean_tail&&n==f->arity){
  f->self_loop=true;
  put(&f->code,"  if(t[%d]==self && rc_arity(self)==%d) {\n",fn,n);
  for(int i=0;i<n;i++)call_argument(f,i,av[i]);
  put(&f->code,"  goto recur;\n  }\n");
 }
 if(project){put(&f->code,"  { RcValue *candidate=t[%d]; t[%d]=NULL; t[%d]=rc_tail_call_reuse(ctx,t[%d],candidate,&t[%d]); }\n",av[0],av[0],result,fn,project->token);}
 else if(recycle.binding>=0){
  put(&f->code,"  { RcValue *candidate=t[%d] ? t[%d] : t[%d]; t[%d]=NULL; t[%d]=NULL; t[%d]=rc_cons_call_reuse(ctx,t[%d],candidate,t[%d],t[%d],%s); }\n",recycle.token,recycle.token,recycle.binding,recycle.token,recycle.binding,result,fn,av[0],av[1],tail?"true":"false");
  drop(f,av[0]);drop(f,av[1]);
 }else{for(int i=0;i<n;i++)call_argument(f,i,av[i]);if(optimize(f)&&special&&leaf_primitive(head))put(&f->code,"  t[%d]=rc_primitive_call_owned(ctx,t[%d],%d,%s,%s);\n",result,fn,n,n?"a":"NULL",tail?"true":"false");else put(&f->code,"  t[%d]=%s(ctx,t[%d],%d,%s);\n",result,call_api(f,tail),fn,n,n?"a":"NULL");if(!optimize(f))for(int i=0;i<n;i++)drop(f,av[i]);}
 if(recycle.binding>=0)f->recycle=recycle.outer;
 drop(f,fn);forget(p,av);check(f,result);drop_dead(f,after);return result;
}
static void finish_function(RcEmitFn *f,int id,int arity,int r){
 Program *p=f->p;
 put(&p->functions,"static RcValue *rc_native_%d(RcContext *ctx,RcValue *self,size_t argc,RcValue *%s*argv) {\n  (void)self; (void)argv;\n  if(argc!=%d) return rc_error(ctx,\"native arity mismatch\");\n  RcValue *result=NULL;\n",id,optimize(f)?"":"const ",arity);
 symbol_frame(&p->functions,f->symbol_used);
 if(f->frame_slots>64)put(&p->functions,"  RcValue **t=calloc(%d,sizeof(*t));\n  if(!t) return rc_error(ctx,\"native frame allocation failed\");\n  RcValue **a=t+%d; (void)a;\n",f->frame_slots+f->max_args,f->frame_slots);
 else put(&p->functions,"  RcValue *t[%d]={0}; RcValue *a[%d]; (void)a;\n",f->frame_slots?f->frame_slots:1,f->max_args?f->max_args:1);
 if(f->self_loop)put(&p->functions,"entry:;\n");
 frame_code(&p->functions,f);r=f->physical[r];
 put(&p->functions,"  result=t[%d]; t[%d]=NULL;\ncleanup:\n  for(size_t i=0;i<%d;i++) if(t[i]) rc_release(t[i]);\n",r,r,f->frame_slots);if(f->frame_slots>64)put(&p->functions,"  free(t);\n");put(&p->functions,"  return result;\n");
 if(f->self_loop){
  /* a owns the staged arguments while the previous iteration's lexical and
   * temporary owners are dropped. argv belongs to the active runtime call,
   * so it is distinct from this callback's frame and scratch storage. */
  put(&p->functions,"recur:\n  for(size_t i=0;i<%d;i++) if(t[i]) { rc_release(t[i]); t[i]=NULL; }\n",f->frame_slots);
  for(int i=0;i<arity;i++)put(&p->functions,"  argv[%d]=a[%d]; a[%d]=NULL;\n",i,i,i);
  put(&p->functions,"  rc_safepoint(ctx);\n  goto entry;\n");
 }
 put(&p->functions,"}\n");
}
static int compile(Program *p,RcValue *params,RcValue *body,Binding *caps,int ncaps,int freeze){
 RcEmitFn f={0};f.p=p;f.code.owner=p;int id=p->next++,arity=freeze==1?0:(freeze==2?1:count(p,params));f.arity=arity;snprintf(f.fail,sizeof f.fail,"cleanup");
 for(int i=0;i<ncaps;i++){int s=fresh(&f);bind(&f,caps[i].name,s);put(&f.code,"  t[%d]=rc_retain(rc_capture(self,%d));\n",s,i);}
 for(int i=0;i<arity;i++,params=freeze==2?rc_nil():rest(params)){RcValue *param=freeze==2?params:first(params);if(!is_symbol(param))bad(p,"rc emit: function parameter");for(int j=ncaps;j<f.nb;j++)if(!strcmp(f.bind[j].name,name(param)))bad(p,"rc emit: duplicate function parameter");int s=fresh(&f);bind(&f,name(param),s);
  if(optimize(&f))put(&f.code,"  t[%d]=argv[%d]; argv[%d]=NULL;\n",s,i,i);else put(&f.code,"  t[%d]=rc_retain(argv[%d]);\n",s,i);}
 Live needed=uses(&f,body);drop_dead(&f,&needed);clear_live(p,&needed);int r=expr(&f,body,1,NULL);
 finish_function(&f,id,arity,r);
 discard(f.code.p);discard(f.bind);return id;
}
/* Construct source metadata as values, never by parsing/evaluating user code.
 * The helper owns each temporary and transfers only its completed AST result. */
static int literal_node(RcEmitFn *f,RcValue *x){
 if(!is_pair(x))return atom(f,x,0,NULL);
 int h=literal_node(f,first(x)),t=literal_node(f,rest(x)),out=fresh(f);
 put(&f->code,"  t[%d]=rc_cons(ctx,t[%d],t[%d]);\n",out,h,t);drop(f,h);drop(f,t);check(f,out);return out;
}
static int literal(Program *p,RcValue *x){
 RcEmitFn f={0};f.p=p;f.code.owner=p;snprintf(f.fail,sizeof f.fail,"cleanup");
 int id=p->next_literal++,result=literal_node(&f,x);
 put(&p->literals,"static RcValue *rc_literal_%d(RcContext *ctx) {\n  RcValue *result=NULL;\n",id);
 symbol_frame(&p->literals,f.symbol_used);
 if(f.slots>64)put(&p->literals,"  RcValue **t=calloc(%d,sizeof(*t)); if(!t) return rc_error(ctx,\"source metadata allocation failed\");\n",f.slots);
 else put(&p->literals,"  RcValue *t[%d]={0};\n",f.slots?f.slots:1);
 put(&p->literals,"%s  result=t[%d]; t[%d]=NULL;\ncleanup:\n  for(size_t i=0;i<%d;i++) if(t[i]) rc_release(t[i]);\n",f.code.p?f.code.p:"",result,result,f.slots);
 if(f.slots>64)put(&p->literals,"  free(t);\n");put(&p->literals,"  return result;\n}\n");return id;
}
static void metadata_helpers(FILE *out){
 fputs("static RcValue *rc_source_call(RcContext *ctx,const char *name,size_t n,RcValue *const *args) {\n"
       "  RcValue *fn=rc_symbol(ctx,name); if(!fn) return NULL;\n"
       "  RcValue *value=rc_apply(ctx,fn,n,args); rc_release(fn); return value;\n}\n"
       "static bool rc_source_arity_value(RcContext *ctx,RcValue *name,size_t arity) {\n"
       "  RcValue *count=rc_integer(ctx,(int64_t)arity); if(!count) return false;\n"
       "  RcValue *args[]={name,count}; RcValue *value=rc_source_call(ctx,\"shen.store-arity\",2,args);\n"
       "  rc_release(count); bool ok=value!=NULL; rc_release(value); return ok;\n}\n"
       "static bool rc_source_definition(RcContext *ctx,RcValue *name,RcValue *ast,size_t arity) {\n"
       "  RcValue *system=rc_source_call(ctx,\"shen.sysfunc?\",1,&name); if(!system) return false;\n"
       "  system=rc_require_boolean(ctx,system); if(!system) return false;\n"
       "  bool forbidden=rc_is_true(system); rc_release(system);\n"
       "  if(forbidden) { rc_error(ctx,\"%s is not a legitimate function name\",rc_text(name)); return false; }\n"
       "  if(!rc_source_arity_value(ctx,name,arity)) return false;\n"
       "  RcValue *args[]={name,ast}; RcValue *value=rc_source_call(ctx,\"shen.record-kl\",2,args);\n"
       "  bool ok=value!=NULL; rc_release(value); return ok;\n}\n",out);
}
/* Replay reader effects in their original order. Values are built as data;
 * auxiliary definitions go through the same native compiler as program code. */
static int event_function(RcEmitFn *f,const char *name){
 int fn=fresh(f);symbol_slot(f,fn,name);check(f,fn);
 put(&f->code,"  { RcValue *resolved=rc_function_get(ctx,t[%d]); rc_release(t[%d]); t[%d]=resolved; }\n",fn,fn,fn);check(f,fn);return fn;
}
static int event_call(RcEmitFn *f,int fn,int *args,int n){
 int r=fresh(f);for(int i=0;i<n;i++)call_argument(f,i,args[i]);
 put(&f->code,"  t[%d]=%s(ctx,t[%d],%d,%s);\n",r,call_api(f,0),fn,n,n?"a":"NULL");
 drop(f,fn);if(!optimize(f))for(int i=0;i<n;i++)drop(f,args[i]);check(f,r);return r;
}
static int compile_event(Program *p,RcValue *event){
 if(!is_pair(event)||!is_symbol(first(event)))bad(p,"invalid source reader event");
 RcValue *kind=first(event),*args=rest(event);
 if(named(kind,"definition")){
  if(count(p,args)!=1||!is_pair(first(args))||!named(first(first(args)),"defun"))bad(p,"invalid native source definition event");
  return compile(p,rc_nil(),first(args),NULL,0,0);
 }
 RcEmitFn f={0};f.p=p;f.code.owner=p;snprintf(f.fail,sizeof f.fail,"cleanup");int id=p->next++,result;
 if(named(kind,"call")){
  if(count(p,args)!=2||!is_symbol(first(args)))bad(p,"invalid source metadata call event");
  if(named(first(args),"eval-kl"))bad(p,"reader eval-kl definitions require native definition events");
  int fn=event_function(&f,name(first(args))),n=count(p,second(args));int *av=alloc(p,(size_t)(n+1)*sizeof(*av));
  RcValue *q=second(args);for(int i=0;i<n;i++,q=rest(q))av[i]=literal_node(&f,first(q));
  result=event_call(&f,fn,av,n);forget(p,av);
 }else if(named(kind,"global")){
  if(count(p,args)!=2||!is_symbol(first(args)))bad(p,"invalid source metadata global event");
  int symbol=literal_node(&f,first(args));result=literal_node(&f,second(args));
  put(&f.code,"  if(!rc_global_set(ctx,t[%d],t[%d])) goto cleanup;\n",symbol,result);drop(&f,symbol);
 }else if(named(kind,"property")){
  if(count(p,args)!=3)bad(p,"invalid source property event");
  int fn=event_function(&f,"put"),av[4];RcValue *q=args;
  for(int i=0;i<3;i++,q=rest(q))av[i]=literal_node(&f,first(q));
  int property_name=fresh(&f);symbol_slot(&f,property_name,"*property-vector*");check(&f,property_name);
  av[3]=fresh(&f);put(&f.code,"  t[%d]=rc_global_get(ctx,t[%d]);\n",av[3],property_name);drop(&f,property_name);check(&f,av[3]);
  result=event_call(&f,fn,av,4);
 }else if(named(kind,"macro")||named(kind,"datatype")){
  if(count(p,args)!=1||!is_symbol(first(args)))bad(p,"invalid source callback metadata event");
  int fn=event_function(&f,named(kind,"macro")?"shen.record-macro":"shen.remember-datatype");
  int symbol=literal_node(&f,first(args)),lookup=event_function(&f,"fn"),copy=fresh(&f);
  put(&f.code,"  t[%d]=rc_retain(t[%d]);\n",copy,symbol);int one[]={copy};int wrapper=event_call(&f,lookup,one,1);
  int two[]={symbol,wrapper};result=event_call(&f,fn,two,2);
 }else bad(p,"unsupported source reader event kind");
 finish_function(&f,id,0,result);return id;
}
static bool identifier(const char *s) {
  if(!s||!*s) return false;
  for(size_t i=0;s[i];i++) if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||s[i]=='_'||(i&&s[i]>='0'&&s[i]<='9'))) return false;
  return true;
}
bool rc_emit_program(RcContext *ctx,FILE *out,RcValue *const *kernel,size_t nkernel,
                     RcValue *const *user,size_t nuser,const RcEmitOptions *options,RcEmitReport *report) {
  RcEmitOptions defaults={.emit_main=true};
  if(!options) options=&defaults;
  const char *bootstrap=options->bootstrap_name?options->bootstrap_name:"rc_bootstrap_kernel";
  if(!ctx||!out||(nkernel&&!kernel)||(nuser&&!user)||!identifier(bootstrap)) {
    rc_error(ctx,"invalid emitter arguments/bootstrap identifier"); return false;
  }
  if(report) *report=(RcEmitReport){0};
  Program *p=calloc(1,sizeof(*p));
  if(!p) { rc_error(ctx,"compiler out of memory"); return false; }
  p->options=*options; p->ctx=ctx; p->functions.owner=p; p->literals.owner=p;
  if(setjmp(p->failure)) { clear_arena(p); free(p); return false; }
  Buf installs={.owner=p},kt={.owner=p},ut={.owner=p},events={.owner=p};
  if(options->source_events&&!options->source_metadata)bad(p,"source reader events require source metadata mode");
  for(RcValue *q=options->source_events;q&&!rc_is_nil(q);q=rest(q)){
    if(!is_pair(q))bad(p,"improper source reader event list");
    p->user_phase=false;p->metadata_root=NULL;
    int id=compile_event(p,first(q));
    put(&events,"  if(!run_top(ctx,rc_native_%d,0)) return false;\n",id);
  }
  for(int phase=0;phase<2;phase++) {
    p->user_phase=phase!=0;
    RcValue *const *forms=phase?user:kernel; size_t n=phase?nuser:nkernel;
    for(size_t i=0;i<n;i++) {
      if(!forms[i]) bad(p,"null KLambda form");
      p->metadata_root=phase?forms[i]:NULL;
      int id=compile(p,rc_nil(),forms[i],NULL,0,0);
      int def=is_pair(forms[i])&&named(first(forms[i]),"defun");
      Buf *where=phase?&ut:(def?&installs:&kt);
      put(where,"  if(!run_top(ctx,rc_native_%d,%d)) return false;\n",id,phase&&!def);
      if(!def) p->tops++;
    }
  }
  fputs("/* Native KLambda -> independent reference-counted runtime.\n"
        " * Owned slots, explicit drops, and cleanup on every return path. */\n"
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <stdint.h>\n#include <math.h>\n#include \"rc.h\"\n#include \"primitives.h\"\n#include \"eval.h\"\n",out);
  if(p->nsymbols) {
    Buf names={.owner=p};
    for(size_t i=0;i<p->nsymbols;i++){if(i)put(&names,",\n");quoted(&names,p->symbols[i]);}
    fprintf(out,"static const char *const rc_module_symbol_names[]={\n%s\n};\nstatic const RcSymbolTable rc_module_symbols={rc_module_symbol_names,%zu};\n",names.p,p->nsymbols);
  }
  if(p->drop_range_used)fputs("static void rc_drop_range(RcValue **slots,size_t first,size_t end) {\n"
                            "  for(size_t i=first;i<end;i++) { if(slots[i]) rc_release(slots[i]); slots[i]=NULL; }\n}\n",out);
  for(int i=0;i<p->next;i++) fprintf(out,"static RcValue *rc_native_%d(RcContext*,RcValue*,size_t,RcValue *%s*);\n",i,options->conservative_ownership?"const":"");
  if(p->next_literal) metadata_helpers(out);
  if(p->literals.p) fputs(p->literals.p,out);
  if(p->functions.p) fputs(p->functions.p,out);
  fprintf(out,"static bool run_top(RcContext *ctx,%s native,int print) {\n  RcValue *fn=%s(ctx,0,native,0,NULL);\n",options->conservative_ownership?"RcNative":"RcNativeOwned",closure_api(p));
  fputs(
        "  if(!fn) return false;\n"
        "  RcValue *value=rc_apply(ctx,fn,0,NULL); rc_release(fn);\n"
        "  if(!value) return false;\n"
        "  if(print && getenv(\"SHEN_RC_PRINT\")) { rc_print(stdout,value); fputc('\\n',stdout); }\n"
        "  rc_release(value); rc_safepoint(ctx); return true;\n}\n",out);
  fprintf(out,"bool %s(RcContext *ctx) {\n",bootstrap);
  fputs("  if(!rc_install_primitives(ctx) || !rc_install_eval(ctx)) return false;\n",out);
  if(installs.p) fputs(installs.p,out);
  fputs("  if(!rc_install_overwrites(ctx)) return false;\n",out);
  if(kt.p) fputs(kt.p,out);
  if(options->init_name&&*options->init_name) {
    Buf init={.owner=p}; quoted(&init,options->init_name);
    fprintf(out,"  { RcValue *fn=rc_symbol(ctx,%s); if(!fn) return false; RcValue *v=rc_apply(ctx,fn,0,NULL); rc_release(fn); if(!v) return false; rc_release(v); }\n",init.p);
  }
  fputs("  return true;\n}\n",out);
  if(nuser||events.n) { fputs("static bool rc_run_user(RcContext *ctx) {\n",out); if(events.p) fputs(events.p,out); if(ut.p) fputs(ut.p,out); fputs("  return true;\n}\n",out); }
  if(options->emit_main) {
    fputs("#ifndef SHEN_RC_NO_MAIN\nint main(void) {\n  RcContext *ctx=rc_context_new();\n  if(!ctx) return 1;\n",out);
    fprintf(out,"  if(!%s(ctx)) goto failed;\n",bootstrap);
    if(nuser||events.n) fputs("  if(!rc_run_user(ctx)) goto failed;\n",out);
    fputs("  if(getenv(\"SHEN_RC_STATS\")) { RcStats s=rc_stats(ctx); fprintf(stderr,\"RC_STATS {\\\"live_objects\\\":%zu,\\\"live_bytes\\\":%zu,\\\"peak_objects\\\":%zu,\\\"retain_calls\\\":%zu,\\\"release_calls\\\":%zu,\\\"pair_allocations\\\":%zu,\\\"pair_reuse_attempts\\\":%zu,\\\"pair_reuses\\\":%zu,\\\"pair_reuse_misses\\\":%zu,\\\"pair_tail_transfers\\\":%zu,\\\"pair_tail_shares\\\":%zu,\\\"capture_edges_created\\\":%zu,\\\"live_capture_edges\\\":%zu,\\\"peak_capture_edges\\\":%zu,\\\"cached_objects\\\":%zu,\\\"cached_bytes\\\":%zu,\\\"peak_cached_bytes\\\":%zu,\\\"system_allocations\\\":%zu,\\\"system_frees\\\":%zu,\\\"collections\\\":%zu,\\\"acyclic_collections\\\":%zu,\\\"cycle_objects_freed\\\":%zu}\\n\",s.live_objects,s.live_bytes,s.peak_objects,s.retain_calls,s.release_calls,s.pair_allocations,s.pair_reuse_attempts,s.pair_reuses,s.pair_reuse_misses,s.pair_tail_transfers,s.pair_tail_shares,s.capture_edges_created,s.live_capture_edges,s.peak_capture_edges,s.cached_objects,s.cached_bytes,s.peak_cached_bytes,s.system_allocations,s.system_frees,s.collections,s.acyclic_collections,s.cycle_objects_freed); }\n",out);
    fputs("  rc_context_free(ctx); return 0;\nfailed:\n"
          "  fprintf(stderr,\"shen-rc: %s\\n\",rc_error_message(ctx));\n"
          "  rc_context_free(ctx); return 1;\n}\n#endif\n",out);
  }
  if(report) *report=(RcEmitReport){p->defuns,p->tops,p->lambdas};
  bool ok=!ferror(out); if(!ok) rc_error(ctx,"failed writing generated C");
  clear_arena(p); free(p); return ok;
}
bool rc_read_path_ex(RcContext *ctx,const char *path,bool escaped,RcValue ***forms_out,size_t *count_out) {
  if(!forms_out||!count_out) { rc_error(ctx,"invalid reader output"); return false; }
  *forms_out=NULL; *count_out=0;
  FILE *input=fopen(path,"rb");
  if(!input) { rc_error(ctx,"cannot open KLambda source %s",path); return false; }
  char *bytes=NULL; size_t length=0,capacity=0;
  bool ok=false; RcValue **forms=NULL; size_t n=0,slots=0;
  for(;;) {
    if(capacity-length<4097) {
      if(capacity>SIZE_MAX/2-4097) { rc_error(ctx,"KLambda source too large"); goto done; }
      size_t next=capacity?capacity*2:8192;
      char *grown=realloc(bytes,next);
      if(!grown) { rc_error(ctx,"out of memory reading KLambda source"); goto done; }
      bytes=grown; capacity=next;
    }
    size_t got=fread(bytes+length,1,capacity-length-1,input);
    if(memchr(bytes+length,0,got)) { rc_error(ctx,"NUL byte in KLambda source %s",path); goto done; }
    length+=got;
    if(ferror(input)) { rc_error(ctx,"failed reading KLambda source %s",path); goto done; }
    if(feof(input)) break;
  }
  bytes[length]=0;
  const char *cursor=bytes;
  for(;;) {
    RcValue *form=escaped?rc_read_escaped(ctx,&cursor):rc_read(ctx,&cursor);
    if(!form) { if(rc_has_error(ctx)) goto done; break; }
    if(n==slots) {
      if(slots>SIZE_MAX/2/sizeof(*forms)) { rc_release(form); rc_error(ctx,"too many KLambda forms"); goto done; }
      size_t next=slots?slots*2:64;
      RcValue **grown=realloc(forms,next*sizeof(*forms));
      if(!grown) { rc_release(form); rc_error(ctx,"out of memory storing KLambda forms"); goto done; }
      forms=grown; slots=next;
    }
    forms[n++]=form;
  }
  *forms_out=forms; *count_out=n; forms=NULL; ok=true;
done:
  if(forms) { for(size_t i=0;i<n;i++) rc_release(forms[i]); free(forms); }
  free(bytes); fclose(input); return ok;
}

bool rc_read_path(RcContext *ctx,const char *path,RcValue ***forms,size_t *count){return rc_read_path_ex(ctx,path,false,forms,count);}
