#include "eval.h"
#include "primitives.h"
#include "read.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t callbacks;
static RcValue *effect(RcContext *c,RcValue *self,size_t n,RcValue *const *args) {
 (void)c;(void)self;if(n!=1)abort();callbacks++;return rc_retain(args[0]);
}
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 size_t n=(size_t)strtoull(argv[2],NULL,10);
 RcContext *c=rc_context_new();if(!c||!rc_install_primitives(c)||!rc_install_eval(c))abort();
 RcValue *name=rc_symbol(c,"callee-effect"),*fn=rc_closure(c,1,effect,0,NULL);
 if(!name||!fn||!rc_function_set(c,name,fn))abort();rc_release(name);rc_release(fn);
 const char *text=!strcmp(argv[1],"atomic")?"(cons? ())":
  !strcmp(argv[1],"effect")?"(cons? ((if true callee-effect missing) ()))":
  !strcmp(argv[1],"lexical")?"(let F cons? (F ()))":
  !strcmp(argv[1],"computed")?"((if true cons? missing) ())":
  !strcmp(argv[1],"user")?"(callee-effect ())":NULL;
 if(!text)return 2;
 RcValue *form=rc_read(c,&text);if(!form||rc_has_error(c))abort();
 RcStats before=rc_stats(c);
 for(size_t i=0;i<n;i++){RcValue *out=rc_eval(c,form);if(!out||rc_has_error(c))abort();rc_release(out);}
 RcStats after=rc_stats(c);
 printf("%s %zu retains=%zu releases=%zu allocations=%zu callbacks=%zu\n",argv[1],n,
  after.retain_calls-before.retain_calls,after.release_calls-before.release_calls,
  after.total_allocations-before.total_allocations,callbacks);
 rc_release(form);rc_context_free(c);return 0;
}
