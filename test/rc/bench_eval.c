#include "eval.h"
#include "primitives.h"
#include "read.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static RcValue *parse(RcContext *c,const char *text) {
 RcValue *value=rc_read(c,&text);if(!value||rc_has_error(c))abort();return value;
}
int main(int argc,char **argv) {
 if(argc<2)return 2;
 int lower=strstr(argv[1],"lower")!=NULL;
 size_t n=argc>2?(size_t)strtoull(argv[2],NULL,10):30000;
 RcContext *c=rc_context_new();if(!c||!rc_install_primitives(c)||!rc_install_eval(c))abort();
 if(!strncmp(argv[1],"lookup",6)) {
  RcValue *key=rc_symbol(c,lower?"local":"Local"),*value=rc_integer(c,42),*env=rc_nil();
  for(size_t i=0;i<128;i++){RcValue *next=rc_env_frame(c,env,1,&key,&value);if(!next)abort();rc_release(env);env=next;}
  RcValue *missing=rc_symbol(c,"global" );
  for(size_t i=0;i<n;i++)if(rc_env_lookup(env,missing))abort();
  rc_release(env);rc_release(key);rc_release(value);rc_release(missing);
 } else {
  char body[16000],next[16000],definition[18000],call[200];
  const char *number=lower?"n":"N",*list=lower?"x":"X";
  snprintf(body,sizeof(body),"(walk (- %s 1) %s)",number,list);
  for(int i=15;i>=0;i--){
   snprintf(next,sizeof(next),"(let %c%d (if (cons? %s) (hd %s) 0) %s)",lower?'v':'V',i,list,list,body);
   strcpy(body,next);
  }
  snprintf(definition,sizeof(definition),"(defun walk (%s %s) (if (= %s 0) (hd %s) %s))",number,list,number,list,body);
  RcValue *ast=parse(c,definition),*out=rc_eval(c,ast);if(!out)abort();rc_release(out);rc_release(ast);
  snprintf(call,sizeof(call),"(walk %zu (cons 42 ()))",n);ast=parse(c,call);
  out=rc_eval(c,ast);if(!out||rc_integer_value(out)!=42||rc_has_error(c))abort();
  rc_release(out);rc_release(ast);
 }
 RcStats s=rc_stats(c);
 printf("%s %zu retains=%zu releases=%zu allocations=%zu\n",argv[1],n,s.retain_calls,s.release_calls,s.total_allocations);
 rc_context_free(c);return 0;
}
