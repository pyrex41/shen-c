#define _POSIX_C_SOURCE 200809L
#include "rc.h"
#include "rc_internal.h"
#include "read.h"
#include "eval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern bool rc_bootstrap_kernel(RcContext *);
static void fail(RcContext *c,const char *message) {
 fprintf(stderr,"%s: %s\n",message,rc_error_message(c));exit(1);
}
static RcValue *function(RcContext *c,const char *name) {
 RcValue *s=rc_symbol(c,name),*f=s?rc_function_get(c,s):NULL;rc_release(s);
 if(!f)fail(c,"function lookup");return f;
}
static void definitions(RcContext *c,const char *path) {
 FILE *f=fopen(path,"rb");if(!f)fail(c,"open definitions");
 if(fseek(f,0,SEEK_END))fail(c,"seek definitions");long n=ftell(f);
 if(n<0||n>4*1024*1024||fseek(f,0,SEEK_SET))fail(c,"definition size");
 char *data=malloc((size_t)n+1);if(!data||fread(data,1,(size_t)n,f)!=(size_t)n)fail(c,"read definitions");
 fclose(f);data[n]=0;const char *cursor=data;RcValue *form;
 while((form=rc_read_escaped(c,&cursor))) {
  RcValue *value=rc_eval(c,form);rc_release(form);
  if(!value)fail(c,"evaluate definitions");rc_release(value);
 }
 free(data);if(rc_has_error(c))fail(c,"parse definitions");
}
static double cpu(void) {
 struct timespec t;if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t))exit(1);
 return (double)t.tv_sec+(double)t.tv_nsec/1e9;
}
int main(int argc,char **argv) {
 if(argc!=5)return 2;
 const char *mode=argv[2];size_t n=(size_t)strtoull(argv[3],NULL,10),iterations=(size_t)strtoull(argv[4],NULL,10);
 if(!n||n>8192||!iterations||iterations>1000000)return 2;
 bool concat=!strcmp(mode,"concat"),scan=!strcmp(mode,"string-scan"),rebuild=!strcmp(mode,"string-rebuild"),build=!strcmp(mode,"string-build"),vscan=!strcmp(mode,"vector-scan"),vbuild=!strcmp(mode,"vector-build");
 if(!(concat||scan||rebuild||build||vscan||vbuild)||((rebuild||build||vbuild)&&n>512)||(vscan&&n>2048))return 2;
 RcContext *c=rc_context_new();if(!c||!rc_bootstrap_kernel(c))fail(c,"bootstrap");definitions(c,argv[1]);
 const char *name=concat?"cn":scan?"fusion-count-string":rebuild?"fusion-rebuild-string":build?"fusion-build-string":vscan?"fusion-count-vector":"fusion-build-vector";
 RcValue *fn=function(c,name),*input=NULL;char *bytes=malloc(n);if(!bytes)fail(c,"input memory");
 for(size_t i=0;i<n;i++)bytes[i]=(char)('a'+i%26);
 if(concat||scan||rebuild)input=rc_string_n(c,bytes,n);
 else if(vscan) {
  RcValue *make=function(c,"vector"),*a[]={rc_integer(c,(int64_t)n)};
  input=rc_apply(c,make,1,a);rc_release(make);
  for(size_t i=1;input&&i<=n;i++)if(!rc_vector_set(c,input,i,rc_integer(c,7)))fail(c,"input vector");
 } else input=rc_integer(c,(int64_t)n);
 if(!input)fail(c,"input");
 RcValue *args[]={input,(scan||vscan)?rc_integer(c,0):input};size_t arity=(concat||scan||vscan)?2:1;
 /* Preparation warmup is explicit and outside counts/timing. */
 for(size_t k=0;k<4;k++) {
  RcValue *warm=rc_apply(c,fn,arity,args);if(!warm)fail(c,"warmup");rc_release(warm);
 }
 rc_collect(c);rc_fusion_reset(c);RcStats before=rc_stats(c);double start=cpu();
 for(size_t k=0;k<iterations;k++) {
  RcValue *result=rc_apply(c,fn,arity,args);if(!result)fail(c,"probe");
  if((scan||vscan)&&(!rc_number_is_int64(result)||rc_integer_value(result)!=(int64_t)n))fail(c,"scan oracle");
  if((concat||rebuild||build)&&(rc_type(result)!=RC_STRING||rc_text_length(result)!=(concat?2*n:n)))fail(c,"string oracle");
  if(concat&&(memcmp(rc_text(result),bytes,n)||memcmp(rc_text(result)+n,bytes,n)))fail(c,"concat oracle");
  if(rebuild&&memcmp(rc_text(result),bytes,n))fail(c,"rebuild oracle");
  if(build)for(size_t i=0;i<n;i++)if(rc_text(result)[i]!='a')fail(c,"build oracle");
  if(vbuild&&(rc_type(result)!=RC_VECTOR||rc_vector_length(result)!=n+1))fail(c,"vector oracle");
  if(vbuild)for(size_t i=0;i<=n;i++) {
   RcValue *item=rc_vector_slot_borrowed(result,i);
   if(!rc_number_is_int64(item)||rc_integer_value(item)!=(int64_t)(i?7:n))fail(c,"vector contents");
  }
  rc_release(result);
 }
 double seconds=cpu()-start;RcStats after=rc_stats(c);RcFusionStats stats=rc_fusion_stats(c);
 printf("{\"mode\":\"%s\",\"size\":%zu,\"iterations\":%zu,\"cpu_seconds\":%.9f,\"allocations\":%zu,\"retains\":%zu,\"releases\":%zu,\"counters\":{",mode,n,iterations,seconds,after.total_allocations-before.total_allocations,after.retain_calls-before.retain_calls,after.release_calls-before.release_calls);
 for(size_t i=0;i<RC_FUSION_COUNT;i++)printf("%s\"%s\":%zu",i?",":"",rc_fusion_name(i),stats.count[i]);
 puts("}}");rc_release(input);rc_release(fn);free(bytes);rc_context_free(c);return 0;
}
