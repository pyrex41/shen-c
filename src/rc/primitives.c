#include "primitives.h"
#include "rc_internal.h"
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/utsname.h>

/* All callbacks borrow argv and return one owned value. Strings use explicit
 * byte lengths; byte operations preserve 0..255, including embedded zero. */
enum Op {
 P_INTERN, P_POS, P_TLSTR, P_CN, P_STR, P_STRING, P_NSTR, P_STRN,
 P_SET, P_VALUE, P_ERROR, P_ERRSTR, P_CONS, P_HD, P_TL, P_CONSP, P_EQ,
 P_TYPE, P_VECTOR, P_VSET, P_VGET, P_VECTORP, P_WRITE, P_READ, P_OPEN,
 P_CLOSE, P_TIME, P_ADD, P_SUB, P_MUL, P_DIV, P_GT, P_LT, P_GE, P_LE,
 P_NUMBER, P_SYMBOL, P_BOOLEAN, P_INTEGER, P_VARIABLE, P_NOT, P_VALUEOR,
 P_VGETOR, P_CHAROUT, P_CHARIN, P_WRITESTR, P_PR, P_FUNCTION, P_EXIT,
 P_GC, P_READCHARS, P_DO
};
struct Primitive { const char *name; size_t arity; enum Op op; };
static const struct Primitive core[] = {
 {"intern",1,P_INTERN},{"pos",2,P_POS},{"tlstr",1,P_TLSTR},
 {"cn",2,P_CN},{"str",1,P_STR},{"string?",1,P_STRING},
 {"n->string",1,P_NSTR},{"string->n",1,P_STRN},
 {"set",2,P_SET},{"value",1,P_VALUE},{"simple-error",1,P_ERROR},
 {"error-to-string",1,P_ERRSTR},{"cons",2,P_CONS},{"hd",1,P_HD},
 {"tl",1,P_TL},{"cons?",1,P_CONSP},{"=",2,P_EQ},{"type",2,P_TYPE},
 {"absvector",1,P_VECTOR},{"address->",3,P_VSET},{"<-address",2,P_VGET},
 {"absvector?",1,P_VECTORP},{"write-byte",2,P_WRITE},{"read-byte",1,P_READ},
 {"open",2,P_OPEN},{"close",1,P_CLOSE},{"get-time",1,P_TIME},
 {"+",2,P_ADD},{"-",2,P_SUB},{"*",2,P_MUL},{"/",2,P_DIV},
 {">",2,P_GT},{"<",2,P_LT},{">=",2,P_GE},{"<=",2,P_LE},
 {"number?",1,P_NUMBER},{"function",1,P_FUNCTION},{"exit",1,P_EXIT},
 {"shen.gc",0,P_GC},{"do",2,P_DO}
};
static const struct Primitive overwrites[] = {
 {"symbol?",1,P_SYMBOL},{"boolean?",1,P_BOOLEAN},{"integer?",1,P_INTEGER},
 {"variable?",1,P_VARIABLE},{"not",1,P_NOT},{"value/or",2,P_VALUEOR},
 {"<-address/or",3,P_VGETOR},{"shen.char-stoutput?",1,P_CHAROUT},
 {"shen.char-stinput?",1,P_CHARIN},{"shen.write-string",2,P_WRITESTR},
 {"pr",2,P_PR},{"read-file-as-charlist",1,P_READCHARS}
};
static bool symbol_is(RcValue *v,const char *s) {
 return rc_type(v)==RC_SYMBOL && strcmp(rc_text(v),s)==0;
}
static bool boolean(RcValue *v) { return rc_is_boolean(v); }
static RcValue *truth(RcContext *c,bool b) {return b?rc_true(c):rc_false(c);}
static bool integer_arg(RcValue *v,int64_t *out) {
 if(rc_type(v)!=RC_NUMBER || !rc_number_is_exact_integer(v)) return false;
 *out=rc_integer_value(v); return true;
}
static RcValue *global(RcContext *c,const char *name) {
 RcValue *s=rc_symbol(c,name); if(!s)return NULL;
 RcValue *v=rc_global_get(c,s);rc_release(s);return v;
}
static bool put_global(RcContext *c,const char *name,RcValue *v) {
 if(!v)return false;
 RcValue *s=rc_symbol(c,name); bool ok=s && rc_global_set(c,s,v);
 rc_release(s); rc_release(v); return ok;
}
static RcValue *atom_string(RcContext *c,RcValue *v) {
 char buf[160];
 switch(rc_type(v)) {
 case RC_SYMBOL:return rc_string_n(c,rc_text(v),rc_text_length(v));
 case RC_STRING:{size_t n=rc_text_length(v); if(n>SIZE_MAX-3)return rc_error(c,"String too large");
  char *s=malloc(n+3);if(!s)return rc_error(c,"Out of memory");
  s[0]='"';memcpy(s+1,rc_text(v),n);s[n+1]='"';s[n+2]=0;
  RcValue *r=rc_string_n(c,s,n+2);free(s);return r;}
 case RC_NUMBER:
  if(rc_number_is_exact_integer(v))snprintf(buf,sizeof buf,"%" PRId64,rc_integer_value(v));
  else {snprintf(buf,sizeof buf,"%.*g",DBL_DIG,rc_number_value(v));
   if(!strchr(buf,'.')&&!strchr(buf,'e')&&isfinite(rc_number_value(v)))strcat(buf,".0");}
  return rc_string(c,buf);
 case RC_CLOSURE:snprintf(buf,sizeof buf,"#<Function {%p}>",(void*)v);return rc_string(c,buf);
 case RC_STREAM:snprintf(buf,sizeof buf,"#<Stream {%p}>",(void*)v);return rc_string(c,buf);
 default:return rc_error(c,"Parameter is not an atom, closure, or stream");
 }
}
static RcValue *numeric(RcContext *c,enum Op op,RcValue *a,RcValue *b) {
 if(rc_type(a)!=RC_NUMBER||rc_type(b)!=RC_NUMBER)return rc_error(c,"Arithmetic requires numbers");
 bool ints=rc_number_is_exact_integer(a)&&rc_number_is_exact_integer(b);
 int64_t i=0,j=0,k=0; double x=0,y=0;
 if(ints){i=rc_integer_value(a);j=rc_integer_value(b);}
 else {x=rc_number_value(a);y=rc_number_value(b);}
 switch(op){
 case P_GT:return truth(c,ints?i>j:x>y);case P_LT:return truth(c,ints?i<j:x<y);
 case P_GE:return truth(c,ints?i>=j:x>=y);case P_LE:return truth(c,ints?i<=j:x<=y);
 case P_ADD:if(ints){if(__builtin_add_overflow(i,j,&k))return rc_error(c,"Integer overflow");return rc_integer(c,k);}return rc_number(c,x+y);
 case P_SUB:if(ints){if(__builtin_sub_overflow(i,j,&k))return rc_error(c,"Integer overflow");return rc_integer(c,k);}return rc_number(c,x-y);
 case P_MUL:if(ints){if(__builtin_mul_overflow(i,j,&k))return rc_error(c,"Integer overflow");return rc_integer(c,k);}return rc_number(c,x*y);
 case P_DIV:
  if(ints?j==0:y==0)return rc_error(c,"Division by zero");
  if(ints){if(i==INT64_MIN&&j==-1)return rc_error(c,"Integer overflow");if(i%j==0)return rc_integer(c,i/j);}
  if(ints){x=(double)i;y=(double)j;}
  return rc_number(c,x/y);
 default:return rc_error(c,"Unknown arithmetic operation");
 }
}
static RcValue *open_file(RcContext *c,RcValue *path,RcValue *direction) {
 if(rc_type(path)!=RC_STRING || (!symbol_is(direction,"in")&&!symbol_is(direction,"out")))
  return rc_error(c,"open requires string path and in or out direction");
 if(memchr(rc_text(path),0,rc_text_length(path)))return rc_error(c,"File path contains zero byte");
 RcValue *home=global(c,"*home-directory*");if(!home)return NULL;
 if(rc_type(home)!=RC_STRING){rc_release(home);return rc_error(c,"*home-directory* must be string");}
 size_t a=rc_text_length(home),b=rc_text_length(path);
 if(a>SIZE_MAX-b-1){rc_release(home);return rc_error(c,"File path too large");}
 char *s=malloc(a+b+1);if(!s){rc_release(home);return rc_error(c,"Out of memory");}
 memcpy(s,rc_text(home),a);memcpy(s+a,rc_text(path),b);s[a+b]=0;rc_release(home);
 FILE *f=fopen(s,symbol_is(direction,"in")?"rb":"wb");
 if(!f){RcValue *r=rc_error(c,"Cannot open %s: %s",s,strerror(errno));free(s);return r;}
 free(s);RcValue *v=rc_stream_with_direction(c,f,true,symbol_is(direction,"in"));if(!v)fclose(f);return v;
}
static RcValue *write_string(RcContext *c,RcValue *text,RcValue *stream) {
 if(rc_type(text)!=RC_STRING||rc_type(stream)!=RC_STREAM||!rc_stream_file(stream)||rc_stream_is_input(stream))
  return rc_error(c,"write-string requires string and open stream");
 FILE *f=rc_stream_file(stream);size_t n=rc_text_length(text);
 if((n&&fwrite(rc_text(text),1,n,f)!=n)||fflush(f))return rc_error(c,"Failed to write stream");
 return rc_retain(text);
}
static RcValue *primitive_op(RcContext *c,RcValue *self,enum Op op,
                              size_t argc,RcValue *const *v) {
 RcValue *a=argc?v[0]:NULL,*b=argc>1?v[1]:NULL;int64_t i;
 if(op>=P_ADD&&op<=P_LE)return numeric(c,op,a,b);
 switch(op){
 case P_INTERN:
  if(rc_type(a)!=RC_STRING)return rc_error(c,"intern requires string");
  if(memchr(rc_text(a),0,rc_text_length(a)))return rc_error(c,"Symbol name contains zero byte");
  return rc_symbol(c,rc_text(a));
 case P_POS:
  if(rc_type(a)!=RC_STRING||!integer_arg(b,&i)||i<0||(uint64_t)i>=rc_text_length(a))return rc_error(c,"String position out of range");
  return rc_string_n(c,rc_text(a)+(size_t)i,1);
 case P_TLSTR:
  if(rc_type(a)!=RC_STRING||!rc_text_length(a))return rc_error(c,"tlstr requires nonempty string");
  return rc_string_n(c,rc_text(a)+1,rc_text_length(a)-1);
 case P_CN:{
  if(rc_type(a)!=RC_STRING||rc_type(b)!=RC_STRING)return rc_error(c,"cn requires strings");
  size_t n=rc_text_length(a),m=rc_text_length(b);if(n>SIZE_MAX-m-1)return rc_error(c,"String too large");
  char *s=malloc(n+m+1);if(!s)return rc_error(c,"Out of memory");memcpy(s,rc_text(a),n);memcpy(s+n,rc_text(b),m);
  RcValue *r=rc_string_n(c,s,n+m);free(s);return r;}
 case P_STR:return atom_string(c,a);
 case P_STRING:return truth(c,rc_type(a)==RC_STRING);
 case P_NSTR:{if(!integer_arg(a,&i)||i<0||i>255)return rc_error(c,"Byte must be integer 0..255");char byte=(char)i;return rc_string_n(c,&byte,1);}
 case P_STRN:
  if(rc_type(a)!=RC_STRING||!rc_text_length(a))return rc_error(c,"string->n requires nonempty string");
  return rc_integer(c,(unsigned char)rc_text(a)[0]);
 case P_SET:return rc_global_set(c,a,b)?rc_retain(b):NULL;
 case P_VALUE:return rc_global_get(c,a);
 case P_ERROR:
  if(rc_type(a)!=RC_STRING)return rc_error(c,"simple-error requires string");
  return rc_error(c,"%s",rc_text(a));
 case P_ERRSTR:
  if(rc_type(a)!=RC_STRING)return rc_error(c,"error-to-string requires error string");return rc_retain(a);
 case P_CONS:return rc_cons(c,a,b);case P_HD:return rc_head(c,a);case P_TL:return rc_tail(c,a);
 case P_CONSP:return truth(c,rc_type(a)==RC_PAIR);
 case P_EQ:{bool equal=rc_equal(a,b);return rc_has_error(c)?NULL:truth(c,equal);}
 case P_TYPE:return rc_retain(a);
 case P_DO:return rc_retain(b);
 case P_VECTOR:
  if(!integer_arg(a,&i)||i<0)return rc_error(c,"Vector size must be nonnegative integer");return rc_vector(c,(size_t)i);
 case P_VSET:case P_VGET:case P_VGETOR:
  if(rc_type(a)!=RC_VECTOR||!integer_arg(b,&i)||i<0||(uint64_t)i>=rc_vector_length(a)){
   if(op==P_VGETOR)return rc_apply(c,v[2],0,NULL);return rc_error(c,"Vector index out of bounds");}
  if(op==P_VSET)return rc_vector_set(c,a,(size_t)i,v[2])?rc_retain(a):NULL;
  return rc_vector_get(c,a,(size_t)i);
 case P_VECTORP:return truth(c,rc_type(a)==RC_VECTOR);
 case P_WRITE:
  if(!integer_arg(a,&i)||i<0||i>255||rc_type(b)!=RC_STREAM||!rc_stream_file(b)||rc_stream_is_input(b))return rc_error(c,"write-byte requires byte and open stream");
  if(fputc((int)i,rc_stream_file(b))==EOF)return rc_error(c,"Failed to write byte");return rc_retain(a);
 case P_READ:{
  if(rc_type(a)!=RC_STREAM||!rc_stream_file(a)||!rc_stream_is_input(a))return rc_error(c,"read-byte requires open stream");
  int byte=fgetc(rc_stream_file(a));if(byte==EOF&&ferror(rc_stream_file(a)))return rc_error(c,"Failed to read byte");return rc_integer(c,byte==EOF?-1:byte);}
 case P_OPEN:return open_file(c,a,b);
 case P_CLOSE:return rc_stream_close(c,a)?rc_nil():NULL;
 case P_TIME:
  if(symbol_is(a,"unix"))return rc_integer(c,(int64_t)time(NULL));
  if(symbol_is(a,"run"))return rc_number(c,(double)clock()/CLOCKS_PER_SEC-rc_number_value(rc_capture(self,1)));
  return rc_error(c,"get-time requires unix or run");
 case P_NUMBER:return truth(c,rc_type(a)==RC_NUMBER);
 case P_SYMBOL:return truth(c,rc_type(a)==RC_SYMBOL&&!boolean(a));
 case P_BOOLEAN:return truth(c,boolean(a));
 case P_INTEGER:return truth(c,rc_type(a)==RC_NUMBER&&(rc_number_is_exact_integer(a)||(isfinite(rc_number_value(a))&&trunc(rc_number_value(a))==rc_number_value(a))));
 case P_VARIABLE:return truth(c,rc_type(a)==RC_SYMBOL&&rc_text_length(a)&&rc_text(a)[0]>='A'&&rc_text(a)[0]<='Z');
 case P_NOT:if(!boolean(a))return rc_error(c,"not requires boolean");return truth(c,!rc_is_true(a));
 case P_VALUEOR:{
  RcValue *r=rc_type(a)==RC_SYMBOL?rc_global_get(c,a):NULL;if(r)return r;rc_clear_error(c);return rc_apply(c,b,0,NULL);}
 case P_CHAROUT:return truth(c,rc_type(a)==RC_STREAM&&rc_stream_file(a)&&!rc_stream_is_input(a));
 case P_CHARIN:return rc_false(c);
 case P_WRITESTR:return write_string(c,a,b);
 case P_PR:{
  RcValue *hush=global(c,"*hush*");if(!hush){rc_clear_error(c);}bool quiet=hush&&rc_is_true(hush)&&rc_stream_file(b)==stdout;rc_release(hush);
  return quiet?rc_retain(a):write_string(c,a,b);}
 case P_FUNCTION:return rc_function_get(c,a);
 case P_EXIT:if(!integer_arg(a,&i)||i<INT_MIN||i>INT_MAX)return rc_error(c,"exit requires integer status");exit((int)i);
 case P_GC:rc_collect(c);return rc_nil();
 case P_READCHARS:{
  RcValue *dir=rc_symbol(c,"in");if(!dir)return NULL;RcValue *stream=open_file(c,a,dir);rc_release(dir);if(!stream)return NULL;
  RcValue *list=rc_nil();int byte;
  while((byte=fgetc(rc_stream_file(stream)))!=EOF){RcValue *item=rc_integer(c,byte);if(!item){rc_release(list);rc_release(stream);return NULL;}
   RcValue *cell=rc_cons(c,item,list);rc_release(item);rc_release(list);if(!cell){rc_release(stream);return NULL;}list=cell;}
  if(ferror(rc_stream_file(stream))){rc_release(list);rc_release(stream);return rc_error(c,"Failed to read file");}rc_release(stream);
  RcValue *out=rc_nil();for(RcValue *p=list;rc_type(p)==RC_PAIR;p=rc_tail_borrowed(p)){RcValue *cell=rc_cons(c,rc_head_borrowed(p),out);rc_release(out);if(!cell){rc_release(list);return NULL;}out=cell;}rc_release(list);return out;}
 default:return rc_error(c,"Unknown primitive");
 }
}
static RcValue *primitive(RcContext *c,RcValue *self,size_t argc,RcValue *const *v) {
 return primitive_op(c,self,(enum Op)rc_integer_value(rc_capture(self,0)),argc,v);
}
bool rc_primitive_is_do(const RcValue *function) {
 RcValue *code=rc_capture(function,0);
 return rc_closure_is_native(function,primitive)&&rc_arity(function)==2&&
        code&&rc_integer_value(code)==P_DO;
}
static RcValue *primitive_call_owned_slow(RcContext *c,RcValue *function,
                                size_t argc,RcValue **args,bool tail_position) {
 RcValue *code=rc_native_call_capture(c,function,argc,args,primitive);
 if(code) {
  enum Op op=(enum Op)rc_integer_value(code);
  size_t expected=0;
  switch(op) {
   case P_HD:case P_TL:case P_CONSP:case P_NUMBER:case P_STRING:
   case P_SYMBOL:case P_BOOLEAN:case P_INTEGER:case P_VARIABLE:
   case P_VECTORP:case P_NOT: expected=1;break;
   case P_DO:case P_CONS:case P_EQ:case P_ADD:case P_SUB:case P_MUL:case P_DIV:
   case P_GT:case P_LT:case P_GE:case P_LE: expected=2;break;
   default:break;
  }
  /* Original arity must match too: a partially applied primitive still has
   * the same callback/opcode but must prepend its captured bound arguments. */
  if(expected&&argc==expected) {
   RcValue *result;
   if(op==P_CONS) {
    RcValue *head=args[0],*tail=args[1];args[0]=NULL;args[1]=NULL;
    result=rc_cons_owned(c,head,tail);
   } else result=primitive_op(c,function,op,argc,args);
   /* C callers may store managed tail requests in pairs or pass them to do.
    * Ordinary apply must drive such a returned request, including its boolean
    * guard. These leaf projections are pure, so defer before consuming args. */
   if(!tail_position&&(op==P_HD||op==P_TL||op==P_DO)&&rc_type(result)==RC_TAIL) {
    rc_release(result);return rc_apply_owned_args(c,function,argc,args);
   }
   for(size_t i=0;i<argc;i++){RcValue *v=args[i];args[i]=NULL;rc_release(v);}
   if(!result)return NULL;
   if(rc_has_error(c)){rc_release(result);return NULL;}
   rc_safepoint(c);
   return result;
  }
 }
 return tail_position?rc_tailcall_owned_args(c,function,argc,args)
                     :rc_apply_owned_args(c,function,argc,args);
}
/* Separate tiny bodies let the compiler remove opcode and arity dispatch.
 * A canonical closure stays a context root even if its name is redefined.
 * Reinstallation replaces that bounded root; old snapshots safely fall back. */
static inline RcValue *leaf_finish(RcContext *c,RcValue *result) {
 if(!result)return NULL;
 if(rc_has_error(c)){rc_release(result);return NULL;}
 rc_safepoint(c);return result;
}
#define UNARY_LEAF(api,slot,expression,projection) \
static inline __attribute__((always_inline)) RcValue *api##_validated(RcContext *c,RcValue *fn,RcValue **args,bool tail) { \
 RcValue *result=(expression); \
 if(projection&&!tail&&rc_type(result)==RC_TAIL) { \
  rc_release(result);return rc_apply_owned_args(c,fn,1,args); \
 } \
 RcValue *arg=args[0];args[0]=NULL;rc_release(arg); \
 return leaf_finish(c,result); \
} \
RcValue *api(RcContext *c,RcValue *fn,RcValue **args,bool tail) { \
 if(!rc_builtin_call_matches(c,slot,fn,1,args)) \
  return primitive_call_owned_slow(c,fn,1,args,tail); \
 return api##_validated(c,fn,args,tail); \
}
UNARY_LEAF(rc_primitive_hd_owned,RC_BUILTIN_HD,rc_head(c,args[0]),true)
UNARY_LEAF(rc_primitive_tl_owned,RC_BUILTIN_TL,rc_tail(c,args[0]),true)
UNARY_LEAF(rc_primitive_consp_owned,RC_BUILTIN_CONSP,truth(c,rc_type(args[0])==RC_PAIR),false)
UNARY_LEAF(rc_primitive_numberp_owned,RC_BUILTIN_NUMBERP,truth(c,rc_type(args[0])==RC_NUMBER),false)
UNARY_LEAF(rc_primitive_stringp_owned,RC_BUILTIN_STRINGP,truth(c,rc_type(args[0])==RC_STRING),false)
#undef UNARY_LEAF
static inline __attribute__((always_inline)) RcValue *rc_primitive_eq_owned_validated(RcContext *c,RcValue *fn,RcValue **args,bool tail) {
 (void)fn;(void)tail;
 RcValue *result=truth(c,rc_equal(args[0],args[1]));
 RcValue *a=args[0],*b=args[1];args[0]=NULL;args[1]=NULL;
 rc_release(a);rc_release(b);return leaf_finish(c,result);
}
RcValue *rc_primitive_eq_owned(RcContext *c,RcValue *fn,RcValue **args,bool tail) {
 if(!rc_builtin_call_matches(c,RC_BUILTIN_EQ,fn,2,args))
  return primitive_call_owned_slow(c,fn,2,args,tail);
 return rc_primitive_eq_owned_validated(c,fn,args,tail);
}
/* The generic dispatcher has validated the canonical identity and operands.
 * Construction consumes both slots without repeating the dispatch guard. */
static inline __attribute__((always_inline)) RcValue *rc_primitive_cons_owned_validated(RcContext *c,RcValue *fn,RcValue **args,bool tail) {
 (void)fn;(void)tail;
 RcValue *head=args[0],*rest=args[1];args[0]=NULL;args[1]=NULL;
 return leaf_finish(c,rc_cons_owned(c,head,rest));
}
RcValue *rc_primitive_cons_owned(RcContext *c,RcValue *fn,RcValue **args,bool tail) {
 if(!rc_builtin_call_matches(c,RC_BUILTIN_CONS,fn,2,args))
  return primitive_call_owned_slow(c,fn,2,args,tail);
 return rc_primitive_cons_owned_validated(c,fn,args,tail);
}
#define BINARY_NUMERIC_LEAF(api,slot,op) \
static inline __attribute__((always_inline)) RcValue *api##_validated(RcContext *c,RcValue *fn,RcValue **args,bool tail) { \
 (void)fn;(void)tail; \
 RcValue *result=numeric(c,op,args[0],args[1]); \
 RcValue *a=args[0],*b=args[1];args[0]=NULL;args[1]=NULL; \
 rc_release(a);rc_release(b);return leaf_finish(c,result); \
} \
RcValue *api(RcContext *c,RcValue *fn,RcValue **args,bool tail) { \
 if(!rc_builtin_call_matches(c,slot,fn,2,args)) \
  return primitive_call_owned_slow(c,fn,2,args,tail); \
 return api##_validated(c,fn,args,tail); \
}
BINARY_NUMERIC_LEAF(rc_primitive_add_owned,RC_BUILTIN_ADD,P_ADD)
BINARY_NUMERIC_LEAF(rc_primitive_sub_owned,RC_BUILTIN_SUB,P_SUB)
BINARY_NUMERIC_LEAF(rc_primitive_mul_owned,RC_BUILTIN_MUL,P_MUL)
BINARY_NUMERIC_LEAF(rc_primitive_div_owned,RC_BUILTIN_DIV,P_DIV)
BINARY_NUMERIC_LEAF(rc_primitive_gt_owned,RC_BUILTIN_GT,P_GT)
BINARY_NUMERIC_LEAF(rc_primitive_lt_owned,RC_BUILTIN_LT,P_LT)
BINARY_NUMERIC_LEAF(rc_primitive_ge_owned,RC_BUILTIN_GE,P_GE)
BINARY_NUMERIC_LEAF(rc_primitive_le_owned,RC_BUILTIN_LE,P_LE)
#undef BINARY_NUMERIC_LEAF
RcValue *rc_primitive_call_owned(RcContext *c,RcValue *fn,
                                size_t argc,RcValue **args,bool tail) {
 typedef RcValue *(*BuiltinCall)(RcContext *,RcValue *,RcValue **,bool);
 _Static_assert(RC_BUILTIN_COUNT == RC_BUILTIN_CONS + 1,
                "extend the validated dispatch table for new canonical slots");
 static const BuiltinCall calls[RC_BUILTIN_COUNT]={
  [RC_BUILTIN_HD]=rc_primitive_hd_owned_validated,
  [RC_BUILTIN_TL]=rc_primitive_tl_owned_validated,
  [RC_BUILTIN_CONSP]=rc_primitive_consp_owned_validated,
  [RC_BUILTIN_NUMBERP]=rc_primitive_numberp_owned_validated,
  [RC_BUILTIN_STRINGP]=rc_primitive_stringp_owned_validated,
  [RC_BUILTIN_EQ]=rc_primitive_eq_owned_validated,
  [RC_BUILTIN_ADD]=rc_primitive_add_owned_validated,
  [RC_BUILTIN_SUB]=rc_primitive_sub_owned_validated,
  [RC_BUILTIN_MUL]=rc_primitive_mul_owned_validated,
  [RC_BUILTIN_DIV]=rc_primitive_div_owned_validated,
  [RC_BUILTIN_GT]=rc_primitive_gt_owned_validated,
  [RC_BUILTIN_LT]=rc_primitive_lt_owned_validated,
  [RC_BUILTIN_GE]=rc_primitive_ge_owned_validated,
  [RC_BUILTIN_LE]=rc_primitive_le_owned_validated,
  [RC_BUILTIN_CONS]=rc_primitive_cons_owned_validated
 };
 /* Failure goes straight to the slow dispatcher. Public tiny entry points do
  * the same, so rejected foreign/null arguments cannot recurse through tags. */
 int slot=rc_builtin_call_slot(c,fn,argc,args);
 return slot>=0 ? calls[(unsigned)slot](c,fn,args,tail)
                : primitive_call_owned_slow(c,fn,argc,args,tail);
}
RcValue *rc_assoc_try_owned(RcContext *c,RcValue *self,RcValue **args,
                            RcValue *const *deps,bool *handled) {
 *handled=false;
 /* The existing nil branch is already cheap; avoid dependency guards there,
  * including when a replacement equality changes that branch's meaning. */
 if(rc_is_nil(args[1]))return NULL;
 if(!rc_call_values_belong(c,self,2,args)||rc_arity(self)!=2)return NULL;
 RcValue *fn=rc_function_lookup(c,deps[0]);bool valid=fn==self;rc_release(fn);
 static const RcBuiltinSlot slots[]={RC_BUILTIN_EQ,RC_BUILTIN_CONSP,RC_BUILTIN_HD,RC_BUILTIN_TL};
 for(size_t i=0;valid&&i<4;i++) {
  fn=rc_function_lookup(c,deps[i+1]);valid=rc_builtin_identity_matches(c,slots[i],fn);rc_release(fn);
 }
 if(!valid)return NULL;
 /* All dependency calls are now pure canonical primitives. Cursor and keys
  * remain rooted by args; no callbacks can invalidate the entry guards.
  * A bounded prefix avoids adding an intrinsic-specific cyclic-list hang. */
 RcValue *cursor=args[1],*result=NULL;
 for(size_t steps=0;steps<4096;steps++) {
  if(rc_is_nil(cursor)){result=rc_nil();break;}
  if(rc_type(cursor)!=RC_PAIR)return NULL;
  RcValue *entry=rc_head_borrowed(cursor);
  if(rc_type(entry)==RC_PAIR) {
   bool equal=rc_equal(args[0],rc_head_borrowed(entry));
   if(rc_has_error(c)){*handled=true;break;}
   if(equal){result=rc_retain(entry);break;}
  }
  cursor=rc_tail_borrowed(cursor);
 }
 if(!result&&!*handled)return NULL;
 *handled=true;
 for(size_t i=0;i<2;i++){RcValue *v=args[i];args[i]=NULL;rc_release(v);}
 return result;
}
RcValue *rc_cons_call_reuse(RcContext *c,RcValue *function,
                            RcValue *candidate,RcValue *head,RcValue *tail,
                            bool tail_position) {
 if(rc_builtin_identity_matches(c,RC_BUILTIN_CONS,function))return rc_cons_reuse(c,candidate,head,tail);
 /* Reinstallation and equivalent original primitive snapshots retain the
  * previous classification, reuse counters, and error behavior. */
 if(rc_closure_is_native(function,primitive)&&rc_arity(function)==2) {
  RcValue *code=rc_capture(function,0);
  if(code&&rc_integer_value(code)==P_CONS)return rc_cons_reuse(c,candidate,head,tail);
 }
 RcValue *args[]={head,tail};
 RcValue *result=tail_position?rc_tailcall(c,function,2,args):rc_apply(c,function,2,args);
 rc_release(candidate);return result;
}
RcValue *rc_tail_call_reuse(RcContext *c,RcValue *function,
                            RcValue *candidate,RcValue **out) {
 if(!out){rc_release(candidate);return rc_error(c,"Missing reuse token slot");}
 *out=NULL;
 if(rc_builtin_identity_matches(c,RC_BUILTIN_TL,function))return rc_pair_take_tail_reuse(c,candidate,out);
 if(rc_closure_is_native(function,primitive)&&rc_arity(function)==1) {
  RcValue *code=rc_capture(function,0);
  if(code&&rc_integer_value(code)==P_TL)return rc_pair_take_tail_reuse(c,candidate,out);
 }
 RcValue *result=rc_apply(c,function,1,&candidate);
 if(result)*out=candidate;else rc_release(candidate);
 return result;
}
static bool install(RcContext *c,const struct Primitive *p,size_t n) {
 for(size_t i=0;i<n;i++){
  RcValue *code=rc_integer(c,p[i].op);if(!code)return false;
  RcValue *started=p[i].op==P_TIME?rc_number(c,(double)clock()/CLOCKS_PER_SEC):NULL;
  if(p[i].op==P_TIME&&!started){rc_release(code);return false;}
  RcValue *captures[]={code,started};
  RcValue *f=rc_closure(c,p[i].arity,primitive,started?2:1,captures);rc_release(code);rc_release(started);if(!f)return false;
  RcValue *s=rc_symbol(c,p[i].name);bool ok=s&&rc_function_set(c,s,f);
  if(ok) {
   switch(p[i].op) {
    case P_HD:rc_builtin_cache_set(c,RC_BUILTIN_HD,f);break;
    case P_TL:rc_builtin_cache_set(c,RC_BUILTIN_TL,f);break;
    case P_CONSP:rc_builtin_cache_set(c,RC_BUILTIN_CONSP,f);break;
    case P_NUMBER:rc_builtin_cache_set(c,RC_BUILTIN_NUMBERP,f);break;
    case P_STRING:rc_builtin_cache_set(c,RC_BUILTIN_STRINGP,f);break;
    case P_EQ:rc_builtin_cache_set(c,RC_BUILTIN_EQ,f);break;
    case P_CONS:rc_builtin_cache_set(c,RC_BUILTIN_CONS,f);break;
    case P_ADD:rc_builtin_cache_set(c,RC_BUILTIN_ADD,f);break;
    case P_SUB:rc_builtin_cache_set(c,RC_BUILTIN_SUB,f);break;
    case P_MUL:rc_builtin_cache_set(c,RC_BUILTIN_MUL,f);break;
    case P_DIV:rc_builtin_cache_set(c,RC_BUILTIN_DIV,f);break;
    case P_GT:rc_builtin_cache_set(c,RC_BUILTIN_GT,f);break;
    case P_LT:rc_builtin_cache_set(c,RC_BUILTIN_LT,f);break;
    case P_GE:rc_builtin_cache_set(c,RC_BUILTIN_GE,f);break;
    case P_LE:rc_builtin_cache_set(c,RC_BUILTIN_LE,f);break;
    default:break;
   }
  }
  rc_release(s);rc_release(f);if(!ok)return false;
 }return true;
}
bool rc_install_overwrites(RcContext *c){return install(c,overwrites,sizeof overwrites/sizeof *overwrites);}
bool rc_install_primitives(RcContext *c){
 struct utsname host;
 if(uname(&host)!=0){rc_error(c,"Cannot determine host OS: %s",strerror(errno));return false;}
 if(!install(c,core,sizeof core/sizeof *core)||!rc_install_overwrites(c))return false;
 if(!put_global(c,"*os*",rc_string(c,host.sysname))||!put_global(c,"*language*",rc_symbol(c,"C"))||!put_global(c,"*implementation*",rc_symbol(c,"C"))||
    !put_global(c,"*home-directory*",rc_string(c,""))||!put_global(c,"*release*",rc_number(c,0.1))||
    !put_global(c,"*port*",rc_string(c,"Shen/C RC"))||!put_global(c,"*porters*",rc_string(c,"Tatsuya Tsuda; RC backend contributors"))||
    !put_global(c,"*stinput*",rc_stream(c,stdin,false))||!put_global(c,"*stoutput*",rc_stream(c,stdout,false))||
    !put_global(c,"*sterror*",rc_stream(c,stderr,false))||!put_global(c,"*hush*",rc_false(c))||
    !put_global(c,"*argv*",rc_nil()))return false;
 return true;
}

static bool print_value(FILE *f,RcValue *v,unsigned depth) {
 if(depth>256)return fputs("#<depth-limit>",f)>=0;
 switch(rc_type(v)){
 case RC_NIL:return fputs("()",f)>=0;
 case RC_SYMBOL:return fwrite(rc_text(v),1,rc_text_length(v),f)==rc_text_length(v);
 case RC_STRING:
  return fputc('"',f)!=EOF&&fwrite(rc_text(v),1,rc_text_length(v),f)==rc_text_length(v)&&fputc('"',f)!=EOF;
 case RC_NUMBER:
  if(rc_number_is_exact_integer(v))return fprintf(f,"%" PRId64,rc_integer_value(v))>=0;
  {char buf[160];snprintf(buf,sizeof buf,"%.*g",DBL_DIG,rc_number_value(v));
   if(!strchr(buf,'.')&&!strchr(buf,'e')&&isfinite(rc_number_value(v)))strcat(buf,".0");return fputs(buf,f)>=0;}
 case RC_PAIR:{
  if(fputc('(',f)==EOF)return false;RcValue *p=v;size_t count=0;
  while(rc_type(p)==RC_PAIR){if(count++&&fputc(' ',f)==EOF)return false;
   if(count>100000)return fputs("...)",f)>=0;
   if(!print_value(f,rc_head_borrowed(p),depth+1))return false;p=rc_tail_borrowed(p);}
  if(!rc_is_nil(p)&&(fputs(" | ",f)<0||!print_value(f,p,depth+1)))return false;return fputc(')',f)!=EOF;}
 case RC_VECTOR:return fprintf(f,"#<Absvector %zu>",rc_vector_length(v))>=0;
 case RC_CLOSURE:return fprintf(f,"#<Function {%p}>",(void*)v)>=0;
 case RC_STREAM:return fprintf(f,"#<Stream {%p} %s>",(void*)v,rc_stream_is_input(v)?"in":"out")>=0;
 case RC_TAIL:return fputs("#<tail-call>",f)>=0;
 case RC_ENV:return fputs("#<internal-environment>",f)>=0;
 }
 return false;
}
bool rc_print(FILE *out,RcValue *value){return out&&value&&print_value(out,value,0)&&!ferror(out);}
