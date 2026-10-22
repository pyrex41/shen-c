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
typedef struct ModuleDef { size_t symbol; int id,arity; bool exact_hashkey,kernel_phase; struct ModuleDef *next; } ModuleDef;
typedef struct ModuleCall { size_t symbol; struct ModuleCall *next; bool call, tail; } ModuleCall;
struct Program { RcEmitOptions options; RcContext *ctx; jmp_buf failure; Allocation *allocations; Buf functions,literals,constants,transparent_do,pvar_identities; const char **symbols; size_t nsymbols,symbol_capacity; ModuleDef *module_defs; ModuleCall *module_calls; bool user_phase,top_body,drop_range_used,drop_range_emitted,transparent_do_used,lazyderef_used; RcValue **runtime_literals; size_t nruntime_literals; RcValue *metadata_root; int next,next_literal,next_constant; long defuns, lambdas, tops; };
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
typedef struct DoGuard { int slot; struct DoGuard *outer; } DoGuard;
typedef struct {
  Program *p; Buf code; Binding *bind; int nb, cap, slots, label, max_args; bool symbol_used;
  int arity, boolean_tail, literal_capture; bool once,self_loop,musttail_used,borrowed_fn_used,assoc_fast,pvar_fast,lazyderef_fast; size_t assoc_symbols[5],prolog_symbols[7];
  DoGuard *do_guard;
  int *physical,*free_slots,slot_cap,frame_slots,nfree; unsigned char *active,*borrowed_fn;
  char fail[64]; struct Recycle *recycle; long constant_min;
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
    f->borrowed_fn=resize(f->p,f->borrowed_fn,(size_t)f->slot_cap*sizeof(*f->borrowed_fn));
  }
  f->physical[slot]=f->nfree?f->free_slots[--f->nfree]:f->frame_slots++;
  f->active[slot]=1;f->borrowed_fn[slot]=0;return slot;
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
static void rewrite_frame_code(Buf *out,const RcEmitFn *f) {
  const char *s=f->code.p?f->code.p:"";size_t start=0,i=0;
  while(s[i]) {
    if(s[i]=='\"'||s[i]=='\'') {
      char quote=s[i++];while(s[i]){if(s[i]=='\\'&&s[i+1])i+=2;else if(s[i++]==quote)break;}
    }else if(s[i]=='t'&&s[i+1]=='['&&(i==0||!((s[i-1]>='a'&&s[i-1]<='z')||(s[i-1]>='A'&&s[i-1]<='Z')||(s[i-1]>='0'&&s[i-1]<='9')||s[i-1]=='_'))) {
      size_t end=i+2;int slot=0;while(s[end]>='0'&&s[end]<='9'){slot=slot*10+s[end]-'0';end++;}
      if(end>i+2&&s[end]==']'&&slot<f->slots) {
        if(f->borrowed_fn[slot])put(out,"%.*sborrowed_fn",(int)(i-start),s+start);
        else put(out,"%.*st[%d]",(int)(i-start),s+start,f->physical[slot]);
        i=end+1;start=i;
      }else i++;
    }else i++;
  }
  put(out,"%s",s+start);
}
/* Facts concern physical frame slots and only the exact generated forms
 * below. Unknown lines (including every control-flow boundary or escaping
 * slot address) discard all facts. Function calls receive values/scratch a,
 * never the private t array, so a simple assignment invalidates its dst only. */
static void frame_code(Buf *out,const RcEmitFn *f) {
 Buf rewritten={.owner=f->p};rewrite_frame_code(&rewritten,f);
 unsigned char *nulls=alloc(f->p,(size_t)(f->frame_slots?f->frame_slots:1));
 memset(nulls,1,(size_t)f->frame_slots);
 char empty[1]="",*line=rewritten.p?rewritten.p:empty;
 while(*line) {
  char *end=strchr(line,'\n');size_t len=end?(size_t)(end-line)+1:strlen(line);
  /* Terminate the line while matching: sscanf may measure its whole input,
   * which made matching quadratic in function size. */
  if(end)*end=0;
  int a=-1,b=-1,c=-1,n=0;char label[64];bool omit=false,recognized=false;
  if((n=0,sscanf(line,"  if(t[%d]) rc_release(t[%d]); t[%d]=NULL;%n",&a,&b,&c,&n))==3&&
     a==b&&b==c&&a>=0&&a<f->frame_slots&&n==(int)(len-(end?1:0))) {
   omit=nulls[a];nulls[a]=1;recognized=true;
  } else if((n=0,sscanf(line,"  rc_drop_range(t,%d,%d);%n",&a,&b,&n))==2&&
            a>=0&&b>=a&&b<=f->frame_slots&&n==(int)(len-(end?1:0))) {
   omit=true;for(int k=a;k<b;k++)if(!nulls[k])omit=false;
   for(int k=a;k<b;k++)nulls[k]=1;recognized=true;
   if(!omit)f->p->drop_range_emitted=true;
  } else if((n=0,sscanf(line,"  t[%d]=NULL;%n",&a,&n))==1&&a>=0&&a<f->frame_slots&&
            n==(int)(len-(end?1:0))) {nulls[a]=1;recognized=true;}
  else if((n=0,sscanf(line,"  t[%d]=t[%d]; t[%d]=NULL;%n",&a,&b,&c,&n))==3&&b==c&&
          a>=0&&a<f->frame_slots&&b>=0&&b<f->frame_slots&&n==(int)(len-(end?1:0))) {
   nulls[a]=nulls[b];nulls[b]=1;recognized=true;
  } else if((n=0,sscanf(line,"  a[%d]=t[%d];%n",&a,&b,&n))==2&&
            n==(int)(len-(end?1:0)))recognized=true;
  else if((n=0,sscanf(line,"  if(!t[%d]) goto %*[^;];%n",&a,&n))==1&&
          n==(int)(len-(end?1:0)))recognized=true;
  /* Borrowed callee storage is separate from owned frame slots. */
  else if((n=0,sscanf(line,"  borrowed_fn=NULL;%n",&n))==0&&
          n==(int)(len-(end?1:0)))recognized=true;
  else if((n=0,sscanf(line,"  borrowed_fn=rc_function_get_borrowed(ctx,symbols[%d]);%n",&a,&n))==1&&
          a>=0&&n==(int)(len-(end?1:0)))recognized=true;
  else if((n=0,sscanf(line,"  if(!borrowed_fn) goto %63[a-zA-Z0-9_];%n",label,&n))==1&&
          n==(int)(len-(end?1:0)))recognized=true;
  /* Ordinary generated RHS calls cannot write any other frame slot. Reject
   * compound statements, labels, braces and address-taking before trusting it. */
  else if((n=0,sscanf(line,"  t[%d]=%n",&a,&n))==1&&a>=0&&a<f->frame_slots&&n>0) {
   bool safe=true;char quote=0;int semicolons=0;
   for(size_t i=(size_t)n;i<len;i++) {
    char ch=line[i];
    if(quote){if(ch=='\\'&&i+1<len)i++;else if(ch==quote)quote=0;}
    else if(ch=='\"'||ch=='\'')quote=ch;
    else if(ch==';')semicolons++;
    else if(ch=='{'||ch=='}'||ch==':'||ch=='&')safe=false;
   }
   if(safe&&semicolons==1&&!quote){nulls[a]=0;recognized=true;}
  }
  if(end)*end='\n';
  if(!recognized)memset(nulls,0,(size_t)f->frame_slots);
  if(!omit)put(out,"%.*s",(int)len,line);
  line+=len;
 }
 discard(nulls);discard(rewritten.p);
}
static bool c_ident(char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';}
/* Bound the C optimizer's work on enormous initializer bodies. Helpers share
 * the activation's existing owned slots and scratch arguments. They add no
 * allocations, safepoints or language callbacks. Only error exits cross a
 * helper boundary; the original outer cleanup still owns every live slot.
 * Dynamic modules, loops, borrowed-callee storage, labels and C locals keep
 * their original lowering. Quoted text is never rewritten. */
static int function_chunks(RcEmitFn *f,int id){
 Program *p=f->p;
 if(p->options.expression_entry||p->options.conservative_ownership||f->self_loop||f->musttail_used||f->borrowed_fn_used||f->code.n<131072)return 0;
 Buf code={.owner=p};frame_code(&code,f);size_t n=code.n;
 char *mask=alloc(p,n+1);memcpy(mask,code.p,n+1);
 for(size_t i=0;i<n;){
  size_t start=i;
  if(mask[i]=='"'||mask[i]=='\''){
   char quote=mask[i++];while(i<n){if(mask[i]=='\\'&&i+1<n)i+=2;else if(mask[i++]==quote)break;}
  }else if(mask[i]=='/'&&i+1<n&&mask[i+1]=='/'){
   while(i<n&&mask[i]!='\n')i++;
  }else if(mask[i]=='/'&&i+1<n&&mask[i+1]=='*'){
   i+=2;while(i+1<n&&(mask[i]!='*'||mask[i+1]!='/'))i++;if(i+1<n)i+=2;
  }else{i++;continue;}
  for(size_t j=start;j<i;j++)if(mask[j]!='\n')mask[j]=' ';
 }
 for(size_t i=0;i<n;){
  if(!((mask[i]>='a'&&mask[i]<='z')||(mask[i]>='A'&&mask[i]<='Z')||mask[i]=='_')){i++;continue;}
  size_t end=i+1;while(end<n&&c_ident(mask[end]))end++;
  size_t next=end;while(next<n&&(mask[next]==' '||mask[next]=='\t'||mask[next]=='\n'))next++;
  if(next<n&&mask[next]==':')return 0;
  static const char *const disallowed[]={"RcValue","bool","int","size_t","int64_t","uint64_t","double","float","char","const","return","switch","case","default","break","continue","for","while","do"};
  for(size_t j=0;j<sizeof(disallowed)/sizeof(*disallowed);j++)
   if(strlen(disallowed[j])==end-i&&!memcmp(mask+i,disallowed[j],end-i))return 0;
  if(end-i==4&&!memcmp(mask+i,"goto",4)&&strncmp(mask+i,"goto cleanup;",13))return 0;
  i=end;
 }
 const size_t target=65536;size_t *cuts=alloc(p,(n/target+3)*sizeof(*cuts)),count=0,start=0;
 int depth=0;char last=0;
 for(size_t i=0;i<n;i++){
  if(mask[i]=='{')depth++;else if(mask[i]=='}')depth--;
  if(depth<0)return 0;
  if(mask[i]=='\n'&&!depth&&i+1-start>=target&&(last==';'||last=='}')){
   size_t next=i+1;while(next<n&&(mask[next]==' '||mask[next]=='\t'||mask[next]=='\n'))next++;
   if(next+4<=n&&!memcmp(mask+next,"else",4)&&!c_ident(mask[next+4]))continue;
   cuts[count++]=i+1;start=i+1;
  }
  if(mask[i]!=' '&&mask[i]!='\t'&&mask[i]!='\n')last=mask[i];
 }
 if(depth)return 0;if(start<n)cuts[count++]=n;if(count<2)return 0;
 start=0;
 for(size_t part=0;part<count;part++){
  put(&p->functions,"#if defined(__GNUC__) || defined(__clang__)\n__attribute__((noinline))\n#endif\nstatic bool rc_native_chunk_%d_%zu(RcContext *ctx,RcValue *self,RcValue **argv,RcValue **t,RcValue **a,RcValue *const *symbols) {\n  (void)ctx; (void)self; (void)argv; (void)t; (void)a; (void)symbols;\n",id,part);
  size_t copied=start;
  for(size_t i=start;i<cuts[part];i++)if(!strncmp(mask+i,"goto cleanup;",13)){
   put(&p->functions,"%.*sreturn false;",(int)(i-copied),code.p+copied);i+=12;copied=i+1;
  }
  put(&p->functions,"%.*s  return true;\n}\n",(int)(cuts[part]-copied),code.p+copied);start=cuts[part];
 }
 return (int)count;
}
static void drop(RcEmitFn *f,int x) {
 if(f->borrowed_fn[x])put(&f->code,"  t[%d]=NULL;\n",x);
 else put(&f->code,"  if(t[%d]) rc_release(t[%d]); t[%d]=NULL;\n",x,x,x);
}
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
/* Exact lexical identity proof shared by native construction and self-loop
 * guards. Distinct binders make the body reference unambiguous. */
static bool exact_second_identity(RcValue *params,RcValue *body,int arity) {
 return arity==2&&is_symbol(body)&&is_symbol(first(params))&&is_symbol(second(params))&&
        !strcmp(name(body),name(second(params)))&&strcmp(name(first(params)),name(second(params)));
}
static bool exact_hashkey(Program *p,RcValue *params,RcValue *body) {
 if(count(p,params)!=1||!is_symbol(first(params))||!is_pair(body)||!named(first(body),"let"))return false;
 RcValue *la=rest(body);if(count(p,la)!=3)return false;
 RcValue *letvar=at(p,la,0),*map=at(p,la,1),*result=at(p,la,2);
 static const char *const reserved[]={"let","lambda","map","explode","string->n","shen.prodbutzero","shen.app","str","number?"};
 for(size_t i=0;i<sizeof(reserved)/sizeof(*reserved);i++)
  if(named(first(params),reserved[i])||named(letvar,reserved[i]))return false;
 if(!is_symbol(letvar)||!is_pair(map)||!named(first(map),"map"))return false;
 RcValue *ma=rest(map);if(count(p,ma)!=2)return false;
 RcValue *lambda=at(p,ma,0),*explode=at(p,ma,1);
 if(!is_pair(lambda)||!named(first(lambda),"lambda"))return false;
 RcValue *lca=rest(lambda);if(count(p,lca)!=2||!is_symbol(at(p,lca,0)))return false;
 RcValue *lambda_param=at(p,lca,0),*string_n=at(p,lca,1);
 if(rc_equal(lambda_param,first(params))||rc_equal(lambda_param,letvar))return false;
 for(size_t i=0;i<sizeof(reserved)/sizeof(*reserved);i++)if(named(lambda_param,reserved[i]))return false;
 if(!is_pair(string_n)||!named(first(string_n),"string->n")||count(p,rest(string_n))!=1||
    !rc_equal(at(p,rest(string_n),0),lambda_param))return false;
 if(!is_pair(explode)||!named(first(explode),"explode")||count(p,rest(explode))!=1||
    !rc_equal(at(p,rest(explode),0),first(params)))return false;
 if(!is_pair(result)||!named(first(result),"shen.prodbutzero")||count(p,rest(result))!=2||
    !rc_equal(at(p,rest(result),0),letvar))return false;
 RcValue *one=at(p,rest(result),1);
 return rc_type(one)==RC_NUMBER&&rc_number_is_exact_integer(one)&&rc_integer_value(one)==1;
}
/* Mode 2 only permits guarded self recursion through pending transparent do
 * calls. Its ordinary fallback must finish before invoking those do calls. */
static const char *call_api(RcEmitFn *f,int tail){return optimize(f)?(tail==1?"rc_tailcall_owned_args":"rc_apply_rooted_owned_args"):(tail==1?"rc_tailcall":"rc_apply");}
static void call_argument(RcEmitFn *f,int index,int slot){argument(f,index,slot);if(optimize(f))put(&f->code,"  t[%d]=NULL;\n",slot);}
static bool leaf_primitive(RcValue *head) {
 static const char *names[]={"hd","tl","cons?","cons","=","number?","string?",
  "symbol?","boolean?","integer?","variable?","absvector?","not",
  "+","-","*","/",">","<",">=","<="};
 for(size_t i=0;i<sizeof(names)/sizeof(*names);i++)if(named(head,names[i]))return true;
 return false;
}
static const char *tiny_leaf_api(RcValue *head,int arity) {
 if(arity==1) {
  if(named(head,"hd"))return "rc_primitive_hd_owned";
  if(named(head,"tl"))return "rc_primitive_tl_owned";
  if(named(head,"cons?"))return "rc_primitive_consp_owned";
  if(named(head,"number?"))return "rc_primitive_numberp_owned";
  if(named(head,"string?"))return "rc_primitive_stringp_owned";
 }
 if(arity==2) {
  if(named(head,"="))return "rc_primitive_eq_owned";
  if(named(head,"cons"))return "rc_primitive_cons_owned";
  if(named(head,"+"))return "rc_primitive_add_owned";
  if(named(head,"-"))return "rc_primitive_sub_owned";
  if(named(head,"*"))return "rc_primitive_mul_owned";
  if(named(head,"/"))return "rc_primitive_div_owned";
  if(named(head,">"))return "rc_primitive_gt_owned";
  if(named(head,"<"))return "rc_primitive_lt_owned";
  if(named(head,">="))return "rc_primitive_ge_owned";
  if(named(head,"<="))return "rc_primitive_le_owned";
 }
 return NULL;
}
static int expr(RcEmitFn *,RcValue *,int,const Live *);
static int compile(Program *,RcValue *,RcValue *,Binding *,int,int);
static int literal(Program *,RcValue *);
static int constant_data(RcEmitFn *,RcValue *,bool);
static long constant_nodes(RcEmitFn *,RcValue *,bool);
/* Smaller trees stay straight-line code; the table only pays off in bulk.
 * A function holding thousands of constructors (the standard library's
 * stored sources and type declarations) is data, so it tables small trees. */
#define RC_CONSTANT_DATA_MIN 64
#define RC_CONSTANT_DATA_BULK_MIN 8
#define RC_CONSTANT_DATA_BULK 2048
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
/* Interned symbols are immortal: the table pointer is already an owner, and
 * the slot's later release is a no-op. */
static void symbol_slot(RcEmitFn *f,int slot,const char *name) {
 size_t index=symbol_index(f,name);
 put(&f->code,"  t[%d]=symbols[%zu];\n",slot,index);
}
static size_t module_call_kind(RcEmitFn *f,const char *name,bool tail){
 size_t symbol=symbol_index(f,name);
 for(ModuleCall *c=f->p->module_calls;c;c=c->next)if(c->symbol==symbol){if(tail)c->tail=true;else c->call=true;return symbol;}
 ModuleCall *c=alloc(f->p,sizeof(*c));*c=(ModuleCall){symbol,f->p->module_calls,!tail,tail};f->p->module_calls=c;return symbol;
}
static size_t module_call(RcEmitFn *f,const char *name){return module_call_kind(f,name,false);}
/* Arguments of a native tail call outlive the caller's frame, so they pass
 * through this module buffer; the callee moves them out at entry. */
#define RC_MUSTTAIL_ARGS 8
static void symbol_frame(Buf *out,const RcEmitFn *f) {
 if(!f->symbol_used)return;
 if(f->p->options.expression_entry)
  put(out,"  RcValue *const *symbols=rc_compiled_literal_slots(self,%d,RC_LITERAL_SYMBOL_OFFSET);\n  if(!symbols)return rc_error(ctx,\"invalid compiled literal frame\");\n",f->literal_capture);
 else put(out,"  RcValue *const *symbols=rc_symbol_table(ctx,&rc_module_symbols);\n  if(!symbols) return NULL;\n");
}
static int runtime_literal(RcEmitFn *f,RcValue *x) {
 Program *p=f->p; size_t i=0;
 for(;i<p->nruntime_literals;i++)if(p->runtime_literals[i]==x)break;
 if(i==p->nruntime_literals){
  p->runtime_literals=resize(p,p->runtime_literals,(i+1)*sizeof(*p->runtime_literals));
  p->runtime_literals[p->nruntime_literals++]=x;
 }
 int t=fresh(f);
 put(&f->code,"  t[%d]=rc_vector_get(ctx,rc_capture(self,%d),%zu);\n",t,f->literal_capture,i);
 check(f,t); return t;
}
static int atom(RcEmitFn *f,RcValue *x,int tail,const Live *after) {
 Program *p=f->p;
 if(p->options.expression_entry && rc_type(x)!=RC_NIL && rc_type(x)!=RC_SYMBOL)
  return runtime_literal(f,x);
 int t=fresh(f);
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
static int condition(RcEmitFn *,RcValue *,const Live *);
static int branch(RcEmitFn *f,RcValue *args,int tail,const Live *after){
 Program *p=f->p;if(count(p,args)!=3)bad(p,"rc emit: if arity");
 Live left=uses(f,at(p,args,1)),right=uses(f,at(p,args,2));merge(p,&left,after);merge(p,&right,after);
 Live tests=copy_live(p,&left);merge(p,&tests,&right);
 int dst=fresh(f),test=condition(f,first(args),&tests);clear_live(p,&tests);
 put(&f->code,"  if(test_%d) {\n",test);drop_dead(f,&left);
 int a=expr(f,at(p,args,1),tail,after);move(f,dst,a);
 put(&f->code,"  } else {\n");drop_dead(f,&right);
 int b=expr(f,at(p,args,2),tail,after);move(f,dst,b);put(&f->code,"  }\n");
 clear_live(p,&left);clear_live(p,&right);return dst;
}
static int cond(RcEmitFn *f,RcValue *clauses,int tail,const Live *after){
 Program *p=f->p;int dst=fresh(f),done=f->label++;
 for(RcValue *q=clauses;!rc_is_nil(q);q=rest(q)){
  if(!is_pair(q))bad(p,"rc emit: cond clauses");RcValue *cl=first(q);if(count(p,cl)!=2)bad(p,"rc emit: cond clause");
  Live needed=sequence_live(f,rest(q),after),value=uses(f,second(cl));merge(p,&needed,&value);
  int test=condition(f,first(cl),&needed);clear_live(p,&needed);
  put(&f->code,"  if(test_%d) {\n",test);merge(p,&value,after);drop_dead(f,&value);clear_live(p,&value);
  int val=expr(f,second(cl),tail,after);move(f,dst,val);put(&f->code,"  goto cond_done_%d;\n  }\n",done);
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
 if(p->options.expression_entry){if(f->max_args<n+1)f->max_args=n+1;put(&f->code,"  a[%d]=rc_capture(self,%d);\n",n,f->literal_capture);}
 int captures=n+(p->options.expression_entry?1:0);
 put(&f->code,"  t[%d]=%s(ctx,%d,rc_native_%d,%d,%s);\n",t,closure_api(p),isfreeze?0:1,id,captures,captures?"a":"NULL");
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
static int function_head(RcEmitFn *f,RcValue *head,const Live *after,bool borrow){
 if(is_symbol(head)&&binding(f,name(head))<0){
  int fn=fresh(f);size_t index=symbol_index(f,name(head));
  /* Resolve the current binding before evaluating any argument. The table
   * caches symbol identity only; redefining functions remains observable. */
  if(borrow){f->borrowed_fn[fn]=1;f->borrowed_fn_used=true;}
  put(&f->code,"  t[%d]=%s(ctx,symbols[%zu]);\n",fn,borrow?"rc_function_get_borrowed":"rc_function_get",index);check(f,fn);return fn;
 }
 int fn=expr(f,head,0,after);put(&f->code,"  if(rc_type(t[%d])==RC_SYMBOL) { RcValue *resolved=rc_function_get(ctx,t[%d]); rc_release(t[%d]); t[%d]=resolved; }\n",fn,fn,fn,fn);check(f,fn);return fn;
}
/* A condition has a C boolean result, so canonical predicates and boolean
 * syntax need no owned true/false temporary. Generic replacements still return
 * an owned value and receive the original boolean validation/error order. */
static int condition_checked(RcEmitFn *f,RcValue *x,const Live *after,bool rhs_boolean){
 Program *p=f->p;int mark=f->slots,id=f->label++;
 put(&f->code,"  int test_%d;\n",id);
 bool special=is_pair(x)&&is_symbol(first(x))&&binding(f,name(first(x)))<0;
 RcValue *head=special?first(x):NULL,*args=special?rest(x):rc_nil();
 if(optimize(f)&&is_symbol(x)&&binding(f,name(x))<0&&(named(x,"true")||named(x,"false"))){
  put(&f->code,"  test_%d=%d;\n",id,named(x,"true"));
 }else if(optimize(f)&&special&&(named(head,"and")||named(head,"or"))&&count(p,args)==2){
  Live needed=uses(f,second(args));merge(p,&needed,after);
  int left=condition_checked(f,first(args),&needed,false);clear_live(p,&needed);
  put(&f->code,"  test_%d=test_%d;\n  if(%stest_%d) {\n",id,left,named(head,"or")?"!":"",left);
  int right=condition_checked(f,second(args),after,true);put(&f->code,"  test_%d=test_%d;\n  }\n",id,right);
 }else{
  const char *api=NULL;int n=0;
  if(optimize(f)&&special){
   n=count(p,args);
   if(n==1){if(named(head,"cons?"))api="consp";else if(named(head,"number?"))api="numberp";else if(named(head,"string?"))api="stringp";}
   else if(n==2){if(named(head,"="))api="eq";else if(named(head,">"))api="gt";else if(named(head,"<"))api="lt";else if(named(head,">="))api="ge";else if(named(head,"<="))api="le";}
  }
  if(api){
   bool borrow=true;
   for(RcValue *q=args;!rc_is_nil(q);q=rest(q))if(is_pair(first(q)))borrow=false;
   Live live=sequence_live(f,args,after);int fn=function_head(f,head,&live,borrow);clear_live(p,&live);
   /* Borrow only lexical owners that remain live afterward. Besides removing
    * copy/release pairs, this preserves exactly the roots seen at the predicate
    * safepoint; last-use owners must still drop before collection. Generic
    * replacements use checked borrowed apply with independent snapshots. */
   bool rooted=true;
   for(RcValue *q=args;!rc_is_nil(q);q=rest(q))
    if(!is_symbol(first(q))||!has(after,binding(f,name(first(q)))))rooted=false;
   if(rooted){
    int i=0;for(RcValue *q=args;!rc_is_nil(q);q=rest(q))argument(f,i++,binding(f,name(first(q))));
   }else{
    int av[2];RcValue *q=args;
    for(int i=0;i<n;i++,q=rest(q)){Live next=sequence_live(f,rest(q),after);av[i]=expr(f,first(q),0,&next);clear_live(p,&next);}
    for(int i=0;i<n;i++)call_argument(f,i,av[i]);
   }
   put(&f->code,"  test_%d=rc_condition_%s_%s(ctx,t[%d],a);\n",id,api,rooted?"borrowed":"owned",fn);drop(f,fn);
   put(&f->code,"  if(test_%d==-2) { rc_error(ctx,\"%s\"); goto %s; }\n  if(test_%d<0) goto %s;\n",id,rhs_boolean?"boolean expected":"condition is not a boolean",f->fail,id,f->fail);drop_dead(f,after);
  }else{
   int value=expr(f,x,0,after);
   put(&f->code,"  if(!rc_is_boolean(t[%d])) { rc_error(ctx,\"%s\"); goto %s; }\n",value,rhs_boolean?"boolean expected":"condition is not a boolean",f->fail);
   put(&f->code,"  test_%d=rc_is_true(t[%d]);\n",id,value);drop(f,value);
  }
 }
 retire_scope(f,mark,-1);return id;
}
static int condition(RcEmitFn *f,RcValue *x,const Live *after){return condition_checked(f,x,after,false);}
static int expr_inner(RcEmitFn *f,RcValue *x,int tail,const Live *after);
static int expr(RcEmitFn *f,RcValue *x,int tail,const Live *after){
 int mark=f->slots,result=expr_inner(f,x,tail,after);retire_scope(f,mark,result);return result;
}
static int expr_inner(RcEmitFn *f,RcValue *x,int tail,const Live *after){
 Program *p=f->p;if(!is_list(x)||rc_is_nil(x))return atom(f,x,tail,after);
 RcValue *head=first(x),*args=rest(x);bool special=is_symbol(head)&&binding(f,name(head))<0;
 if(special&&(named(head,"quote")||named(head,"c-quote"))){
  if(count(p,args)!=1)bad(p,"rc emit: quote arity");
  if(p->options.expression_entry)return runtime_literal(f,first(args));
  int quoted=fresh(f),data=literal(p,first(args));
  put(&f->code,"  t[%d]=rc_literal_%d(ctx);\n",quoted,data);check(f,quoted);return quoted;
 }
 if(optimize(f)&&!tail&&special&&(named(head,"cons")||named(head,"protect"))&&constant_nodes(f,x,false)>=(f->constant_min?f->constant_min:RC_CONSTANT_DATA_MIN)){
  int t=constant_data(f,x,false);drop_dead(f,after);return t;
 }
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
  for(int i=0;i<n;i++){functions[i]=function_head(f,first(nodes[i]),&suffix[i],false);left[i]=expr(f,first(rest(nodes[i])),0,&suffix[i+1]);}
  DoGuard *saved=f->do_guard,*guards=NULL;
  if(optimize(f)&&tail){
   guards=alloc(p,(size_t)n*sizeof(*guards));
   for(int i=0;i<n;i++){guards[i]=(DoGuard){functions[i],f->do_guard};f->do_guard=&guards[i];}
   p->transparent_do_used=true;
  }
  int result=expr(f,end,guards?2:0,after);f->do_guard=saved;forget(p,guards);
  for(int i=n-1;i>=0;i--){int next=fresh(f);
   /* Only a certified identity may forward a pending tail request. Applying
    * even an identity through ordinary apply would drive it recursively. */
   if(optimize(f)&&tail){put(&f->code,"  if(rc_transparent_do(ctx,t[%d])) {\n",functions[i]);drop(f,left[i]);move(f,next,result);put(&f->code,"  } else {\n");}
   call_argument(f,0,left[i]);call_argument(f,1,result);
   if(optimize(f))put(&f->code,"  t[%d]=rc_primitive_call_owned(ctx,t[%d],2,a,%s);\n",next,functions[i],tail==1&&i==0?"true":"false");else put(&f->code,"  t[%d]=%s(ctx,t[%d],2,a);\n",next,call_api(f,tail==1&&i==0),functions[i]);
   if(optimize(f)&&tail)put(&f->code,"  }\n");
   drop(f,functions[i]);if(!optimize(f)){drop(f,left[i]);drop(f,result);}check(f,next);result=next;}
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
  { ModuleDef *d=alloc(p,sizeof(*d));
    *d=(ModuleDef){symbol_index(f,name(first(args))),id,count(p,second(args)),
                   named(first(args),"shen.hashkey")&&exact_hashkey(p,second(args),at(p,args,2)),
                   !p->user_phase,p->module_defs};
    p->module_defs=d;
  }
  symbol_slot(f,t,name(first(args)));check(f,t);
  /* A definition executed at most once per context gets an immortal,
   * capture-free closure: named calls through it need no reference counts.
   * Definitions that may run repeatedly keep ordinary closures, so replaced
   * bindings are still freed. Dynamic expression modules capture their
   * literal vector and may be re-run from the code cache, so they never
   * qualify. */
  bool once=optimize(f)&&f->once&&!p->options.expression_entry;
  if(p->options.expression_entry){
   if(f->max_args<1)f->max_args=1;
   put(&f->code,"  a[0]=rc_capture(self,%d);\n",f->literal_capture);
   if(exact_second_identity(second(args),at(p,args,2),count(p,second(args))))
    put(&f->code,"  t[%d]=%s(ctx,rc_native_%d,1,a);\n",cl,p->options.conservative_ownership?"rc_closure_return_second":"rc_closure_return_second_owned",id);
   else put(&f->code,"  t[%d]=%s(ctx,%d,rc_native_%d,1,a);\n",cl,closure_api(p),count(p,second(args)),id);
  }else if(exact_second_identity(second(args),at(p,args,2),count(p,second(args))))
   put(&f->code,"  t[%d]=%s%s(ctx,rc_native_%d,0,NULL)%s;\n",cl,once?"rc_closure_static(":"",p->options.conservative_ownership?"rc_closure_return_second":"rc_closure_return_second_owned",id,once?")":"");
  else put(&f->code,"  t[%d]=%s%s(ctx,%d,rc_native_%d,0,NULL)%s;\n",cl,once?"rc_closure_static(":"",closure_api(p),count(p,second(args)),id,once?")":"");
  check(f,cl);
  if(p->options.source_metadata&&p->user_phase&&x==p->metadata_root){
   int quoted=fresh(f),data=literal(p,x);
   put(&f->code,"  t[%d]=rc_literal_%d(ctx);\n",quoted,data);check(f,quoted);
   put(&f->code,"  if(!rc_source_definition(ctx,t[%d],t[%d],%d)) goto %s;\n",t,quoted,count(p,second(args)),f->fail);drop(f,quoted);
  }
  put(&f->code,"  if(!%s(ctx,t[%d],t[%d])) goto %s;\n",p->options.module_entry?"rc_module_define":"rc_function_set",t,cl,f->fail);drop(f,cl);p->defuns++;drop_dead(f,after);return t;
 }
 int n=count(p,args);
 /* Atom construction can allocate, but never invokes callbacks or a safepoint.
  * Therefore the selected function's symbol binding roots it until leaf-helper
  * entry. Generic helper fallback roots the callee before running user code.
  * Module direct bodies and reuse helpers require a caller-owned callee. */
 bool borrow_fn=optimize(f)&&special&&leaf_primitive(head)&&!named(head,"cons")&&(!named(head,"tl")||!f->recycle);
 if(borrow_fn)for(RcValue *q=args;!rc_is_nil(q);q=rest(q)){
  RcType type=rc_type(first(q));
  if(type!=RC_NIL&&type!=RC_SYMBOL&&type!=RC_NUMBER&&type!=RC_STRING){borrow_fn=false;break;}
 }
 Live head_after=sequence_live(f,args,after);int fn=function_head(f,head,&head_after,borrow_fn);clear_live(p,&head_after);
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
  put(&f->code,"  if(t[%d]==self && rc_arity(self)==%d",fn,n);
  if(tail==2)for(DoGuard *g=f->do_guard;g;g=g->outer)put(&f->code," && rc_transparent_do(ctx,t[%d])",g->slot);
  put(&f->code,") {\n");
  for(int i=0;i<n;i++)call_argument(f,i,av[i]);
  put(&f->code,"  goto recur;\n  }\n");
 }
 /* A transparent do chain leaves only result forwarding (and any boolean
  * completion guard). Use a runtime tail request when every saved callee is
  * still the certified identity; arbitrary replacements stay eager. */
 bool guarded_tail=optimize(f)&&tail==2&&f->do_guard;
 if(guarded_tail){
  put(&f->code,"  if(");bool first_guard=true;
  for(DoGuard *g=f->do_guard;g;g=g->outer){put(&f->code,"%src_transparent_do(ctx,t[%d])",first_guard?"":" && ",g->slot);first_guard=false;}
  put(&f->code,") {\n");for(int i=0;i<n;i++)call_argument(f,i,av[i]);
  put(&f->code,"  t[%d]=rc_tailcall_owned_args(ctx,t[%d],%d,%s);\n  } else {\n",result,fn,n,n?"a":"NULL");
 }
 if(project){put(&f->code,"  { RcValue *candidate=t[%d]; t[%d]=NULL; t[%d]=rc_tail_call_reuse(ctx,t[%d],candidate,&t[%d]); }\n",av[0],av[0],result,fn,project->token);}
 else if(recycle.binding>=0){
  put(&f->code,"  { RcValue *candidate=t[%d] ? t[%d] : t[%d]; t[%d]=NULL; t[%d]=NULL; t[%d]=rc_cons_call_reuse(ctx,t[%d],candidate,t[%d],t[%d],%s); }\n",recycle.token,recycle.token,recycle.binding,recycle.token,recycle.binding,result,fn,av[0],av[1],tail==1?"true":"false");
  drop(f,av[0]);drop(f,av[1]);
 }else{
  for(int i=0;i<n;i++)call_argument(f,i,av[i]);
  if(optimize(f)&&special&&tiny_leaf_api(head,n))
   put(&f->code,"  t[%d]=%s(ctx,t[%d],a,%s);\n",result,tiny_leaf_api(head,n),fn,tail==1?"true":"false");
  else if(optimize(f)&&special&&leaf_primitive(head))
   put(&f->code,"  t[%d]=rc_primitive_call_owned(ctx,t[%d],%d,%s,%s);\n",result,fn,n,n?"a":"NULL",tail==1?"true":"false");
  else if(optimize(f)&&special&&tail!=1)
   put(&f->code,"  t[%d]=rc_module_call_%zu(ctx,t[%d],%d,%s);\n",result,module_call(f,name(head)),fn,n,n?"a":"NULL");
  else {
   /* A tail call to an immortal definition of this module becomes a C tail
    * call: no heap request, no trampoline bounce. The frame is released
    * first, as the request path also does before the callee runs, and the
    * collector's safepoint still runs once per bounce. Replaced, captured or
    * partial callees and other modules keep the runtime request. */
   if(optimize(f)&&special&&tail==1&&!f->boolean_tail&&n<=RC_MUSTTAIL_ARGS&&!p->options.expression_entry){
    size_t k=module_call_kind(f,name(head),true);f->musttail_used=true;
    put(&f->code,"#ifdef RC_MUSTTAIL\n  if(rc_module_tail_ok_%zu(t[%d],%d)) {\n    RcValue *callee=t[%d]; t[%d]=NULL;\n",k,fn,n,fn,fn);
    for(int i=0;i<n;i++)put(&f->code,"    rc_tail_argv[%d]=a[%d];\n",i,i);
    put(&f->code,"    rc_tail_release_frame(t,rc_frame_slots,rc_frame_heap);\n    rc_safepoint(ctx);\n    RC_MUSTTAIL return rc_module_tail_%zu(ctx,callee,rc_tail_argv);\n  }\n#endif\n",k);
   }
   put(&f->code,"  t[%d]=%s(ctx,t[%d],%d,%s);\n",result,call_api(f,tail),fn,n,n?"a":"NULL");
  }
  if(!optimize(f))for(int i=0;i<n;i++)drop(f,av[i]);
 }
 if(guarded_tail)put(&f->code,"  }\n");
 if(recycle.binding>=0)f->recycle=recycle.outer;
 drop(f,fn);forget(p,av);check(f,result);drop_dead(f,after);return result;
}
/* Only this complete pinned kernel definition is eligible. No spelling-only
 * replacement: modified source, parameters, error text or clauses stay native. */
/* Kernel templates are compared up to renaming of their bound variables, so
 * a regenerated kernel with different V/W/Z numbering still certifies while
 * any change of structure, constants or free names does not. Binders are the
 * defun parameters and KL let/lambda variables. */
#define ALPHA_BINDERS 64
typedef struct { const char *a[ALPHA_BINDERS],*b[ALPHA_BINDERS]; int n; } AlphaScope;
static int alpha_index(const char *const *names,int n,const char *s) {
 for(int i=n-1;i>=0;i--) if(!strcmp(names[i],s)) return i;
 return -1;
}
static bool alpha_equal(RcValue *a,RcValue *b,AlphaScope *scope) {
 if(is_symbol(a)||is_symbol(b)) {
  if(!is_symbol(a)||!is_symbol(b)) return false;
  int i=alpha_index(scope->a,scope->n,name(a)),j=alpha_index(scope->b,scope->n,name(b));
  return i==j&&(i>=0||!strcmp(name(a),name(b)));
 }
 if(!is_pair(a)||!is_pair(b)) return !is_pair(a)&&!is_pair(b)&&rc_equal(a,b);
 bool let=named(first(a),"let")&&named(first(b),"let");
 bool lambda=named(first(a),"lambda")&&named(first(b),"lambda");
 if(let||lambda) {
  /* (let X V B) binds X in B only; (lambda X B) binds X in B. */
  RcValue *xa=rest(a),*xb=rest(b);
  if(!is_pair(xa)||!is_pair(xb)||!is_symbol(first(xa))||!is_symbol(first(xb))) return false;
  RcValue *va=rest(xa),*vb=rest(xb);
  if(let) {
   if(!is_pair(va)||!is_pair(vb)||!alpha_equal(first(va),first(vb),scope)) return false;
   va=rest(va); vb=rest(vb);
  }
  if(!is_pair(va)||!is_pair(vb)||!rc_is_nil(rest(va))||!rc_is_nil(rest(vb))||scope->n==ALPHA_BINDERS) return false;
  scope->a[scope->n]=name(first(xa)); scope->b[scope->n]=name(first(xb)); scope->n++;
  bool equal=alpha_equal(first(va),first(vb),scope);
  scope->n--; return equal;
 }
 for(;is_pair(a)&&is_pair(b);a=rest(a),b=rest(b)) if(!alpha_equal(first(a),first(b),scope)) return false;
 return alpha_equal(a,b,scope);
}
/* Compare a candidate's parameters and body with (defun NAME PARAMS BODY). */
static bool kernel_template(Program *p,RcValue *params,RcValue *body,const char *text,const char *label) {
 RcValue *expected=rc_read(p->ctx,&text);
 if(!expected) bad(p,label);
 RcValue *tparams=second(rest(expected)),*tbody=at(p,rest(expected),2);
 AlphaScope scope={.n=0}; bool equal=count(p,params)==count(p,tparams);
 for(RcValue *x=params,*y=tparams;equal&&is_pair(x);x=rest(x),y=rest(y)) {
  if(!is_symbol(first(x))||scope.n==ALPHA_BINDERS) { equal=false; break; }
  scope.a[scope.n]=name(first(x)); scope.b[scope.n]=name(first(y)); scope.n++;
 }
 equal=equal&&alpha_equal(body,tbody,&scope);
 rc_release(expected);
 if(rc_has_error(p->ctx)) bad(p,label);
 return equal;
}
static bool exact_assoc(RcEmitFn *f,RcValue *params,RcValue *body) {
 if(!optimize(f)||count(f->p,params)!=2)return false;
 return kernel_template(f->p,params,body,"(defun assoc (V3928 V3929) (cond ((= () V3929) ()) ((and (cons? V3929) (and (cons? (hd V3929)) (= V3928 (hd (hd V3929))))) (hd V3929)) ((cons? V3929) (assoc V3928 (tl V3929))) (true (simple-error \"attempt to search a non-list with assoc\n\"))))","assoc template comparison failed");
}
static bool exact_prolog(RcEmitFn *f,RcValue *params,RcValue *body,bool lazy) {
 if(!optimize(f))return false;
 const char *text=lazy
  ? "(defun shen.lazyderef (V2111 V2112) (if (shen.pvar? V2111) (let W2113 (<-address V2112 (<-address V2111 1)) (if (= W2113 shen.-null-) V2111 (shen.lazyderef W2113 V2112))) V2111))"
  : "(defun shen.pvar? (V2109) (and (absvector? V2109) (= (trap-error (<-address V2109 0) (lambda Z2110 shen.not-pvar)) shen.pvar)))";
 return kernel_template(f->p,params,body,text,"Prolog template comparison failed");
}
static void finish_function(RcEmitFn *f,int id,int arity,int r){
 Program *p=f->p;
 int chunks=function_chunks(f,id);
 if(optimize(f))put(&p->functions,"static RcValue *rc_native_body_%d(RcContext *ctx,RcValue *self,RcValue **argv) {\n  (void)self; (void)argv;\n  if(!rc_stack_check(ctx)) return NULL;\n  RcValue *result=NULL;\n",id);
 else put(&p->functions,"static RcValue *rc_native_%d(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *argv) {\n  (void)self; (void)argv;\n  if(argc!=%d) return rc_error(ctx,\"native arity mismatch\");\n  if(!rc_stack_check(ctx)) return NULL;\n  RcValue *result=NULL;\n",id,arity);
 symbol_frame(&p->functions,f);
 if(f->assoc_fast)put(&p->functions,"  { bool handled=false; RcValue *deps[]={symbols[%zu],symbols[%zu],symbols[%zu],symbols[%zu],symbols[%zu]};\n    RcValue *fast=rc_assoc_try_owned(ctx,self,argv,deps,&handled); if(handled) return fast; }\n",f->assoc_symbols[0],f->assoc_symbols[1],f->assoc_symbols[2],f->assoc_symbols[3],f->assoc_symbols[4]);
 if(f->pvar_fast)put(&p->functions,"  { bool handled=false; RcValue *deps[]={symbols[%zu],symbols[%zu],symbols[%zu],symbols[%zu]};\n    RcValue *fast=rc_pvar_try_owned(ctx,self,argv,deps,&handled); if(handled) return fast; }\n",f->prolog_symbols[0],f->prolog_symbols[1],f->prolog_symbols[2],f->prolog_symbols[3]);
 if(f->lazyderef_fast)put(&p->functions,"  { bool handled=false; RcValue *deps[]={symbols[%zu],symbols[%zu],symbols[%zu],symbols[%zu],symbols[%zu],symbols[%zu],symbols[%zu]};\n    RcValue *fast=rc_lazyderef_try_owned(ctx,self,argv,deps,rc_certified_pvar,&handled); if(handled) return fast; }\n",f->prolog_symbols[0],f->prolog_symbols[1],f->prolog_symbols[2],f->prolog_symbols[3],f->prolog_symbols[4],f->prolog_symbols[5],f->prolog_symbols[6]);
 if(f->frame_slots>64)put(&p->functions,"  RcValue **t=calloc(%d,sizeof(*t));\n  if(!t) return rc_error(ctx,\"native frame allocation failed\");\n  RcValue **a=t+%d; (void)a;\n",f->frame_slots+f->max_args,f->frame_slots);
 else put(&p->functions,"  RcValue *t[%d]={0}; RcValue *a[%d]; (void)a;\n",f->frame_slots?f->frame_slots:1,f->max_args?f->max_args:1);
 if(f->musttail_used)put(&p->functions,"  enum { rc_frame_slots=%d, rc_frame_heap=%d };\n",f->frame_slots,f->frame_slots>64);
 /* Atom-only argument evaluation prevents overlapping borrowed callees in
  * this activation, so all logical borrowed slots share one scalar. */
 if(f->borrowed_fn_used)put(&p->functions,"  RcValue *borrowed_fn=NULL; (void)borrowed_fn;\n");
 if(f->self_loop)put(&p->functions,"entry:;\n");
 if(chunks)for(int part=0;part<chunks;part++)
  put(&p->functions,"  if(!rc_native_chunk_%d_%d(ctx,self,argv,t,a,%s)) goto cleanup;\n",id,part,f->symbol_used?"symbols":"NULL");
 else frame_code(&p->functions,f);
 r=f->physical[r];
 put(&p->functions,"  result=t[%d]; t[%d]=NULL;\n",r,r);
 if(optimize(f)) {
  /* Inductive owned-expression contract: successful emission leaves only its
   * result owner; after=NULL releases dead lexical bindings. Moving that final
   * result therefore leaves no frame owners. Error and recur edges retain the
   * complete scan. Qualification can check this summary at every invocation. */
  put(&p->functions,"#ifdef SHEN_RC_VERIFY_OWNERSHIP\n  for(size_t i=0;i<%d;i++) if(t[i]) { fprintf(stderr,\"native %d: live slot %%zu at successful exit\\n\",i); abort(); }\n#endif\n",f->frame_slots,id);
  if(f->frame_slots>64)put(&p->functions,"  free(t);\n");
  put(&p->functions,"  return result;\n");
 }
 put(&p->functions,"cleanup:\n  for(size_t i=0;i<%d;i++) if(t[i]) rc_release(t[i]);\n",f->frame_slots);
 if(f->borrowed_fn_used)put(&p->functions,"  borrowed_fn=NULL;\n");
 if(f->frame_slots>64)put(&p->functions,"  free(t);\n");put(&p->functions,"  return result;\n");
 if(f->self_loop){
  /* a owns the staged arguments while the previous iteration's lexical and
   * temporary owners are dropped. argv belongs to the active runtime call,
   * so it is distinct from this callback's frame and scratch storage. */
  put(&p->functions,"recur:\n  for(size_t i=0;i<%d;i++) if(t[i]) { rc_release(t[i]); t[i]=NULL; }\n",f->frame_slots);
  if(f->borrowed_fn_used)put(&p->functions,"  borrowed_fn=NULL;\n");
  for(int i=0;i<arity;i++)put(&p->functions,"  argv[%d]=a[%d]; a[%d]=NULL;\n",i,i,i);
  put(&p->functions,"  rc_safepoint(ctx);\n  goto entry;\n");
 }
 put(&p->functions,"}\n");
 if(optimize(f))put(&p->functions,"static RcValue *rc_native_%d(RcContext *ctx,RcValue *self,size_t argc,RcValue **argv) {\n  if(argc!=%d) return rc_error(ctx,\"native arity mismatch\");\n  return rc_native_body_%d(ctx,self,argv);\n}\n",id,arity,id);
}
/* Rough count of cons/protect occurrences in a body. Eligibility itself is
 * still decided per tree, with lexical scope, by constant_nodes. */
static long constructors(RcValue *x){
 long n=0;
 for(;is_pair(x);x=rest(x)){RcValue *y=first(x);if(named(y,"cons")||named(y,"protect"))n++;else n+=constructors(y);}
 return n;
}
static int compile(Program *p,RcValue *params,RcValue *body,Binding *caps,int ncaps,int freeze){
 /* A top-level body runs once per bootstrap and cannot loop: it has no name
  * to recurse through. Nested lambdas and definitions are compiled below with
  * the flag cleared. */
 RcEmitFn f={0};f.once=p->top_body;p->top_body=false;f.p=p;f.code.owner=p;
 if(constructors(body)>=RC_CONSTANT_DATA_BULK)f.constant_min=RC_CONSTANT_DATA_BULK_MIN;int id=p->next++,arity=freeze==1?0:(freeze==2?1:count(p,params));f.arity=arity;f.literal_capture=ncaps;snprintf(f.fail,sizeof f.fail,"cleanup");
 if(!ncaps&&!freeze&&exact_assoc(&f,params,body)) {
  const char *deps[]={"assoc","=","cons?","hd","tl"};f.assoc_fast=true;
  for(size_t i=0;i<5;i++)f.assoc_symbols[i]=symbol_index(&f,deps[i]);
 }
 if(!ncaps&&!freeze&&count(p,params)==1&&exact_prolog(&f,params,body,false)) {
  const char *deps[]={"absvector?","<-address","=","shen.pvar"};f.pvar_fast=true;
  for(size_t i=0;i<4;i++)f.prolog_symbols[i]=symbol_index(&f,deps[i]);
  put(&p->pvar_identities," || rc_compiled_call_matches(fn,rc_native_%d,1)",id);
 }
 if(!ncaps&&!freeze&&count(p,params)==2&&exact_prolog(&f,params,body,true)) {
  const char *deps[]={"shen.lazyderef","shen.pvar?","absvector?","<-address","=","shen.pvar","shen.-null-"};f.lazyderef_fast=true;p->lazyderef_used=true;
  for(size_t i=0;i<7;i++)f.prolog_symbols[i]=symbol_index(&f,deps[i]);
 }
 /* Certify the exact effect-free identity body, independent of its name.
  * The generated guard also rejects bound/partially applied closures. */
 if(optimize(&f)&&!ncaps&&!freeze&&exact_second_identity(params,body,arity))
  put(&p->transparent_do," || rc_closure_is_owned_native(fn,rc_native_%d)",id);
 for(int i=0;i<ncaps;i++){int s=fresh(&f);bind(&f,caps[i].name,s);put(&f.code,"  t[%d]=rc_retain(rc_capture(self,%d));\n",s,i);}
 for(int i=0;i<arity;i++,params=freeze==2?rc_nil():rest(params)){RcValue *param=freeze==2?params:first(params);if(!is_symbol(param))bad(p,"rc emit: function parameter");for(int j=ncaps;j<f.nb;j++)if(!strcmp(f.bind[j].name,name(param)))bad(p,"rc emit: duplicate function parameter");int s=fresh(&f);bind(&f,name(param),s);
  if(optimize(&f))put(&f.code,"  t[%d]=argv[%d]; argv[%d]=NULL;\n",s,i,i);else put(&f.code,"  t[%d]=rc_retain(argv[%d]);\n",s,i);}
 Live needed=uses(&f,body);drop_dead(&f,&needed);clear_live(p,&needed);int r=expr(&f,body,1,NULL);
 finish_function(&f,id,arity,r);
 discard(f.code.p);discard(f.bind);return id;
}
/* Large constant trees, such as the standard library's stored sources and
 * type signatures, become a static table run by one small loop instead of one
 * straight-line statement per cell. In expression mode the table replays the
 * emitted call order exactly: each (cons A B) or (protect A) resolves its
 * callee's current binding before its arguments, arguments run left to right,
 * and the same owned-call APIs apply them, so redefinition stays observable.
 * Raw mode builds reader data with rc_cons_owned, like literal_node. Every
 * node allocates afresh: no sharing beyond what the straight-line code had. */
enum { RC_CONST_NIL, RC_CONST_SYMBOL, RC_CONST_STRING, RC_CONST_INTEGER, RC_CONST_REAL,
       RC_CONST_FUNCTION, RC_CONST_CONS, RC_CONST_CALL1, RC_CONST_PAIR };
/* Node count of an eligible tree, or -1. Expression mode admits unbound
 * symbols, nil, numbers, strings, and canonical-shape cons/protect calls. */
static long constant_nodes(RcEmitFn *f,RcValue *x,bool raw){
 if(rc_is_nil(x)||rc_type(x)==RC_NUMBER||rc_type(x)==RC_STRING)return 1;
 if(is_symbol(x))return raw||binding(f,name(x))<0?1:-1;
 if(!is_pair(x))return -1;
 long n=1;RcValue *q=x;
 if(!raw){
  RcValue *head=first(x);int argc=named(head,"cons")?2:named(head,"protect")?1:0;
  if(!argc||binding(f,name(head))>=0)return -1;
  q=rest(x);for(int i=0;i<argc;i++,q=rest(q)){if(!is_pair(q))return -1;long k=constant_nodes(f,first(q),false);if(k<0)return -1;n+=k;}
  return rc_is_nil(q)?n:-1;
 }
 long h=constant_nodes(f,first(x),true),t=h<0?-1:constant_nodes(f,rest(x),true);
 return h<0||t<0?-1:n+h+t;
}
typedef struct { Buf code,strings,integers,reals; size_t words,nstrings,nintegers,nreals,sp,depth; bool symbols; } ConstTable;
static void const_word(Program *p,ConstTable *c,unsigned op,size_t operand,int effect){
 if(operand>(UINT32_MAX>>4))bad(p,"constant data table overflow");
 put(&c->code,"%s%" PRIu32 "u",c->words?(c->words%16?",":",\n "):" ",(uint32_t)(op|(uint32_t)operand<<4));c->words++;
 c->sp=(size_t)((long)c->sp+effect);if(c->sp>c->depth)c->depth=c->sp;
}
static void const_symbol(RcEmitFn *f,ConstTable *c,unsigned op,const char *s){
 c->symbols=true;const_word(f->p,c,op,symbol_index(f,s),1);
}
static void const_node(RcEmitFn *f,ConstTable *c,RcValue *x,bool raw){
 Program *p=f->p;
 if(rc_is_nil(x))const_word(p,c,RC_CONST_NIL,0,1);
 else if(rc_type(x)==RC_NUMBER&&rc_number_is_exact_integer(x)){
  int64_t n=rc_integer_value(x);
  if(n==INT64_MIN)put(&c->integers,"%sINT64_MIN",c->nintegers?",":"");
  else put(&c->integers,"%sINT64_C(%" PRId64 ")",c->nintegers?",":"",n);
  const_word(p,c,RC_CONST_INTEGER,c->nintegers++,1);
 }else if(rc_type(x)==RC_NUMBER){double n=rc_number_value(x);
  if(isnan(n))put(&c->reals,"%sNAN",c->nreals?",":"");
  else if(isinf(n))put(&c->reals,"%s%sINFINITY",c->nreals?",":"",n<0?"-":"");
  else put(&c->reals,"%s%a",c->nreals?",":"",n);
  const_word(p,c,RC_CONST_REAL,c->nreals++,1);
 }else if(rc_type(x)==RC_STRING){
  put(&c->strings,"%s\n {",c->nstrings?",":"");quoted_n(&c->strings,rc_text(x),rc_text_length(x));put(&c->strings,",%zu}",rc_text_length(x));
  const_word(p,c,RC_CONST_STRING,c->nstrings++,1);
 }else if(is_symbol(x))const_symbol(f,c,RC_CONST_SYMBOL,name(x));
 else if(raw){const_node(f,c,first(x),true);const_node(f,c,rest(x),true);const_word(p,c,RC_CONST_PAIR,0,-1);}
 else{
  bool cons=named(first(x),"cons");const_symbol(f,c,RC_CONST_FUNCTION,name(first(x)));
  for(RcValue *q=rest(x);!rc_is_nil(q);q=rest(q))const_node(f,c,first(q),false);
  const_word(p,c,cons?RC_CONST_CONS:RC_CONST_CALL1,0,cons?-2:-1);
 }
}
static int constant_data(RcEmitFn *f,RcValue *x,bool raw){
 Program *p=f->p;ConstTable c={0};c.code.owner=c.strings.owner=c.integers.owner=c.reals.owner=p;
 const_node(f,&c,x,raw);if(c.sp!=1)bad(p,"constant data stack imbalance");
 int id=p->next_constant++;
 put(&p->constants,"static const uint32_t rc_constant_code_%d[]={\n%s\n};\n",id,c.code.p);
 if(c.nstrings)put(&p->constants,"static const RcConstantString rc_constant_strings_%d[]={%s\n};\n",id,c.strings.p);
 if(c.nintegers)put(&p->constants,"static const int64_t rc_constant_integers_%d[]={%s};\n",id,c.integers.p);
 if(c.nreals)put(&p->constants,"static const double rc_constant_reals_%d[]={%s};\n",id,c.reals.p);
 put(&p->constants,"static const RcConstantData rc_constant_%d={rc_constant_code_%d,%zu,%zu,",id,id,c.words,c.depth);
 if(c.nstrings)put(&p->constants,"rc_constant_strings_%d,",id);else put(&p->constants,"NULL,");
 if(c.nintegers)put(&p->constants,"rc_constant_integers_%d,",id);else put(&p->constants,"NULL,");
 if(c.nreals)put(&p->constants,"rc_constant_reals_%d};\n",id);else put(&p->constants,"NULL};\n");
 discard(c.code.p);discard(c.strings.p);discard(c.integers.p);discard(c.reals.p);
 int t=fresh(f);put(&f->code,"  t[%d]=rc_constant_data(ctx,%s,&rc_constant_%d);\n",t,c.symbols?"symbols":"NULL",id);check(f,t);return t;
}
static void constant_helpers(FILE *out){
 fprintf(out,"typedef struct { const char *text; size_t length; } RcConstantString;\n"
       "typedef struct { const uint32_t *code; size_t length,depth; const RcConstantString *strings; const int64_t *integers; const double *reals; } RcConstantData;\n"
       "/* Runs one constant table. Every stack entry is an owned value; calls\n"
       " * consume their argument slots on every path, the callee is released\n"
       " * after the call, and failure releases whatever remains. */\n"
       "static RcValue *rc_constant_data(RcContext *ctx,RcValue *const *symbols,const RcConstantData *data) {\n"
       "  RcValue *small[64],**stack=data->depth<=64?small:calloc(data->depth,sizeof(*stack)),*result=NULL;\n"
       "  if(!stack) return rc_error(ctx,\"constant data allocation failed\");\n"
       "  size_t sp=0;\n"
       "  for(size_t pc=0;pc<data->length;pc++) {\n"
       "    uint32_t op=data->code[pc]&15u,x=data->code[pc]>>4; RcValue *v=NULL,*fn;\n"
       "    switch(op) {\n"
       "    case %d: v=rc_nil(); break;\n"
       "    case %d: v=rc_retain(symbols[x]); break;\n"
       "    case %d: v=rc_string_n(ctx,data->strings[x].text,data->strings[x].length); break;\n"
       "    case %d: v=rc_integer(ctx,data->integers[x]); break;\n"
       "    case %d: v=rc_number(ctx,data->reals[x]); break;\n"
       "    case %d: v=rc_function_get(ctx,symbols[x]); break;\n"
       "    case %d: sp-=3; fn=stack[sp]; v=rc_primitive_cons_owned(ctx,fn,stack+sp+1,false); rc_release(fn); break;\n"
       "    case %d: sp-=2; fn=stack[sp]; v=rc_apply_rooted_owned_args(ctx,fn,1,stack+sp+1); rc_release(fn); break;\n"
       "    case %d: sp-=2; v=rc_cons_owned(ctx,stack[sp],stack[sp+1]); break;\n"
       "    default: v=rc_error(ctx,\"invalid constant data\"); break;\n"
       "    }\n"
       "    if(!v) goto cleanup;\n"
       "    stack[sp++]=v;\n"
       "  }\n"
       "  if(sp==1) { result=stack[0]; sp=0; } else rc_error(ctx,\"invalid constant data\");\n"
       "cleanup:\n"
       "  while(sp) rc_release(stack[--sp]);\n"
       "  if(stack!=small) free(stack);\n"
       "  return result;\n}\n",
       RC_CONST_NIL,RC_CONST_SYMBOL,RC_CONST_STRING,RC_CONST_INTEGER,RC_CONST_REAL,
       RC_CONST_FUNCTION,RC_CONST_CONS,RC_CONST_CALL1,RC_CONST_PAIR);
}
/* Construct source metadata as values, never by parsing/evaluating user code.
 * The helper owns each temporary and transfers only its completed AST result. */
static int literal_node(RcEmitFn *f,RcValue *x){
 if(!is_pair(x))return atom(f,x,0,NULL);
 if(constant_nodes(f,x,true)>=RC_CONSTANT_DATA_MIN)return constant_data(f,x,true);
 int h=literal_node(f,first(x)),t=literal_node(f,rest(x)),out=fresh(f);
 put(&f->code,"  t[%d]=rc_cons(ctx,t[%d],t[%d]);\n",out,h,t);drop(f,h);drop(f,t);check(f,out);return out;
}
static int literal(Program *p,RcValue *x){
 RcEmitFn f={0};f.p=p;f.code.owner=p;snprintf(f.fail,sizeof f.fail,"cleanup");
 int id=p->next_literal++,result=literal_node(&f,x);
 put(&p->literals,"static RcValue *rc_literal_%d(RcContext *ctx) {\n  if(!rc_stack_check(ctx)) return NULL;\n  RcValue *result=NULL;\n",id);
 symbol_frame(&p->literals,&f);
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
  p->top_body=true;return compile(p,rc_nil(),first(args),NULL,0,0);
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
 }else if(named(kind,"unproperty")){
  if(count(p,args)!=2)bad(p,"invalid source property removal event");
  int fn=event_function(&f,"unput"),av[3];RcValue *q=args;
  for(int i=0;i<2;i++,q=rest(q))av[i]=literal_node(&f,first(q));
  int property_name=fresh(&f);symbol_slot(&f,property_name,"*property-vector*");check(&f,property_name);
  av[2]=fresh(&f);put(&f.code,"  t[%d]=rc_global_get(ctx,t[%d]);\n",av[2],property_name);drop(&f,property_name);check(&f,av[2]);
  result=event_call(&f,fn,av,3);
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
  p->options=*options; p->ctx=ctx; p->functions.owner=p; p->literals.owner=p; p->constants.owner=p; p->transparent_do.owner=p; p->pvar_identities.owner=p;
  if(setjmp(p->failure)) { clear_arena(p); free(p); return false; }
  int expression_id=-1;
  Buf installs={.owner=p},kt={.owner=p},ut={.owner=p},events={.owner=p};
  if(options->source_events&&!options->source_metadata)bad(p,"source reader events require source metadata mode");
  if(options->expression_entry){
    if(!identifier(options->expression_entry)||nkernel!=1||nuser||options->module_entry||options->source_events||options->source_metadata||options->emit_main||options->init_name||!options->literal_bindings||!options->literal_count)
      bad(p,"invalid expression module options");
    *options->literal_bindings=NULL;*options->literal_count=0;
  }
  if(options->module_entry){
    if(!identifier(options->module_entry)||nuser||options->source_events||options->source_metadata||options->emit_main||options->init_name)
      bad(p,"invalid native module options");
    for(size_t i=0;i<nkernel;i++)if(!kernel[i]||!is_pair(kernel[i])||!named(first(kernel[i]),"defun"))bad(p,"native module forms must be defuns");
  }
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
      p->top_body=true;int id=compile(p,options->function_parameters?options->function_parameters:rc_nil(),forms[i],NULL,0,0);
      if(options->expression_entry)expression_id=id;
      int def=is_pair(forms[i])&&named(first(forms[i]),"defun");
      Buf *where=phase?&ut:(def?&installs:&kt);
      put(where,"  if(!run_top(ctx,rc_native_%d,%d)) return false;\n",id,phase&&!def);
      if(!def) p->tops++;
    }
  }
  fputs("/* Native KLambda -> independent reference-counted runtime.\n"
        " * Owned slots, explicit drops, and cleanup on every return path. */\n"
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <stdint.h>\n#include <math.h>\n#include \"rc.h\"\n#include \"primitives.h\"\n#include \"eval.h\"\n#define RC_LAYOUT_FAST 1\n#include \"rc_layout.h\"\n",out);
  if(options->module_entry)fputs("static bool (*rc_module_define)(RcContext*,RcValue*,RcValue*);\n",out);
  if(options->expression_entry)fprintf(out,"#define RC_LITERAL_SYMBOL_OFFSET %zu\n",p->nruntime_literals);
  if(p->nsymbols&&!options->expression_entry) {
    Buf names={.owner=p};
    for(size_t i=0;i<p->nsymbols;i++){if(i)put(&names,",\n");quoted(&names,p->symbols[i]);}
    fprintf(out,"static const char *const rc_module_symbol_names[]={\n%s\n};\nstatic const RcSymbolTable rc_module_symbols={rc_module_symbol_names,%zu};\n",names.p,p->nsymbols);
  }
  if(p->drop_range_emitted)fputs("static void rc_drop_range(RcValue **slots,size_t first,size_t end) {\n"
                            "  for(size_t i=first;i<end;i++) { if(slots[i]) rc_release(slots[i]); slots[i]=NULL; }\n}\n",out);
  for(int i=0;i<p->next;i++) fprintf(out,"static RcValue *rc_native_%d(RcContext*,RcValue*,size_t,RcValue *%s*);\n",i,options->conservative_ownership?"const":"");
  if(!options->conservative_ownership){
   for(int i=0;i<p->next;i++)fprintf(out,"static RcValue *rc_native_body_%d(RcContext*,RcValue*,RcValue**);\n",i);
   /* The name only selects possible module callbacks. The snapshotted closure
    * decides every call, including redefinitions during argument evaluation. */
   bool any_tail=false;
   for(ModuleCall *c=p->module_calls;c;c=c->next)any_tail|=c->tail;
   if(any_tail){
    fputs("#if defined(__has_attribute)\n#if __has_attribute(musttail)\n#define RC_MUSTTAIL __attribute__((musttail))\n#endif\n#endif\n",out);
    fprintf(out,"#ifdef RC_MUSTTAIL\nstatic _Thread_local RcValue *rc_tail_argv[%d];\n",RC_MUSTTAIL_ARGS);
    fputs("static void rc_tail_release_frame(RcValue **t,size_t n,int heap) {\n  for(size_t i=0;i<n;i++) if(t[i]) { rc_release(t[i]); t[i]=NULL; }\n  if(heap) free(t);\n}\n",out);
   }
   for(ModuleCall *c=p->module_calls;c;c=c->next)if(c->tail){
    /* Only an immortal callee may outlive the caller's frame: nothing else
     * roots it once that frame is gone. */
    fprintf(out,"static bool rc_module_tail_ok_%zu(RcValue *fn,int argc) {\n  (void)fn; (void)argc;\n  return rc_refcount(fn)==SIZE_MAX && (false",c->symbol);
    for(ModuleDef *d=p->module_defs;d;d=d->next)if(d->symbol==c->symbol)
     fprintf(out," || (argc==%d && rc_compiled_call_matches(fn,rc_native_%d,%d))",d->arity,d->id,d->arity);
    fputs(");\n}\n",out);
    fprintf(out,"static RcValue *rc_module_tail_%zu(RcContext *ctx,RcValue *fn,RcValue **argv) {\n  (void)fn; (void)argv;\n",c->symbol);
    for(ModuleDef *d=p->module_defs;d;d=d->next)if(d->symbol==c->symbol)
     fprintf(out,"  if(rc_arity(fn)==%d && rc_compiled_call_matches(fn,rc_native_%d,%d)) RC_MUSTTAIL return rc_native_body_%d(ctx,fn,argv);\n",d->arity,d->id,d->arity,d->id);
    fputs("  return rc_error(ctx,\"native tail call target changed\");\n}\n",out);
   }
   if(any_tail)fputs("#endif\n",out);
   for(ModuleCall *c=p->module_calls;c;c=c->next)if(c->call){
    bool first=true;
    fprintf(out,"static RcValue *rc_module_call_%zu(RcContext *ctx,RcValue *fn,size_t argc,RcValue **argv) {\n",c->symbol);
    for(ModuleDef *d=p->module_defs;d;d=d->next)if(d->symbol==c->symbol){
     if(first)fputs("  RcValue *result;\n",out);
     fprintf(out,"  %sif(argc==%d && rc_compiled_call_matches(fn,rc_native_%d,%d)) result=rc_native_body_%d(ctx,fn,argv);\n",first?"":"else ",d->arity,d->id,d->arity,d->id);first=false;
    }
    fprintf(out,"  %sreturn rc_apply_rooted_owned_args(ctx,fn,argc,argv);\n",first?"":"else ");
    if(!first)fputs("#ifdef SHEN_RC_VERIFY_OWNERSHIP\n  if(result) for(size_t i=0;i<argc;i++) if(argv[i]) { fputs(\"compiled call left an owned argument\\n\",stderr); abort(); }\n#endif\n  return rc_compiled_call_complete(ctx,result,result?0:argc,argv);\n",out);
    fputs("}\n",out);
   }
  }
  if(p->lazyderef_used&&p->pvar_identities.n)fprintf(out,"static bool rc_certified_pvar(RcValue *fn) { return false%s; }\n",p->pvar_identities.p);
  else if(p->lazyderef_used)fputs("static bool rc_certified_pvar(RcValue *fn) { (void)fn; return false; }\n",out);
  if(p->transparent_do_used)fprintf(out,"static bool rc_transparent_do(RcContext *ctx,RcValue *fn) {\n  return rc_arity(fn)==2 && (rc_closure_returns_second(ctx,fn) || rc_primitive_is_do(fn)%s);\n}\n",p->transparent_do.p?p->transparent_do.p:"");
  if(p->next_constant) { constant_helpers(out); fputs(p->constants.p,out); }
  if(p->next_literal) metadata_helpers(out);
  if(p->literals.p) fputs(p->literals.p,out);
  if(p->functions.p) fputs(p->functions.p,out);
  if(options->expression_entry){
    int arity=options->function_parameters?count(p,options->function_parameters):0;
    fprintf(out,"__attribute__((visibility(\"default\"))) RcValue *%s(RcContext *ctx,RcValue *literals) {\n  RcValue *fn=%s(ctx,%d,rc_native_%d,1,&literals);\n",options->expression_entry,closure_api(p),arity,expression_id);
    if(options->function_parameters)fputs("  return fn;\n}\n",out);
    else fputs("  if(!fn)return NULL;\n  RcValue *result=rc_apply(ctx,fn,0,NULL);rc_release(fn);return result;\n}\n",out);
    size_t n=p->nruntime_literals+p->nsymbols;
    RcValue **bindings=n?malloc(n*sizeof(*bindings)):NULL;
    if(n&&!bindings)bad(p,"expression literal bindings allocation failed");
    if(p->nruntime_literals)memcpy(bindings,p->runtime_literals,p->nruntime_literals*sizeof(*bindings));
    for(size_t i=0;i<p->nsymbols;i++){
      RcValue *symbol=rc_symbol(ctx,p->symbols[i]);
      if(!symbol){free(bindings);bad(p,"cannot bind compiled symbols");}
      bindings[p->nruntime_literals+i]=symbol;rc_release(symbol); /* Intern table roots it. */
    }
    bool ok=!ferror(out);if(!ok){free(bindings);rc_error(ctx,"failed writing generated C");}
    else {*options->literal_bindings=bindings;*options->literal_count=n;}
    if(report)*report=(RcEmitReport){p->defuns,p->tops,p->lambdas};
    clear_arena(p);free(p);return ok;
  }
  fprintf(out,"static bool run_top(RcContext *ctx,%s native,int print) {\n  RcValue *fn=%s(ctx,0,native,0,NULL);\n",options->conservative_ownership?"RcNative":"RcNativeOwned",closure_api(p));
  fputs(
        "  if(!fn) return false;\n"
        "  RcValue *value=rc_apply(ctx,fn,0,NULL); rc_release(fn);\n"
        "  if(!value) return false;\n"
        "  if(print && getenv(\"SHEN_RC_PRINT\")) { rc_print(stdout,value); fputc('\\n',stdout); }\n"
        "  rc_release(value); rc_safepoint(ctx); return true;\n}\n",out);
  if(options->module_entry) {
    /* Module definitions are only offered to the host; it decides publication. */
    fprintf(out,"__attribute__((visibility(\"default\"))) bool %s(RcContext *ctx,bool (*define)(RcContext*,RcValue*,RcValue*)) {\n  rc_module_define=define;\n",options->module_entry);
    if(installs.p) fputs(installs.p,out);
    fputs("  return true;\n}\n",out);
    if(report) *report=(RcEmitReport){p->defuns,p->tops,p->lambdas};
    bool ok=!ferror(out); if(!ok) rc_error(ctx,"failed writing generated C");
    clear_arena(p); free(p); return ok;
  }
  fprintf(out,"bool %s(RcContext *ctx) {\n",bootstrap);
  fputs("  if(!rc_install_primitives(ctx) || !rc_install_eval(ctx)) return false;\n",out);
  if(installs.p) fputs(installs.p,out);
  fputs("  if(!rc_install_overwrites(ctx)) return false;\n",out);
  if(!options->conservative_ownership) {
    int hash_id=-1;
    for(ModuleDef *d=p->module_defs;d;d=d->next)
      if(d->kernel_phase&&!strcmp(p->symbols[d->symbol],"shen.hashkey")&&d->exact_hashkey) { hash_id=d->id; break; }
    if(hash_id>=0) fprintf(out,"  if(!rc_install_numeric_hash_owned(ctx,rc_native_%d)) return false;\n",hash_id);
  } else {
    int hash_id=-1;
    for(ModuleDef *d=p->module_defs;d;d=d->next)
      if(d->kernel_phase&&!strcmp(p->symbols[d->symbol],"shen.hashkey")&&d->exact_hashkey) { hash_id=d->id; break; }
    if(hash_id>=0) fprintf(out,"  if(!rc_install_numeric_hash_borrowed(ctx,rc_native_%d)) return false;\n",hash_id);
  }
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
