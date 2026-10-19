#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "native_compile.h"
#include "emit.h"
#include "rc_internal.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#include "tier_config.h" /* embedded headers exceed the C99 minimum */
#pragma GCC diagnostic pop
#include <dlfcn.h>
#include <dirent.h>
#include <signal.h>
#include <math.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

typedef struct {const char *key,*source;size_t length;RcValue *(*entry)(RcContext *,RcValue *);} RcPackedEntry;
typedef const RcPackedEntry *(*RcBundleLookup)(const char *,const char *,size_t);
typedef struct Module { struct Module *next; char key[33]; char *source; size_t length; bool owns_source; void *handle; RcValue *(*entry)(RcContext *,RcValue *); } Module;
typedef struct {
 RcContext *ctx; char *cc,*cflags,*cache,*include; char abi[33];
 bool cache_owned,stats; Module *modules;
 void *bundle_handle; RcBundleLookup bundle_lookup; size_t bundle_hits;
 size_t lazy_closures,templates,definitions,evaluations,compiles,hits,memory_hits,emitted_bytes,failures;
 double emit_s,compile_s,compile_user_s,compile_system_s,load_s,timeout;
 size_t cache_limit;
} Tier;
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9;
}
static void digest(const char *a,size_t na,const char *b,size_t nb,char out[33]) {
    uint64_t h1=UINT64_C(14695981039346656037),h2=UINT64_C(0x84222325cbf29ce4);
    for(size_t i=0;i<na+nb;i++) {
        unsigned char c=(unsigned char)(i<na?a[i]:b[i-na]);
        h1=(h1^c)*UINT64_C(1099511628211); h2=(h2^c^(h2>>29))*UINT64_C(0x100000001b3);
    }
    snprintf(out,33,"%016llx%016llx",(unsigned long long)h1,(unsigned long long)h2);
}
static int mkdirs(const char *path) {
    char b[4096]; size_t n=strlen(path); if(!n||n>=sizeof(b))return -1;
    memcpy(b,path,n+1);
    for(char *p=b+1;*p;p++) if(*p=='/') { *p=0; if(mkdir(b,0700)&&errno!=EEXIST)return -1; *p='/'; }
    return mkdir(b,0700)&&errno!=EEXIST?-1:0;
}
static bool write_file(const char *path,const char *text,size_t n) {
    char tmp[4200]; snprintf(tmp,sizeof(tmp),"%s.%ld.XXXXXX",path,(long)getpid());
    int fd=mkstemp(tmp); if(fd<0)return false;
    bool ok=true;
    for(size_t off=0;ok && off<n;) { ssize_t w=write(fd,text+off,n-off); if(w<=0)ok=false; else off+=(size_t)w; }
    if(close(fd))ok=false;
    if(ok && rename(tmp,path))ok=false;
    if(!ok)unlink(tmp);
    return ok;
}
static bool same_file(const char *path,const char *text,size_t n) {
    FILE *f=fopen(path,"rb"); if(!f)return false;
    bool same=true; char buf[8192]; size_t off=0,r;
    while(same && (r=fread(buf,1,sizeof(buf),f))>0) {
        if(off+r>n || memcmp(buf,text+off,r))same=false; off+=r;
    }
    same=same && !ferror(f) && off==n; fclose(f); return same;
}
static char *join(const char *a,const char *b) {
    size_t na=strlen(a),nb=strlen(b); char *s=malloc(na+nb+1);
    if(s) { memcpy(s,a,na); memcpy(s+na,b,nb+1); } return s;
}
static char *path_of(const char *dir,const char *key,const char *suffix) {
    size_t n=strlen(dir)+strlen(key)+strlen(suffix)+2; char *s=malloc(n);
    if(s)snprintf(s,n,"%s/%s%s",dir,key,suffix); return s;
}
/* Whitespace-split words appended to argv; returns the new count. */
static size_t words(char *text,char **argv,size_t n,size_t cap) {
    for(char *p=text;*p && n<cap;) {
        while(*p==' '||*p=='\t')*p++=0;
        if(!*p)break;
        argv[n++]=p; while(*p && *p!=' ' && *p!='\t')p++;
    }
    return n;
}
static void compiler_usage(Tier *t,const struct rusage *before){
 struct rusage after;getrusage(RUSAGE_CHILDREN,&after);
 t->compile_user_s+=(after.ru_utime.tv_sec-before->ru_utime.tv_sec)+(after.ru_utime.tv_usec-before->ru_utime.tv_usec)*1e-6;
 t->compile_system_s+=(after.ru_stime.tv_sec-before->ru_stime.tv_sec)+(after.ru_stime.tv_usec-before->ru_stime.tv_usec)*1e-6;
}
/* Returns 0 on success, 127 when the compiler could not be executed. */
static int run_compiler(Tier *t,const char *source,const char *object,const char *log) {
    char *flags=join(t->cflags," " RC_TIER_HOST_FLAGS); if(!flags)return 1;
    char *argv[96]; size_t n=0; char incflag[4200];
    argv[n++]=t->cc;
    n=words(flags,argv,n,80);
    snprintf(incflag,sizeof(incflag),"-I%s",t->include);
    /* The module must use this host's value representation and policy,
     * whatever flags the host was built with. */
    argv[n++]=SHEN_RC_IMMEDIATE_INTEGERS ? "-DSHEN_RC_IMMEDIATE_INTEGERS=1" : "-DSHEN_RC_IMMEDIATE_INTEGERS=0";
#ifdef SHEN_RC_ACYCLIC_ONLY
    argv[n++]="-DSHEN_RC_ACYCLIC_ONLY";
#endif
    argv[n++]="-std=c17"; argv[n++]="-w"; argv[n++]="-fPIC"; argv[n++]="-shared";
    argv[n++]="-fvisibility=hidden";
#ifdef __APPLE__
    argv[n++]="-undefined"; argv[n++]="dynamic_lookup";
#endif
    argv[n++]=incflag; argv[n++]=(char*)source; argv[n++]="-o"; argv[n++]=(char*)object; argv[n]=NULL;
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions,0,"/dev/null",O_RDONLY,0);
    posix_spawn_file_actions_addopen(&actions,1,log,O_WRONLY|O_CREAT|O_TRUNC,0600);
    posix_spawn_file_actions_adddup2(&actions,1,2);
    struct rusage usage;getrusage(RUSAGE_CHILDREN,&usage);
    pid_t pid; int status=0;
    posix_spawnattr_t attr;posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP);posix_spawnattr_setpgroup(&attr,0);
    int spawn=posix_spawnp(&pid,t->cc,&actions,&attr,argv,environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions); free(flags);
    if(spawn)return 127;
    double deadline=now()+t->timeout;
    for(;;){
      pid_t got=waitpid(pid,&status,WNOHANG);
      if(got==pid)break;
      if(got<0&&errno!=EINTR)return 1;
      struct stat info;bool too_large=!stat(log,&info)&&info.st_size>8*1024*1024;
      if(now()>=deadline||too_large){
       kill(-pid,SIGKILL);kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}
       compiler_usage(t,&usage);if(too_large)(void)truncate(log,8*1024*1024);return too_large?125:124;
      }
      struct timespec pause={0,10000000};nanosleep(&pause,NULL);
    }
    compiler_usage(t,&usage);
    if(!WIFEXITED(status))return 1;
    return WEXITSTATUS(status);
}
static bool private_directory(const char *path) {
    struct stat st;
    return !lstat(path,&st) && S_ISDIR(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode&(S_IWGRP|S_IWOTH));
}
static bool prepare_include(Tier *t) {
    t->include=path_of(t->cache,"include-",t->abi);
    if(!t->include || mkdirs(t->include) || !private_directory(t->include))return false;
    for(size_t i=0;i<sizeof(rc_tier_headers)/sizeof(*rc_tier_headers);i++) {
        char *file=path_of(t->include,rc_tier_headers[i].name,"");
        size_t n=strlen(rc_tier_headers[i].text);
        bool ok=file && (same_file(file,rc_tier_headers[i].text,n) || write_file(file,rc_tier_headers[i].text,n));
        free(file); if(!ok)return false;
    }
    return true;
}
static char *env_or(const char *name,const char *fallback) {
    const char *v=getenv(name); return strdup(v && *v ? v : fallback);
}

/* Code objects remain mapped for process lifetime. This state owns no managed
 * values: live literals belong to the normal closure graph and cycle collector. */
void rc_native_compile_free(void *state) {
 Tier *t=state;if(!t)return;
 RcStats runtime=rc_stats(t->ctx);
 if(t->stats)fprintf(stderr,"RC_NATIVE lazy_closures=%zu templates=%zu definitions=%zu evaluations=%zu compiles=%zu disk_hits=%zu bundle_hits=%zu memory_hits=%zu emitted_bytes=%zu failures=%zu interpreter_entries=%zu emit_s=%.6f compile_s=%.6f compile_user_s=%.6f compile_system_s=%.6f load_s=%.6f retain_calls=%zu release_calls=%zu system_allocations=%zu pair_reuses=%zu collections=%zu cycle_objects_freed=%zu peak_objects=%zu live_bytes=%zu\n",t->lazy_closures,t->templates,t->definitions,t->evaluations,t->compiles,t->hits,t->bundle_hits,t->memory_hits,t->emitted_bytes,t->failures,rc_interpreter_entries(t->ctx),t->emit_s,t->compile_s,t->compile_user_s,t->compile_system_s,t->load_s,runtime.retain_calls,runtime.release_calls,runtime.system_allocations,runtime.pair_reuses,runtime.collections,runtime.cycle_objects_freed,runtime.peak_objects,runtime.live_bytes);
 while(t->modules){Module *m=t->modules;t->modules=m->next;if(m->owns_source)free(m->source);free(m);}
 if(t->cache_owned){
  if(t->include){for(size_t i=0;i<sizeof(rc_tier_headers)/sizeof(*rc_tier_headers);i++){char *f=path_of(t->include,rc_tier_headers[i].name,"");if(f)unlink(f);free(f);}rmdir(t->include);}
  DIR *d=opendir(t->cache);if(d){struct dirent *e;while((e=readdir(d)))if(e->d_name[0]!='.'){char *f=path_of(t->cache,e->d_name,"");if(f)unlink(f);free(f);}closedir(d);}rmdir(t->cache);
 }
 free(t->cc);free(t->cflags);free(t->cache);free(t->include);free(t);
}
static Tier *state_for(RcContext *ctx){
 Tier *t=*rc_native_compile_slot(ctx);if(t)return t;
 t=calloc(1,sizeof(*t));if(!t){rc_error(ctx,"native compiler: out of memory");return NULL;}
 t->ctx=ctx;const char *stats=getenv("SHEN_RC_NATIVE_STATS");t->stats=stats&&strcmp(stats,"0");
 *rc_native_compile_slot(ctx)=t;return t;
}
static bool object_receipt(const char *,const char *,bool);
static const char *const compiler_environment[]={"SDKROOT","MACOSX_DEPLOYMENT_TARGET","CPATH","C_INCLUDE_PATH","LIBRARY_PATH","DEVELOPER_DIR","NIX_CFLAGS_COMPILE","NIX_LDFLAGS","NIX_CFLAGS_LINK","NIX_CC","NIX_ENFORCE_PURITY"};
static void config_word(FILE *out,const char *name,const char *value){
 fprintf(out,"%s ",name);if(!value)fputc('-',out);
 else for(const unsigned char *p=(const unsigned char*)value;*p;p++)fprintf(out,"%02x",*p);
 fputc('\n',out);
}
static void save_configuration(Tier *t){
 char *text=NULL;size_t length=0;FILE *out=open_memstream(&text,&length);if(!out)return;
 fprintf(out,"SHEN_NATIVE_CONFIG_V1\nabi %s\nimmediate %d\nacyclic %d\n",t->abi,(int)SHEN_RC_IMMEDIATE_INTEGERS,
#ifdef SHEN_RC_ACYCLIC_ONLY
 1
#else
 0
#endif
 );
 config_word(out,"cc",t->cc);config_word(out,"cflags",t->cflags);config_word(out,"host_flags",RC_TIER_HOST_FLAGS);
 for(size_t i=0;i<sizeof(compiler_environment)/sizeof(*compiler_environment);i++){
  char name[128];snprintf(name,sizeof(name),"env_%s",compiler_environment[i]);config_word(out,name,getenv(compiler_environment[i]));
 }
 if(fclose(out)){free(text);return;}
 char stem[64];snprintf(stem,sizeof(stem),"compile-%s",t->abi);char *path=path_of(t->cache,stem,".config");
 if(path&&!same_file(path,text,length))(void)write_file(path,text,length);
 free(path);free(text);
}
static void load_bundle(Tier *t){
 const char *mode=getenv("SHEN_RC_NATIVE_BUNDLE");if(mode&&!strcmp(mode,"off"))return;
 char stem[64];snprintf(stem,sizeof(stem),"bundle-%s",t->abi);
 char *path=path_of(t->cache,stem,".so"),*receipt=path_of(t->cache,stem,".receipt");
 double begin=now();
 if(path&&receipt&&object_receipt(path,receipt,false)){
  void *handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);
  if(handle){
   const char *(*abi)(void)=NULL;RcBundleLookup lookup=NULL;
   *(void**)&abi=dlsym(handle,"rc_bundle_abi");*(void**)&lookup=dlsym(handle,"rc_bundle_lookup");
   const char *actual=abi?abi():NULL;
   if(actual&&!strcmp(actual,t->abi)&&lookup){t->bundle_handle=handle;t->bundle_lookup=lookup;}
   else dlclose(handle);
  }
 }
 t->load_s+=now()-begin;free(path);free(receipt);
}
static Tier *initialize(RcContext *ctx){
 Tier *t=state_for(ctx);if(!t)return NULL;
 const char *limit=getenv("SHEN_RC_NATIVE_CACHE_BYTES");t->cache_limit=limit?strtoull(limit,NULL,10):1024u*1024u*1024u;
 t->cc=env_or("SHEN_RC_NATIVE_CC",RC_TIER_DEFAULT_CC);t->cflags=env_or("SHEN_RC_NATIVE_CFLAGS","-O1");
 const char *timeout=getenv("SHEN_RC_COMPILE_TIMEOUT");t->timeout=timeout?strtod(timeout,NULL):120;
 if(!isfinite(t->timeout)||t->timeout<=0)t->timeout=120;
 const char *stats=getenv("SHEN_RC_NATIVE_STATS");t->stats=stats&&strcmp(stats,"0");
 const char *cache=getenv("SHEN_RC_NATIVE_CACHE");
 if(cache&&(!strcmp(cache,"off")||!strcmp(cache,"0"))){
  const char *tmp=getenv("TMPDIR");t->cache=path_of(tmp&&*tmp?tmp:"/tmp","shen-rc-compile-XXXXXX","");
  if(!t->cache||!mkdtemp(t->cache))goto failed;t->cache_owned=true;
 }else if(cache&&*cache)t->cache=strdup(cache);
 else {const char *xdg=getenv("XDG_CACHE_HOME"),*home=getenv("HOME");
  t->cache=xdg&&*xdg?path_of(xdg,"shen-c/compiled",""):home&&*home?path_of(home,".cache/shen-c/compiled",""):NULL;
 }
 if(!t->cc||!t->cflags||!t->cache||mkdirs(t->cache)||!private_directory(t->cache))goto failed;
 /* Resolve the compiler, including PATH changes and binary replacement. */
 char resolved[4096]="",candidate[4096];struct stat st={0};
 if(strchr(t->cc,'/')){if(!realpath(t->cc,resolved))snprintf(resolved,sizeof(resolved),"%s",t->cc);}
 else {char *paths=env_or("PATH","/usr/bin:/bin"),*save=NULL;
  if(paths){for(char *dir=strtok_r(paths,":",&save);dir;dir=strtok_r(NULL,":",&save)){
   snprintf(candidate,sizeof(candidate),"%s/%s",dir,t->cc);if(!access(candidate,X_OK)&&realpath(candidate,resolved))break;
  }free(paths);}
 }
 (void)stat(resolved,&st);
 char identity[16384];snprintf(identity,sizeof(identity),"compile-first-v2\n%s\n%llu/%llu/%lld/%lld\n%d\n%s\n%s\n%s",resolved,(unsigned long long)st.st_dev,(unsigned long long)st.st_ino,(long long)st.st_size,(long long)st.st_mtime,(int)SHEN_RC_IMMEDIATE_INTEGERS,t->cc,t->cflags,RC_TIER_HOST_FLAGS);
 for(size_t i=0;i<sizeof(compiler_environment)/sizeof(*compiler_environment);i++){
  const char *value=getenv(compiler_environment[i]);size_t used=strlen(identity);
  if(value)snprintf(identity+used,sizeof(identity)-used,"\n%s=%s",compiler_environment[i],value);
 }
 digest(identity,strlen(identity),RC_TIER_HEADER_DIGEST,strlen(RC_TIER_HEADER_DIGEST),t->abi);
 if(!prepare_include(t))goto failed;
 save_configuration(t);load_bundle(t);
 *rc_native_compile_slot(ctx)=t;return t;
failed:rc_error(ctx,"native compiler: cannot initialize private cache/compiler configuration");*rc_native_compile_slot(ctx)=NULL;rc_native_compile_free(t);return NULL;
}
typedef struct {char key[33];time_t age;size_t bytes;} CacheEntry;
static int oldest_first(const void *a,const void *b){const CacheEntry *x=a,*y=b;return (x->age>y->age)-(x->age<y->age);}
/* Called under the cache lock. Removing a mapped object does not unmap code. */
static void prune_cache(Tier *t){
 DIR *dir=opendir(t->cache);if(!dir)return;
 CacheEntry *items=NULL;size_t n=0,total=0;struct dirent *e;
 while((e=readdir(dir))){
  if(strlen(e->d_name)!=34||strcmp(e->d_name+32,".c"))continue;
  CacheEntry item={0};memcpy(item.key,e->d_name,32);
  const char *suffix[]={".c",".so",".receipt",".log"};
  for(size_t j=0;j<4;j++){char *p=path_of(t->cache,item.key,suffix[j]);struct stat st;if(p&&!lstat(p,&st)&&S_ISREG(st.st_mode)){item.bytes+=(size_t)st.st_size;if(!j)item.age=st.st_mtime;}free(p);}
  CacheEntry *grown=realloc(items,(n+1)*sizeof(*items));if(!grown)break;items=grown;items[n++]=item;total+=item.bytes;
 }
 closedir(dir);if(n>1)qsort(items,n,sizeof(*items),oldest_first);
 for(size_t i=0;i<n&&total>t->cache_limit;i++){
  const char *suffix[]={".c",".so",".receipt",".log"};for(size_t j=0;j<4;j++){char *p=path_of(t->cache,items[i].key,suffix[j]);if(p)unlink(p);free(p);}total-=items[i].bytes;
 }
 free(items);
}
static size_t entry_bytes(Tier *t,const char *key){
 size_t bytes=0;const char *suffix[]={".c",".so",".receipt",".log"};
 for(size_t j=0;j<4;j++){char *p=path_of(t->cache,key,suffix[j]);struct stat st;if(p&&!lstat(p,&st)&&S_ISREG(st.st_mode))bytes+=(size_t)st.st_size;free(p);}return bytes;
}
/* A serialized accounting file avoids rescanning every cache artifact on
 * every module load. Missing/invalid accounting reconstructs it by scanning. */
static void account_cache(Tier *t,size_t old_bytes,size_t new_bytes){
 char *path=path_of(t->cache,"cache",".size");if(!path)return;
 FILE *f=fopen(path,"r");unsigned long long total=0;bool valid=f&&fscanf(f,"%llu",&total)==1;if(f)fclose(f);
 if(!valid||total<old_bytes||total-old_bytes>SIZE_MAX-new_bytes||total-old_bytes+new_bytes>t->cache_limit){
  prune_cache(t);DIR *dir=opendir(t->cache);total=0;
  if(dir){struct dirent *e;while((e=readdir(dir)))if(strlen(e->d_name)==34&&!strcmp(e->d_name+32,".c")){char key[33]={0};memcpy(key,e->d_name,32);total+=entry_bytes(t,key);}closedir(dir);}
 }else total=total-old_bytes+new_bytes;
 char text[64];int n=snprintf(text,sizeof(text),"%llu\n",total);(void)write_file(path,text,(size_t)n);free(path);
}
static bool object_receipt(const char *object,const char *receipt,bool publish){
 FILE *f=fopen(object,"rb");if(!f)return false;
 char *data=NULL;size_t n=0,cap=0;char block[8192];size_t k;bool ok=true;
 while((k=fread(block,1,sizeof(block),f))){if(n+k>cap){size_t next=(n+k)*2;char *q=realloc(data,next);if(!q){ok=false;break;}data=q;cap=next;}memcpy(data+n,block,k);n+=k;}
 if(ferror(f))ok=false;fclose(f);char sum[33];digest(data,n,"",0,sum);free(data);
 return ok&&(publish?write_file(receipt,sum,32):same_file(receipt,sum,32));
}
static bool bind_entry(Module *m){*(void**)&m->entry=dlsym(m->handle,"rc_expression_module");return m->entry!=NULL;}
static RcValue *compile_execute(RcContext *ctx,RcValue *expression,RcValue *parameters){
 Tier *t=*rc_native_compile_slot(ctx);if(!t||!t->include)t=initialize(ctx);if(!t)return NULL;
 t->evaluations++;
 RcValue *root=rc_retain(expression),**bindings=NULL,*literals=NULL,*result=NULL;size_t nb=0,length=0;
 char *text=NULL,*source=NULL,*object=NULL,*receipt=NULL,*lockfile=NULL,*log=NULL;int lock=-1;size_t previous_bytes=0;
 double begin=now();FILE *out=open_memstream(&text,&length);
 RcEmitOptions opts={.expression_entry="rc_expression_module",.function_parameters=parameters,.literal_bindings=&bindings,.literal_count=&nb};
 bool emitted=out&&rc_emit_program(ctx,out,&root,1,NULL,0,&opts,NULL);if(out&&fclose(out))emitted=false;
 t->emit_s+=now()-begin;t->emitted_bytes+=length;
 if(!emitted)goto done;
 literals=rc_vector(ctx,nb);if(!literals)goto done;
 for(size_t i=0;i<nb;i++)if(!rc_vector_set(ctx,literals,i,bindings[i]))goto done;
 char key[33];digest(t->abi,strlen(t->abi),text,length,key);
 Module *m;for(m=t->modules;m;m=m->next)if(!strcmp(m->key,key)&&m->length==length&&!memcmp(m->source,text,length))break;
 if(m){t->memory_hits++;result=m->entry(ctx,literals);goto done;}
 if(t->bundle_lookup){
  const RcPackedEntry *packed=t->bundle_lookup(key,text,length);
  if(packed){
   m=calloc(1,sizeof(*m));if(!m){rc_error(ctx,"native compiler: out of memory");goto done;}
   memcpy(m->key,key,sizeof(key));m->source=(char*)packed->source;m->length=length;m->entry=packed->entry;m->handle=t->bundle_handle;
   m->next=t->modules;t->modules=m;free(text);text=NULL;t->bundle_hits++;
   result=m->entry(ctx,literals);goto done;
  }
 }
 source=path_of(t->cache,key,".c");object=path_of(t->cache,key,".so");receipt=path_of(t->cache,key,".receipt");lockfile=path_of(t->cache,"cache",".lock");log=path_of(t->cache,key,".log");
 if(!source||!object||!receipt||!lockfile||!log){rc_error(ctx,"native compiler: out of memory");goto done;}
 lock=open(lockfile,O_RDWR|O_CREAT,0600);if(lock<0||flock(lock,LOCK_EX)){rc_error(ctx,"native compiler: cannot lock cache entry");goto done;}
 previous_bytes=entry_bytes(t,key);
 m=calloc(1,sizeof(*m));if(!m){rc_error(ctx,"native compiler: out of memory");goto done;}
 begin=now();
 if(same_file(source,text,length)&&object_receipt(object,receipt,false)){
  m->handle=dlopen(object,RTLD_NOW|RTLD_LOCAL);if(m->handle&&!bind_entry(m)){dlclose(m->handle);m->handle=NULL;}
  if(m->handle)t->hits++;
 }
 if(!m->handle){
  char temp[4300];snprintf(temp,sizeof(temp),"%s.%ld.tmp",object,(long)getpid());
  if(!write_file(source,text,length)){rc_error(ctx,"native compiler: cannot write C source");free(m);goto done;}
  double compile_begin=now();int status=run_compiler(t,source,temp,log);t->compiles++;t->compile_s+=now()-compile_begin;
  if(status){unlink(temp);rc_error(ctx,"native compilation failed (status %d); diagnostics: %s",status,log);free(m);goto done;}
  if(rename(temp,object)||!object_receipt(object,receipt,true)){unlink(temp);rc_error(ctx,"native compiler: cannot publish object");free(m);goto done;}
  begin=now();m->handle=dlopen(object,RTLD_NOW|RTLD_LOCAL);
  if(!m->handle||!bind_entry(m)){rc_error(ctx,"native compiler: cannot load module: %s",dlerror());if(m->handle)dlclose(m->handle);free(m);goto done;}
  unlink(log);
 }
 t->load_s+=now()-begin;memcpy(m->key,key,sizeof(key));m->source=text;m->owns_source=true;text=NULL;m->length=length;m->next=t->modules;t->modules=m;
 account_cache(t,previous_bytes,entry_bytes(t,key));
 flock(lock,LOCK_UN);close(lock);lock=-1;
 result=m->entry(ctx,literals);
done:
 if(lock>=0){account_cache(t,previous_bytes,entry_bytes(t,key));flock(lock,LOCK_UN);close(lock);}
 if(!result)t->failures++;
 free(bindings);free(text);free(source);free(object);free(receipt);free(lockfile);free(log);rc_release(literals);rc_release(root);return result;
}

static bool symbol_named(RcValue *x,const char *name){return rc_type(x)==RC_SYMBOL&&!strcmp(rc_text(x),name);}
static RcValue *lazy_call(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *argv){
 RcValue *cell=rc_capture(self,0),*fn=rc_vector_slot_borrowed(cell,0);
 if(rc_is_nil(fn)){
  fn=compile_execute(ctx,rc_vector_slot_borrowed(cell,2),rc_vector_slot_borrowed(cell,1));if(!fn)return NULL;
  bool ok=rc_vector_set(ctx,cell,0,fn);rc_release(fn);if(!ok)return NULL;
  fn=rc_vector_slot_borrowed(cell,0);
  if(!rc_vector_set(ctx,cell,1,rc_nil())||!rc_vector_set(ctx,cell,2,rc_nil()))return NULL;
 }
 RcValue *name=rc_capture(self,1);
 /* Only replace our own binding. Historical closures remain callable after
  * redefinition and must never overwrite the replacement. */
 if(rc_type(name)==RC_SYMBOL&&rc_function_slot_borrowed(ctx,name)==self&&!rc_function_set(ctx,name,fn))return NULL;
 return rc_tailcall(ctx,fn,argc,argv);
}
static RcValue *make_lazy(RcContext *ctx,size_t arity,RcValue *parameters,RcValue *body,RcValue *name){
 RcValue *cell=rc_vector(ctx,3);if(!cell)return NULL;
 bool ok=rc_vector_set(ctx,cell,1,parameters)&&rc_vector_set(ctx,cell,2,body);
 RcValue *captures[]={cell,name};RcValue *fn=ok?rc_closure(ctx,arity,lazy_call,2,captures):NULL;
 rc_release(cell);return fn;
}
static RcValue *identity_second(RcContext *ctx,RcValue *self,size_t argc,RcValue **argv){
 (void)ctx;(void)self;(void)argc;rc_release(argv[0]);argv[0]=NULL;
 RcValue *result=argv[1];argv[1]=NULL;return result;
}
static RcValue *define_lazy(RcContext *ctx,RcValue *args){
 RcValue *values[3],*q=args;for(size_t i=0;i<3;i++){if(rc_type(q)!=RC_PAIR)return rc_error(ctx,"defun: expected name, parameters and body");values[i]=rc_head_borrowed(q);q=rc_tail_borrowed(q);}
 if(!rc_is_nil(q)||rc_type(values[0])!=RC_SYMBOL)return rc_error(ctx,"defun: invalid definition");
 size_t arity=0;
 for(q=values[1];!rc_is_nil(q);q=rc_tail_borrowed(q)){
  if(rc_type(q)!=RC_PAIR||rc_type(rc_head_borrowed(q))!=RC_SYMBOL)return rc_error(ctx,"defun: invalid parameter list");
  for(RcValue *r=values[1];r!=q;r=rc_tail_borrowed(r))if(rc_head_borrowed(r)==rc_head_borrowed(q))return rc_error(ctx,"defun: duplicate parameter");
  arity++;
 }
 Tier *t=state_for(ctx);if(!t)return NULL;t->definitions++;
 if(arity==2&&values[2]==rc_head_borrowed(rc_tail_borrowed(values[1]))){
  RcValue *fn=rc_closure_return_second_owned(ctx,identity_second,0,NULL);if(!fn)return NULL;
  bool ok=rc_function_set(ctx,values[0],fn);rc_release(fn);return ok?rc_retain(values[0]):NULL;
 }
 RcValue *fn=make_lazy(ctx,arity,values[1],values[2],values[0]);if(!fn)return NULL;
 bool ok=rc_function_set(ctx,values[0],fn);rc_release(fn);return ok?rc_retain(values[0]):NULL;
}
/* A compiled template for exact curried eta expansions. There is no syntax
 * traversal at execution: captures are a target symbol and bound argument
 * values. Lookup occurs on the final application, preserving redefinition. */
#define ETA_LIMIT 16
static RcValue *eta_call(RcContext *ctx,RcValue *self,size_t argc,RcValue **argv){
 (void)argc;RcValue *bound=rc_capture(self,1);size_t n=rc_vector_length(bound);
 size_t arity=(size_t)rc_integer_value(rc_capture(self,2));
 if(argc!=1||!arity||arity>ETA_LIMIT||n>=arity)return rc_error(ctx,"invalid native eta closure");
 if(n+1<arity){
  RcValue *next=rc_vector(ctx,n+1);if(!next)return NULL;
  bool ok=true;for(size_t i=0;i<n;i++)if(!rc_vector_set(ctx,next,i,rc_vector_slot_borrowed(bound,i)))ok=false;
  if(!rc_vector_set(ctx,next,n,argv[0]))ok=false;
  RcValue *caps[]={rc_capture(self,0),next,rc_capture(self,2)};
  RcValue *fn=ok?rc_closure_owned(ctx,1,eta_call,3,caps):NULL;rc_release(next);
  if(fn){rc_release(argv[0]);argv[0]=NULL;}return fn;
 }
 RcValue *fn=rc_function_get(ctx,rc_capture(self,0));if(!fn)return NULL;
 RcValue *args[ETA_LIMIT];for(size_t i=0;i<n;i++)args[i]=rc_retain(rc_vector_slot_borrowed(bound,i));
 args[n]=argv[0];argv[0]=NULL;
 RcValue *result=rc_tailcall_owned_args(ctx,fn,n+1,args);rc_release(fn);return result;
}
static bool list_width(RcValue *x,size_t width){
 for(size_t i=0;i<width;i++){if(rc_type(x)!=RC_PAIR)return false;x=rc_tail_borrowed(x);}return rc_is_nil(x);
}
static RcValue *eta_template(RcContext *ctx,RcValue *expression,bool *matched){
 RcValue *parameters[ETA_LIMIT],*body=expression;size_t n=0;*matched=false;
 while(rc_type(body)==RC_PAIR&&symbol_named(rc_head_borrowed(body),"lambda")){
  if(n==ETA_LIMIT||!list_width(body,3))return NULL;
  RcValue *args=rc_tail_borrowed(body),*param=rc_head_borrowed(args);
  if(rc_type(param)!=RC_SYMBOL)return NULL;
  for(size_t i=0;i<n;i++)if(parameters[i]==param)return NULL;
  parameters[n++]=param;body=rc_head_borrowed(rc_tail_borrowed(args));
 }
 if(!n||!list_width(body,n+1))return NULL;
 RcValue *target=rc_head_borrowed(body);if(rc_type(target)!=RC_SYMBOL)return NULL;
 for(size_t i=0;i<n;i++)if(parameters[i]==target)return NULL;
 const char *special[]={"if","cond","let","lambda","freeze","defun","trap-error","type","quote","c-quote","and","or"};
 for(size_t i=0;i<sizeof(special)/sizeof(*special);i++)if(symbol_named(target,special[i]))return NULL;
 RcValue *args=rc_tail_borrowed(body);for(size_t i=0;i<n;i++,args=rc_tail_borrowed(args))if(rc_head_borrowed(args)!=parameters[i])return NULL;
 *matched=true;Tier *t=state_for(ctx);if(!t)return NULL;t->templates++;t->evaluations++;
 RcValue *bound=rc_vector(ctx,0),*arity=rc_integer(ctx,(int64_t)n);
 RcValue *caps[]={target,bound,arity};RcValue *fn=bound&&arity?rc_closure_owned(ctx,1,eta_call,3,caps):NULL;
 rc_release(bound);rc_release(arity);return fn;
}
RcValue *rc_native_eval(RcContext *ctx,RcValue *expression){
 /* Native definition instruction: install a compiler thunk. Its body is
  * compiled before its first call, and is never evaluated by the interpreter.
  * Eager mode is retained only as a measurement/differential control. */
 const char *templates=getenv("SHEN_RC_NATIVE_TEMPLATES");
 if(!templates||strcmp(templates,"0")){
  bool matched=false;RcValue *fn=eta_template(ctx,expression,&matched);if(matched)return fn;
 }
 const char *eager=getenv("SHEN_RC_NATIVE_EAGER");
 if((!eager||!strcmp(eager,"0"))&&rc_type(expression)==RC_PAIR&&symbol_named(rc_head_borrowed(expression),"defun"))
  return define_lazy(ctx,rc_tail_borrowed(expression));
 if(rc_type(expression)==RC_PAIR){
  RcValue *head=rc_head_borrowed(expression),*args=rc_tail_borrowed(expression);
  bool lambda=symbol_named(head,"lambda"),freeze=symbol_named(head,"freeze");
  if((!eager||!strcmp(eager,"0"))&&((lambda&&list_width(args,2)&&rc_type(rc_head_borrowed(args))==RC_SYMBOL)||(freeze&&list_width(args,1)))){
   Tier *t=state_for(ctx);if(!t)return NULL;t->lazy_closures++;t->evaluations++;
   RcValue *parameters=lambda?rc_cons(ctx,rc_head_borrowed(args),rc_nil()):rc_nil();if(!parameters)return NULL;
   RcValue *body=lambda?rc_head_borrowed(rc_tail_borrowed(args)):rc_head_borrowed(args);
   RcValue *fn=make_lazy(ctx,lambda?1:0,parameters,body,rc_nil());rc_release(parameters);return fn;
  }
  if((symbol_named(head,"quote")||symbol_named(head,"c-quote"))&&list_width(args,1)){
   Tier *t=state_for(ctx);if(!t)return NULL;t->templates++;t->evaluations++;return rc_retain(rc_head_borrowed(args));
  }
 }else{
  Tier *t=state_for(ctx);if(!t)return NULL;t->templates++;t->evaluations++;return rc_retain(expression);
 }
 return compile_execute(ctx,expression,NULL);
}
