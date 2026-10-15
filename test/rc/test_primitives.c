#include "primitives.h"
#ifdef NDEBUG
#undef NDEBUG /* Test checks and their setup must run in Release builds. */
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static RcValue *call(RcContext *c,const char *name,size_t n,RcValue **args) {
 RcValue *s=rc_symbol(c,name);assert(s);RcValue *f=rc_function_get(c,s);rc_release(s);assert(f);
 RcValue *v=rc_apply(c,f,n,args);rc_release(f);return v;
}
static RcValue *fallback(RcContext *c,RcValue *s,size_t n,RcValue *const *a){(void)s;(void)n;(void)a;return rc_integer(c,42);}
int main(void){
 RcContext *c=rc_context_new();assert(c&&rc_install_primitives(c));
 RcValue *osname=rc_symbol(c,"*os*");RcValue *os=rc_global_get(c,osname);assert(os&&rc_type(os)==RC_STRING&&rc_text_length(os));rc_release(osname);rc_release(os);
 RcValue *a=rc_integer(c,7),*b=rc_integer(c,2),*args[]={a,b},*v;
 v=call(c,"do",2,args);assert(v==b);rc_release(v);
 v=call(c,"/",2,args);assert(v&&rc_number_value(v)==3.5);rc_release(v);
 v=call(c,"+",2,args);assert(v&&rc_integer_value(v)==9);rc_release(v);
 v=call(c,"=",2,args);assert(v&&!rc_is_true(v));rc_release(v);args[1]=a;v=call(c,"=",2,args);assert(v&&rc_is_true(v));rc_release(v);args[1]=b;
 RcValue *floating=rc_number(c,2.0);args[1]=floating;
 v=call(c,"+",2,args);assert(v&&rc_number_value(v)==9&&!rc_number_is_exact_integer(v));rc_release(v);rc_release(floating);
 RcValue *huge=rc_number(c,1e20);args[1]=huge;
 v=call(c,"/",2,args);assert(v&&rc_number_value(v)==7e-20);rc_release(v);rc_release(huge);args[1]=b;
 RcValue *zero=rc_integer(c,0);args[1]=zero;assert(!call(c,"/",2,args)&&rc_has_error(c));rc_clear_error(c);rc_release(zero);args[1]=b;
 RcValue *max=rc_integer(c,INT64_MAX);args[0]=max;assert(!call(c,"+",2,args)&&rc_has_error(c));rc_clear_error(c);rc_release(max);args[0]=a;
 for(int i=0;i<256;i++){
  RcValue *num=rc_integer(c,i),*one[]={num};RcValue *str=call(c,"n->string",1,one);assert(str&&rc_text_length(str)==1);
  one[0]=str;v=call(c,"string->n",1,one);assert(v&&rc_integer_value(v)==i);rc_release(v);rc_release(str);rc_release(num);
 }
 RcValue *text=rc_string_n(c,"a\0b",3),*index=rc_integer(c,1);args[0]=text;args[1]=index;
 v=call(c,"pos",2,args);assert(v&&rc_text_length(v)==1&&rc_text(v)[0]==0);rc_release(v);
 v=call(c,"tlstr",1,args);assert(v&&rc_text_length(v)==2&&rc_text(v)[0]==0);rc_release(v);
 args[1]=text;v=call(c,"cn",2,args);assert(v&&rc_text_length(v)==6);rc_release(v);
 RcValue *pair=rc_cons(c,a,rc_nil());args[0]=pair;v=call(c,"hd",1,args);assert(v==a);rc_release(v);rc_release(pair);
 RcValue *sym=rc_symbol(c,"*primitive-test*"),*setargs[]={sym,text};v=call(c,"set",2,setargs);assert(v==text);rc_release(v);
 v=call(c,"value",1,setargs);assert(v==text);rc_release(v);
 RcValue *size=rc_integer(c,2),*one[]={size};RcValue *vec=call(c,"absvector",1,one);assert(vec);
 RcValue *put[]={vec,index,text};v=call(c,"address->",3,put);assert(v==vec);rc_release(v);
 v=call(c,"<-address",2,put);assert(v==text);rc_release(v);
 RcValue *thunk=rc_closure(c,0,fallback,0,NULL),*bad=rc_integer(c,5),*orargs[]={vec,bad,thunk};
 v=call(c,"<-address/or",3,orargs);assert(v&&rc_integer_value(v)==42);rc_release(v);
 RcValue *missing=rc_symbol(c,"*missing-test*"),*varor[]={missing,thunk};v=call(c,"value/or",2,varor);assert(v&&rc_integer_value(v)==42&&!rc_has_error(c));rc_release(v);
 FILE *file=tmpfile();assert(file);RcValue *stream=rc_stream_with_direction(c,file,true,false),*outargs[]={text,stream};
 v=call(c,"shen.write-string",2,outargs);assert(v==text);rc_release(v);rewind(file);char bytes[3];assert(fread(bytes,1,3,file)==3&&!memcmp(bytes,"a\0b",3));
 assert(!call(c,"read-byte",1,&stream)&&rc_has_error(c));rc_clear_error(c);
 v=call(c,"close",1,&stream);assert(v&&rc_is_nil(v));rc_release(v);
 assert(!call(c,"shen.write-string",2,outargs)&&rc_has_error(c));rc_clear_error(c);
 FILE *input=tmpfile();assert(input);fputc(255,input);rewind(input);RcValue *in=rc_stream_with_direction(c,input,true,true);
 v=call(c,"read-byte",1,&in);assert(v&&rc_integer_value(v)==255);rc_release(v);v=call(c,"read-byte",1,&in);assert(v&&rc_integer_value(v)==-1);rc_release(v);
 RcValue *err=rc_string(c,"expected error");assert(!call(c,"simple-error",1,&err)&&!strcmp(rc_error_message(c),"expected error"));rc_clear_error(c);
 v=call(c,"error-to-string",1,&err);assert(v==err);rc_release(v);
 FILE *repr=tmpfile();assert(repr&&rc_print(repr,text));rewind(repr);char printed[5];assert(fread(printed,1,5,repr)==5&&!memcmp(printed,"\"a\0b\"",5));fclose(repr);
 rc_release(a);rc_release(b);rc_release(text);rc_release(index);rc_release(sym);rc_release(size);rc_release(vec);rc_release(thunk);rc_release(bad);rc_release(missing);rc_release(stream);rc_release(in);rc_release(err);
 rc_collect(c);assert(!rc_has_error(c));rc_context_free(c);puts("RC primitives: PASS");return 0;
}
