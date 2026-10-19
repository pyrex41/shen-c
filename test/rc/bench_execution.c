#define _POSIX_C_SOURCE 200809L
/* Profile already-loaded code. Setup and a complete first call precede timing.
 * Both modes use the same kernel, inputs, result checks and borrowed call ABI. */
#include "rc.h"
#include "eval.h"
#include "read.h"
#include "rc_internal.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

extern bool rc_bootstrap_kernel(RcContext *ctx);
static double wall(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static double seconds(struct timeval t){return t.tv_sec+t.tv_usec*1e-6;}
static RcValue *function(RcContext *c,const char *name){
 RcValue *s=rc_symbol(c,name),*f=s?rc_function_get(c,s):NULL;rc_release(s);return f;
}
static RcValue *call(RcContext *c,const char *name,size_t n,RcValue **args){
 RcValue *f=function(c,name),*v=f?rc_apply(c,f,n,args):NULL;rc_release(f);return v;
}
static bool kl(RcContext *c,const char *source){
 RcValue *form=rc_read(c,&source),*v=form?rc_eval(c,form):NULL;rc_release(form);
 bool ok=v!=NULL;rc_release(v);return ok;
}
static bool shen(RcContext *c,const char *source){
 RcValue *s=rc_string(c,source),*forms=s?call(c,"read-from-string",1,&s):NULL;rc_release(s);
 if(!forms)return false;bool ok=true;
 for(RcValue *rest=forms;rc_type(rest)==RC_PAIR;rest=rc_tail_borrowed(rest)){
  RcValue *form=rc_head_borrowed(rest),*v=call(c,"eval",1,&form);if(!v){ok=false;break;}rc_release(v);
 }
 rc_release(forms);return ok;
}
static bool load(RcContext *c,const char *filename){
 RcValue *s=rc_string(c,filename),*v=s?call(c,"load",1,&s):NULL;rc_release(s);
 bool ok=v!=NULL;rc_release(v);return ok;
}
int main(int argc,char **argv){
 if(argc!=4){fprintf(stderr,"usage: %s CASE REPETITIONS PINNED_SUITE_DIRECTORY\n",argv[0]);return 2;}
 char *end=NULL;unsigned long repetitions=strtoul(argv[2],&end,10);
 if(!*argv[2]||*end||!repetitions||repetitions>1000000||chdir(argv[3]))return 2;
 double start=wall();RcContext *c=rc_context_new();if(!c)return 1;
 RcValue *f=NULL,*expected=NULL,*value=NULL;int status=1;
 if(!rc_bootstrap_kernel(c)||!kl(c,"(set *hush* true)")||!kl(c,"(tc -)"))goto done;
 const char *expected_text=NULL;
 if(!strcmp(argv[1],"tail-count")){
  if(!kl(c,"(defun rc-bench-count (N A) (if (= N 0) A (rc-bench-count (- N 1) (+ A N))))")||
     !kl(c,"(defun rc-bench-run () (rc-bench-count 100000 0))"))goto done;
  expected_text="5000050000";
 }else if(!strcmp(argv[1],"list-build")){
  if(!kl(c,"(defun rc-bench-build (N A) (if (= N 0) A (rc-bench-build (- N 1) (cons N A))))")||
     !kl(c,"(defun rc-bench-run () (length (rc-bench-build 10000 ())))"))goto done;
  expected_text="10000";
 }else if(!strcmp(argv[1],"queens")){
  if(!load(c,"n queens.shen")||!kl(c,"(defun rc-bench-run () (length (n-queens 6)))"))goto done;
  expected_text="4";
 }else if(!strcmp(argv[1],"prolog")){
  if(!load(c,"nreverseprolog.shen")||!shen(c,"(define rc-bench-run -> (prolog? (nreverse [1 2 3 4 5 6 7 8 9 10 11 12] Out) (return Out)))"))goto done;
  expected_text="(12 11 10 9 8 7 6 5 4 3 2 1)";
 }else if(!strcmp(argv[1],"typecheck")){
  if(!kl(c,"(defun rc-bench-run () (shen.typecheck (quote (+ 1 (* 2 3))) (quote number)))"))goto done;
  expected_text="number";
 }else{fprintf(stderr,"unknown execution case\n");goto done;}
 expected=rc_read(c,&expected_text);f=function(c,"rc-bench-run");
 if(!expected||!f)goto done;
 value=rc_apply(c,f,0,NULL);if(!value||!rc_equal(value,expected))goto done;rc_release(value);value=NULL;
 /* Lazy compilation can publish a new native binding on the first call. */
 rc_release(f);f=function(c,"rc-bench-run");if(!f)goto done;
 double setup=wall()-start;RcStats before=rc_stats(c);size_t interpreted=rc_interpreter_entries(c);
 struct rusage cpu_before,cpu_after,child_before,child_after;
 getrusage(RUSAGE_SELF,&cpu_before);getrusage(RUSAGE_CHILDREN,&child_before);start=wall();
 fprintf(stderr,"RC_EXEC_BEGIN case=%s repetitions=%lu\n",argv[1],repetitions);fflush(stderr);
 for(unsigned long i=0;i<repetitions;i++){
  value=rc_apply(c,f,0,NULL);if(!value||!rc_equal(value,expected))goto done;
  rc_release(value);value=NULL;
 }
 double elapsed=wall()-start;getrusage(RUSAGE_SELF,&cpu_after);getrusage(RUSAGE_CHILDREN,&child_after);
 RcStats after=rc_stats(c);size_t interpreter_delta=rc_interpreter_entries(c)-interpreted;
 double child=seconds(child_after.ru_utime)-seconds(child_before.ru_utime)+seconds(child_after.ru_stime)-seconds(child_before.ru_stime);
 fprintf(stderr,"RC_EXEC_END case=%s\n",argv[1]);
 printf("RC_EXEC_BENCH {\"case\":\"%s\",\"repetitions\":%lu,\"setup_wall\":%.9f,\"wall\":%.9f,\"user\":%.9f,\"system\":%.9f,\"compiler_child_cpu\":%.9f,\"interpreter_entries\":%zu,\"retain_calls\":%zu,\"release_calls\":%zu,\"system_allocations\":%zu,\"pair_reuses\":%zu,\"collections\":%zu,\"cycle_objects_freed\":%zu}\n",
 argv[1],repetitions,setup,elapsed,seconds(cpu_after.ru_utime)-seconds(cpu_before.ru_utime),seconds(cpu_after.ru_stime)-seconds(cpu_before.ru_stime),child,interpreter_delta,
 after.retain_calls-before.retain_calls,after.release_calls-before.release_calls,after.system_allocations-before.system_allocations,after.pair_reuses-before.pair_reuses,after.collections-before.collections,after.cycle_objects_freed-before.cycle_objects_freed);
 bool native=!getenv("SHEN_RC_EXECUTION")||strcmp(getenv("SHEN_RC_EXECUTION"),"interpret");
 if(child!=0||(native&&interpreter_delta))goto done;
 status=0;
done:
 if(status)fprintf(stderr,"execution benchmark failed: %s\n",rc_error_message(c));
 rc_release(value);rc_release(expected);rc_release(f);rc_context_free(c);return status;
}
