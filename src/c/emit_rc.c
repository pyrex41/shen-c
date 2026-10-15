#include <stdarg.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "emit_rc.h"
#include "boolean.h"
#include "list.h"
#include "number.h"
#include "string.h"
#include "symbol.h"

/* Scoped owned slots, borrowed inputs, and owned return values. Every
 * expression result has one owner. Consumed temporaries drop immediately;
 * the common exit drops everything still live, including error paths.
 * This is conservative lexical ownership, not a claim of Perceus last-use
 * inference. Tail bounces own arguments before the current frame drops. */
typedef struct { char *p; size_t n, cap; } Buf;
typedef struct { const char *name; int slot; } Binding;
typedef struct Program { Buf functions; int next; long defuns, lambdas, tops; } Program;
typedef struct {
  Program *p; Buf code; Binding *bind; int nb, cap, slots, label, max_args;
  char fail[64];
} RcEmitFn;
static void put(Buf *b, const char *fmt, ...) {
  va_list ap; int n;
  va_start(ap,fmt); n=vsnprintf(NULL,0,fmt,ap); va_end(ap);
  if(n<0) throw_kl_exception("rc emitter formatting failure");
  if(b->n+(size_t)n+1>b->cap) {
    b->cap=(b->n+(size_t)n+1)*2+256; b->p=realloc(b->p,b->cap);
    if(!b->p) throw_kl_exception("rc emitter allocation failure");
  }
  va_start(ap,fmt); vsnprintf(b->p+b->n,b->cap-b->n,fmt,ap); va_end(ap); b->n+=(size_t)n;
}
static void quoted(Buf *b,const char *s) {
  put(b,"\"");
  for(;*s;s++) { unsigned char c=(unsigned char)*s;
    if(c=='"'||c=='\\') put(b,"\\%c",c);
    else if(c<32||c>=127) put(b,"\\%03o",c);
    else put(b,"%c",c);
  }
  put(b,"\"");
}
static const char *name(KLObject *x) { return get_string(get_kl_symbol_name(x)); }
static int named(KLObject *x,const char *s) { return is_kl_symbol(x)&&!strcmp(name(x),s); }
static int binding(RcEmitFn *f,const char *s) { for(int i=f->nb-1;i>=0;i--) if(!strcmp(f->bind[i].name,s)) return f->bind[i].slot; return -1; }
static void bind(RcEmitFn *f,const char *s,int slot) {
  if(f->nb==f->cap) { f->cap=f->cap?f->cap*2:32; f->bind=realloc(f->bind,(size_t)f->cap*sizeof(*f->bind)); if(!f->bind) throw_kl_exception("rc emitter binding allocation"); }
  f->bind[f->nb++]=(Binding){s,slot};
}
static int fresh(RcEmitFn *f) { return f->slots++; }
static void drop(RcEmitFn *f,int x) { put(&f->code,"  rc_release(t[%d]); t[%d]=NULL;\n",x,x); }
static void check(RcEmitFn *f,int x) { put(&f->code,"  if(!t[%d]) goto %s;\n",x,f->fail); }
static void move(RcEmitFn *f,int dst,int src) { put(&f->code,"  t[%d]=t[%d]; t[%d]=NULL;\n",dst,src,src); }
/* The runtime borrows this scratch storage only for the duration of a call;
 * tail requests and closures retain/copy values before returning. Reusing it
 * avoids one function-lifetime compound array for every generated callsite. */
static void argument(RcEmitFn *f,int index,int slot) {
  if(index+1>f->max_args) f->max_args=index+1;
  put(&f->code,"  a[%d]=t[%d];\n",index,slot);
}
static int expr(RcEmitFn *,KLObject *,int);
static int compile(Program *,KLObject *,KLObject *,Binding *,int,int);
static int count(KLObject *xs) {
  int n=0; while(!is_empty_kl_list(xs)) { if(!is_kl_list(xs)) throw_kl_exception("rc emit: improper argument list"); n++; xs=CDR(xs); } return n;
}
static KLObject *at(KLObject *xs,int n) {
  while(n-- && !is_empty_kl_list(xs)) xs=CDR(xs);
  if(is_empty_kl_list(xs)||!is_kl_list(xs)) throw_kl_exception("rc emit: missing special form argument");
  return CAR(xs);
}
static int atom(RcEmitFn *f,KLObject *x,int tail) {
  int t=fresh(f);
  if(is_empty_kl_list(x)) put(&f->code,"  t[%d]=rc_nil();\n",t);
  else if(is_kl_boolean(x)) put(&f->code,"  t[%d]=rc_%s(ctx);\n",t,get_boolean(x)?"true":"false");
  else if(is_kl_number_l(x)) {
    long n=get_kl_number_number_l(x);
    if((int64_t)n==INT64_MIN) put(&f->code,"  t[%d]=rc_integer(ctx,INT64_MIN);\n",t);
    else put(&f->code,"  t[%d]=rc_integer(ctx,INT64_C(%ld));\n",t,n);
  } else if(is_kl_number_d(x)) {
    double n=get_kl_number_number_d(x);
    if(isnan(n)) put(&f->code,"  t[%d]=rc_number(ctx,NAN);\n",t);
    else if(isinf(n)) put(&f->code,"  t[%d]=rc_number(ctx,%sINFINITY);\n",t,n<0?"-":"");
    else put(&f->code,"  t[%d]=rc_number(ctx,%.17g);\n",t,n);
  }
  else if(is_kl_string(x)) { put(&f->code,"  t[%d]=rc_string(ctx,",t); quoted(&f->code,get_string(x)); put(&f->code,");\n"); }
  else if(is_kl_symbol(x)) {
    int b=binding(f,name(x));
    if(b>=0) {
      if(tail) move(f,t,b); /* Final result moves its local owner. */
      else put(&f->code,"  t[%d]=rc_retain(t[%d]);\n",t,b);
    }
    else { put(&f->code,"  t[%d]=rc_symbol(ctx,",t); quoted(&f->code,name(x)); put(&f->code,");\n"); }
  } else throw_kl_exception("rc emit: unsupported atom");
  check(f,t); return t;
}
static void truth(RcEmitFn *f,int t) {
  /* Shen conditions require a boolean, not a general truthy value. */
  put(&f->code,"  if(rc_type(t[%d])!=RC_SYMBOL || (strcmp(rc_text(t[%d]),\"true\") && strcmp(rc_text(t[%d]),\"false\"))) { rc_error(ctx,\"condition is not a boolean\"); goto %s; }\n",t,t,t,f->fail);
}
static int branch(RcEmitFn *f,KLObject *args,int tail) {
  if(count(args)!=3) throw_kl_exception("rc emit: if arity");
  int dst=fresh(f),test=expr(f,at(args,0),0); truth(f,test);
  put(&f->code,"  if(rc_is_true(t[%d])) {\n",test); drop(f,test);
  int a=expr(f,at(args,1),tail); move(f,dst,a);
  put(&f->code,"  } else {\n"); drop(f,test);
  int b=expr(f,at(args,2),tail); move(f,dst,b); put(&f->code,"  }\n"); return dst;
}
static int cond(RcEmitFn *f,KLObject *clauses,int tail) {
  int dst=fresh(f),done=f->label++;
  for(KLObject *p=clauses;!is_empty_kl_list(p);p=CDR(p)) {
    KLObject *cl=CAR(p); if(count(cl)!=2) throw_kl_exception("rc emit: cond clause");
    int test=expr(f,CAR(cl),0); truth(f,test);
    put(&f->code,"  if(rc_is_true(t[%d])) {\n",test); drop(f,test);
    int val=expr(f,CADR(cl),tail); move(f,dst,val);
    put(&f->code,"  goto cond_done_%d;\n  }\n",done); drop(f,test);
  }
  put(&f->code,"  rc_error(ctx,\"cond has no true branch\"); goto %s;\ncond_done_%d:;\n",f->fail,done); return dst;
}
static int closure(RcEmitFn *f,KLObject *params,KLObject *body,int isfreeze) {
  int ncap=f->nb,t=fresh(f);
  /* Keep visible bindings only: shadowed names are not free captures. */
  Binding *caps=malloc((size_t)(ncap+1)*sizeof(*caps)); int n=0;
  if(!caps) throw_kl_exception("rc emitter capture allocation");
  for(int i=0;i<ncap;i++) if(binding(f,f->bind[i].name)==f->bind[i].slot) caps[n++]=f->bind[i];
  int id=compile(f->p,params,body,caps,n,isfreeze);
  for(int i=0;i<n;i++) argument(f,i,caps[i].slot);
  put(&f->code,"  t[%d]=rc_closure(ctx,%d,rc_native_%d,%d,%s);\n",t,isfreeze?0:count(params),id,n,n?"a":"NULL");
  free(caps); check(f,t); f->p->lambdas++; return t;
}
static int trap(RcEmitFn *f,KLObject *args,int tail) {
  if(count(args)!=2) throw_kl_exception("rc emit: trap-error arity");
  int dst=fresh(f),mark=f->slots,id=f->label++; char saved[64]; snprintf(saved,sizeof(saved),"%s",f->fail);
  snprintf(f->fail,sizeof(f->fail),"trap_%d",id);
  int val=expr(f,CAR(args),0); move(f,dst,val);
  snprintf(f->fail,sizeof(f->fail),"%s",saved);
  put(&f->code,"  goto trap_done_%d;\ntrap_%d:;\n",id,id);
  for(int i=mark;i<f->slots;i++) drop(f,i);
  int err=fresh(f); put(&f->code,"  t[%d]=rc_string(ctx,rc_error_message(ctx));\n",err); check(f,err);
  put(&f->code,"  rc_clear_error(ctx);\n");
  int h=expr(f,CADR(args),0);
  argument(f,0,err);
  put(&f->code,"  t[%d]=%s(ctx,t[%d],1,a);\n",dst,tail?"rc_tailcall":"rc_apply",h);
  drop(f,h); drop(f,err); check(f,dst);
  put(&f->code,"trap_done_%d:;\n",id); return dst;
}
static int function_head(RcEmitFn *f,KLObject *head) {
  int fn=expr(f,head,0);
  put(&f->code,"  if(rc_type(t[%d])==RC_SYMBOL) { RcValue *resolved=rc_function_get(ctx,t[%d]); rc_release(t[%d]); t[%d]=resolved; }\n",fn,fn,fn,fn);
  check(f,fn); return fn;
}
static int expr(RcEmitFn *f,KLObject *x,int tail) {
  if(!is_kl_list(x)||is_empty_kl_list(x)) return atom(f,x,tail);
  KLObject *head=CAR(x),*args=CDR(x);
  int special=is_kl_symbol(head)&&binding(f,name(head))<0;
  if(special&&named(head,"if")) return branch(f,args,tail);
  if(special&&named(head,"cond")) return cond(f,args,tail);
  if(special&&named(head,"let")) {
    if(count(args)!=3||!is_kl_symbol(CAR(args))) throw_kl_exception("rc emit: let shape");
    int a=expr(f,CADR(args),0),old=f->nb; bind(f,name(CAR(args)),a);
    int result=expr(f,at(args,2),tail); f->nb=old; drop(f,a); return result;
  }
  if(special&&named(head,"do")&&count(args)==2) {
    /* do is an ordinary eager function. Flatten its right spine to bound
     * compiler recursion while retaining each function cell before its
     * arguments, so redefinition during an argument cannot change it. */
    int *functions=NULL,*left=NULL,n=0;
    while(is_non_empty_kl_list(x)&&named(CAR(x),"do")&&binding(f,"do")<0&&count(CDR(x))==2) {
      args=CDR(x);
      functions=realloc(functions,(size_t)(n+1)*sizeof(*functions));
      left=realloc(left,(size_t)(n+1)*sizeof(*left));
      if(!functions||!left) throw_kl_exception("rc emitter do allocation");
      functions[n]=function_head(f,CAR(x));
      left[n]=expr(f,CAR(args),0); n++; x=CADR(args);
    }
    int result=expr(f,x,0);
    for(int i=n-1;i>=0;i--) {
      int next=fresh(f);
      argument(f,0,left[i]); argument(f,1,result);
      put(&f->code,"  t[%d]=%s(ctx,t[%d],2,a);\n",next,tail&&i==0?"rc_tailcall":"rc_apply",functions[i]);
      drop(f,functions[i]); drop(f,left[i]); drop(f,result); check(f,next); result=next;
    }
    free(functions); free(left); return result;
  }
  if(special&&named(head,"lambda")) {
    if(count(args)!=2||!is_kl_symbol(CAR(args))) throw_kl_exception("rc emit: lambda shape");
    KLObject *params=CONS(CAR(args),get_empty_kl_list());
    return closure(f,params,CADR(args),0);
  }
  if(special&&named(head,"freeze")) { if(count(args)!=1) throw_kl_exception("rc emit: freeze arity"); return closure(f,get_empty_kl_list(),CAR(args),1); }
  if(special&&named(head,"trap-error")) return trap(f,args,tail);
  if(special&&named(head,"type")) { if(count(args)!=2) throw_kl_exception("rc emit: type arity"); return expr(f,CAR(args),tail); }
  if(special&&(named(head,"and")||named(head,"or"))) {
    if(count(args)!=2) throw_kl_exception("rc emit: boolean form arity");
    int dst=fresh(f),a=expr(f,CAR(args),0); truth(f,a);
    put(&f->code,"  if(%src_is_true(t[%d])) {\n",named(head,"and")?"":"!",a);
    drop(f,a); int b=expr(f,CADR(args),tail);
    put(&f->code,"  t[%d]=rc_require_boolean(ctx,t[%d]);\n",b,b); check(f,b); move(f,dst,b);
    put(&f->code,"  } else {\n"); move(f,dst,a); put(&f->code,"  }\n"); return dst;
  }
  if(special&&named(head,"defun")) {
    if(count(args)!=3||!is_kl_symbol(CAR(args))) throw_kl_exception("rc emit: defun shape");
    int id=compile(f->p,CADR(args),at(args,2),NULL,0,0),t=fresh(f),cl=fresh(f);
    put(&f->code,"  t[%d]=rc_symbol(ctx,",t); quoted(&f->code,name(CAR(args))); put(&f->code,");\n"); check(f,t);
    put(&f->code,"  t[%d]=rc_closure(ctx,%d,rc_native_%d,0,NULL);\n",cl,count(CADR(args)),id); check(f,cl);
    put(&f->code,"  if(!rc_function_set(ctx,t[%d],t[%d])) goto %s;\n",t,cl,f->fail); drop(f,cl); f->p->defuns++; return t;
  }
  int fn=function_head(f,head),n=count(args),*av=malloc((size_t)(n+1)*sizeof(*av));
  if(!av) throw_kl_exception("rc emitter argument allocation");
  for(int i=0;i<n;i++) { av[i]=expr(f,CAR(args),0); args=CDR(args); }
  int result=fresh(f);
  for(int i=0;i<n;i++) argument(f,i,av[i]);
  put(&f->code,"  t[%d]=%s(ctx,t[%d],%d,%s);\n",result,tail?"rc_tailcall":"rc_apply",fn,n,n?"a":"NULL");
  drop(f,fn); for(int i=0;i<n;i++) drop(f,av[i]); free(av); check(f,result); return result;
}
static int compile(Program *p,KLObject *params,KLObject *body,Binding *caps,int ncaps,int freeze) {
  RcEmitFn f={0}; f.p=p; int id=p->next++,arity=freeze?0:count(params); snprintf(f.fail,sizeof(f.fail),"cleanup");
  for(int i=0;i<ncaps;i++) { int s=fresh(&f); bind(&f,caps[i].name,s); put(&f.code,"  t[%d]=rc_retain(rc_capture(self,%d));\n",s,i); }
  for(int i=0;i<arity;i++,params=CDR(params)) { if(!is_kl_symbol(CAR(params))) throw_kl_exception("rc emit: function parameter"); int s=fresh(&f); bind(&f,name(CAR(params)),s); put(&f.code,"  t[%d]=rc_retain(argv[%d]);\n",s,i); }
  int r=expr(&f,body,1);
  put(&p->functions,"static RcValue *rc_native_%d(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *argv) {\n  (void)self; (void)argv;\n  if(argc!=%d) return rc_error(ctx,\"native arity mismatch\");\n  RcValue *result=NULL;\n",id,arity);
  if(f.slots>64) {
    /* One zeroed frame slab: owned slots first, borrowed scratch after them.
     * Failure occurs before retaining any borrowed inputs. */
    put(&p->functions,"  RcValue **t=calloc(%d,sizeof(*t));\n  if(!t) return rc_error(ctx,\"native frame allocation failed\");\n  RcValue **a=t+%d; (void)a;\n",f.slots+f.max_args,f.slots);
  } else {
    put(&p->functions,"  RcValue *t[%d]={0}; RcValue *a[%d]; (void)a;\n",f.slots?f.slots:1,f.max_args?f.max_args:1);
  }
  put(&p->functions,"%s  result=t[%d]; t[%d]=NULL;\ncleanup:\n  for(size_t i=0;i<%d;i++) rc_release(t[i]);\n",f.code.p?f.code.p:"",r,r,f.slots);
  if(f.slots>64) put(&p->functions,"  free(t);\n");
  put(&p->functions,"  return result;\n}\n");
  free(f.code.p); free(f.bind); return id;
}
int shen_emit_rc_program(FILE *out,KLObject **kernel,long nkernel,KLObject **user,long nuser,const char *init,const char *label,ShenEmitReport *report) {
  Program p={0}; Buf installs={0},kt={0},ut={0};
  (void)label;
  if(!out||nkernel<0||nuser<0) return -1;
  for(int phase=0;phase<2;phase++) {
    KLObject **forms=phase?user:kernel; long n=phase?nuser:nkernel;
    for(long i=0;i<n;i++) {
      int id=compile(&p,get_empty_kl_list(),forms[i],NULL,0,0);
      int def=is_non_empty_kl_list(forms[i])&&named(CAR(forms[i]),"defun");
      Buf *where=phase?&ut:(def?&installs:&kt);
      put(where,"  if(!run_top(ctx,rc_native_%d,%d)) goto failed;\n",id,phase&&!def);
      if(!def) p.tops++;
    }
  }
  fputs("/* Native KLambda -> independent reference-counted runtime.\n"
        " * Scoped owned slots with explicit temporary drops and error cleanup.\n"
        " * No legacy KLObject, GC allocator, or source evaluator wrapper. */\n"
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <stdint.h>\n#include <math.h>\n#include \"rc.h\"\n#include \"primitives.h\"\n#include \"eval.h\"\n",out);
  for(int i=0;i<p.next;i++) fprintf(out,"static RcValue *rc_native_%d(RcContext*,RcValue*,size_t,RcValue *const*);\n",i);
  if(p.functions.p) fputs(p.functions.p,out);
  fputs("static int run_top(RcContext *ctx,RcNative native,int print) {\n"
        "  RcValue *fn=rc_closure(ctx,0,native,0,NULL);\n"
        "  if(!fn) return 0;\n"
        "  RcValue *value=rc_apply(ctx,fn,0,NULL); rc_release(fn);\n"
        "  if(!value) return 0;\n"
        "  if(print && getenv(\"SHEN_RC_PRINT\")) { rc_print(stdout,value); fputc('\\n',stdout); }\n"
        "  rc_release(value); rc_safepoint(ctx); return 1;\n}\n"
        "int main(void) {\n  RcContext *ctx=rc_context_new();\n"
        "  if(!ctx) return 1;\n"
        "  if(!rc_install_primitives(ctx) || !rc_install_eval(ctx)) goto failed;\n",out);
  if(installs.p) fputs(installs.p,out);
  fputs("  if(!rc_install_overwrites(ctx)) goto failed;\n",out);
  if(kt.p) fputs(kt.p,out);
  if(init&&*init) {
    Buf s={0}; quoted(&s,init);
    fprintf(out,"  { RcValue *fn=rc_symbol(ctx,%s); if(!fn) goto failed; RcValue *v=rc_apply(ctx,fn,0,NULL); rc_release(fn); if(!v) goto failed; rc_release(v); }\n",s.p); free(s.p);
  }
  if(ut.p) fputs(ut.p,out);
  fputs("  rc_context_free(ctx); return 0;\nfailed:\n"
        "  fprintf(stderr,\"shen-rc: %s\\n\",rc_error_message(ctx));\n"
        "  rc_context_free(ctx); return 1;\n}\n",out);
  if(report) *report=(ShenEmitReport){p.defuns,p.tops,p.lambdas};
  free(p.functions.p); free(installs.p); free(kt.p); free(ut.p);
  return ferror(out)?-1:0;
}
