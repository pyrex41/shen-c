#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "native_tier.h"
#include "emit.h"
#include "rc_internal.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#include "tier_config.h" /* embedded headers exceed the C99 minimum */
#pragma GCC diagnostic pop
#include <dlfcn.h>
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

/* Registry entries own their interpreted closure, name and defun syntax, so a
 * registered closure address can never be reused while its slot is live. */
typedef enum { COLD, PENDING, FAILED, PROMOTED, STALE } EntryState;
typedef struct { RcValue *fn, *name, *definition; uint64_t calls; EntryState state; } Entry;
#define TOMBSTONE SIZE_MAX
#define HISTOGRAM 24
#define BATCH_LIMIT 512
typedef struct {
    RcContext *ctx;
    Entry *entries; size_t nentries, entry_cap, live;
    size_t *slots, slot_cap, slot_used; /* entry index + 1; 0 empty */
    size_t *pending, npending, pending_cap;
    size_t *batch, nbatch; /* entries offered to the running module */
    uint64_t threshold;
    bool count_only, disabled, busy, stats, cache_owned;
    char *cc, *cflags, *cache, *include;
    char abi[33];
    void **handles; size_t nhandles, handle_cap;
    /* Diagnostics. histogram[k] counts retired/remaining defuns whose call
     * count c has bit length k; calls[k] sums their calls. */
    size_t defuns, redefined, promoted, failed, batches, compiles, cache_hits,
           fallbacks, emit_failures, guard_misses, load_failures;
    char load_error[256];
    uint64_t histogram[HISTOGRAM], calls[HISTOGRAM];
    double emit_s, compile_s, load_s;
} Tier;
static Tier *current; /* Module define callback target; single-threaded. */

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9;
}
static size_t hash_ptr(const void *p,size_t cap) {
    uint64_t x=(uint64_t)(uintptr_t)p; x^=x>>33; x*=UINT64_C(0xff51afd7ed558ccd); x^=x>>33;
    return (size_t)x&(cap-1);
}
static size_t *find_slot(Tier *t,const RcValue *fn) {
    if(!t->slot_cap)return NULL;
    for(size_t i=hash_ptr(fn,t->slot_cap);;i=(i+1)&(t->slot_cap-1)) {
        size_t s=t->slots[i];
        if(!s)return NULL;
        if(s!=TOMBSTONE && t->entries[s-1].fn==fn)return &t->slots[i];
    }
}
static bool grow_slots(Tier *t) {
    size_t cap=t->slot_cap?t->slot_cap*2:1024;
    while(cap<4*(t->live+1))cap*=2;
    size_t *slots=calloc(cap,sizeof(*slots)); if(!slots)return false;
    for(size_t i=0;i<t->slot_cap;i++) {
        size_t s=t->slots[i]; if(!s||s==TOMBSTONE)continue;
        size_t j=hash_ptr(t->entries[s-1].fn,cap);
        while(slots[j])j=(j+1)&(cap-1);
        slots[j]=s;
    }
    free(t->slots); t->slots=slots; t->slot_cap=cap; t->slot_used=t->live; return true;
}
static void record(Tier *t,uint64_t calls) {
    int k=0; while(calls>>k && k<HISTOGRAM-1)k++;
    t->histogram[k]++; t->calls[k]+=calls;
}
/* Retire one entry, releasing its owners. Its pending index, if any, is
 * skipped later because the entry no longer has an owned closure. */
static void retire(Tier *t,size_t *slot) {
    Entry *e=&t->entries[*slot-1];
    record(t,e->calls);
    rc_release(e->fn); rc_release(e->name); rc_release(e->definition);
    *e=(Entry){0}; *slot=TOMBSTONE; t->live--;
}
static void push_pending(Tier *t,size_t index) {
    if(t->npending==t->pending_cap) {
        size_t cap=t->pending_cap?t->pending_cap*2:256;
        size_t *grown=realloc(t->pending,cap*sizeof(*grown)); if(!grown)return;
        t->pending=grown; t->pending_cap=cap;
    }
    t->entries[index].state=PENDING; t->pending[t->npending++]=index;
}

static void on_defun(RcContext *ctx,void *state,RcValue *name,RcValue *definition,
                     RcValue *fn,const RcValue *old) {
    Tier *t=state; (void)ctx;
    if(old) { size_t *slot=find_slot(t,old); if(slot) { retire(t,slot); t->redefined++; } }
    t->defuns++;
    if(t->disabled && !t->count_only)return;
    if(4*(t->slot_used+1)>3*t->slot_cap && !grow_slots(t))return;
    if(t->nentries==t->entry_cap) {
        /* Compact retired entries before growing; slots are rebuilt. */
        size_t keep=0;
        for(size_t i=0;i<t->nentries;i++) if(t->entries[i].fn) t->entries[keep++]=t->entries[i];
        if(keep!=t->nentries) {
            t->nentries=keep; t->npending=0;
            for(size_t i=0;i<keep;i++) if(t->entries[i].state==PENDING) push_pending(t,i);
            memset(t->slots,0,t->slot_cap*sizeof(*t->slots)); t->slot_used=0;
            for(size_t i=0;i<keep;i++) {
                size_t j=hash_ptr(t->entries[i].fn,t->slot_cap);
                while(t->slots[j])j=(j+1)&(t->slot_cap-1);
                t->slots[j]=i+1; t->slot_used++;
            }
        }
        if(t->nentries==t->entry_cap) {
            size_t cap=t->entry_cap?t->entry_cap*2:1024;
            Entry *grown=realloc(t->entries,cap*sizeof(*grown)); if(!grown)return;
            t->entries=grown; t->entry_cap=cap;
        }
    }
    size_t index=t->nentries++;
    t->entries[index]=(Entry){rc_retain(fn),rc_retain(name),rc_retain(definition),0,COLD};
    size_t j=hash_ptr(fn,t->slot_cap);
    while(t->slots[j] && t->slots[j]!=TOMBSTONE)j=(j+1)&(t->slot_cap-1);
    if(!t->slots[j])t->slot_used++;
    t->slots[j]=index+1; t->live++;
    if(!t->count_only && !t->threshold)push_pending(t,index);
}
static void on_call(RcContext *ctx,void *state,RcValue *self) {
    Tier *t=state; (void)ctx;
    size_t *slot=find_slot(t,self); if(!slot)return;
    Entry *e=&t->entries[*slot-1];
    if(++e->calls==t->threshold && e->state==COLD && !t->count_only && !t->disabled)
        push_pending(t,*slot-1);
}

/* Content identity: two independent 64-bit FNV-1a lanes. A cache hit also
 * compares the stored C text byte for byte, so a collision cannot load code
 * for different input. */
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
    char tmp[4200]; snprintf(tmp,sizeof(tmp),"%s.%ld.tmp",path,(long)getpid());
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0600); if(fd<0)return false;
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
/* Returns 0 on success, 127 when the compiler could not be executed. */
static int run_compiler(Tier *t,const char *source,const char *object,const char *log) {
    char *flags=join(t->cflags," " RC_TIER_HOST_FLAGS); if(!flags)return 1;
    char *argv[96]; size_t n=0; char incflag[4200];
    argv[n++]=t->cc;
    n=words(flags,argv,n,80);
    snprintf(incflag,sizeof(incflag),"-I%s",t->include);
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
    pid_t pid; int status=0;
    int spawn=posix_spawnp(&pid,t->cc,&actions,NULL,argv,environ);
    posix_spawn_file_actions_destroy(&actions); free(flags);
    if(spawn)return 127;
    while(waitpid(pid,&status,0)<0) if(errno!=EINTR)return 1;
    if(!WIFEXITED(status))return 1;
    return WEXITSTATUS(status);
}
static bool define(RcContext *ctx,RcValue *name,RcValue *function) {
    Tier *t=current;
    for(size_t i=0;i<t->nbatch;i++) {
        Entry *e=&t->entries[t->batch[i]];
        if(e->name!=name || e->state!=PENDING)continue;
        /* Identity guard: a redefinition since the snapshot always wins. */
        if(rc_function_slot_borrowed(ctx,name)!=e->fn || rc_arity(function)!=rc_arity(e->fn)) {
            e->state=STALE; t->guard_misses++; return true;
        }
        if(!rc_function_set(ctx,name,function))return false;
        e->state=PROMOTED; t->promoted++;
        return true;
    }
    return true;
}
/* Emits forms[0..n) into one translation unit. NULL on emitter failure. */
static char *emit(Tier *t,RcValue **forms,size_t n,size_t *length) {
    char *text=NULL; FILE *out=open_memstream(&text,length);
    if(!out)return NULL;
    RcEmitOptions options={.module_entry="rc_tier_module"};
    bool ok=rc_emit_program(t->ctx,out,forms,n,NULL,0,&options,NULL);
    if(fclose(out))ok=false;
    if(!ok) { free(text); rc_clear_error(t->ctx); return NULL; }
    return text;
}
static void *open_object(Tier *t,const char *path) {
    void *handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!handle) {
        const char *e=dlerror(); t->load_failures++;
        snprintf(t->load_error,sizeof(t->load_error),"%s",e?e:"unknown");
    }
    return handle;
}
static void fail_batch(Tier *t,size_t *batch,size_t n) {
    for(size_t i=0;i<n;i++) { t->entries[batch[i]].state=FAILED; t->failed++; }
    t->fallbacks++;
}
/* Injective, type-tagged KL serialization used as the cache key and stored
 * beside each object; a hit requires byte equality, so a digest collision
 * cannot load code compiled from different input. Returns false for values
 * that have no stable spelling (closures, vectors, streams in syntax). */
static bool serialize(FILE *out,RcValue *x) {
    for(;;) {
        switch(rc_type(x)) {
        case RC_NIL: fputc('N',out); return true;
        case RC_NUMBER:
            if(rc_number_is_int64(x)) fprintf(out,"I%lld;",(long long)rc_integer_value(x));
            else fprintf(out,"R%a;",rc_number_value(x));
            return true;
        case RC_SYMBOL: case RC_STRING:
            fprintf(out,"%c%zu:",rc_type(x)==RC_SYMBOL?'S':'T',rc_text_length(x));
            fwrite(rc_text(x),1,rc_text_length(x),out); return true;
        case RC_PAIR:
            fputc('(',out);
            if(!serialize(out,rc_head_borrowed(x)))return false;
            x=rc_tail_borrowed(x); continue;
        default: return false;
        }
    }
}
static char *serialize_forms(Tier *t,size_t *batch,size_t n,size_t *length) {
    char *text=NULL; FILE *out=open_memstream(&text,length);
    if(!out)return NULL;
    fputs(t->abi,out); fputc('\n',out);
    bool ok=true;
    for(size_t i=0;ok && i<n;i++) { ok=serialize(out,t->entries[batch[i]].definition); fputc('\n',out); }
    if(fclose(out))ok=false;
    if(!ok) { free(text); return NULL; }
    return text;
}
/* Keeps only forms that serialize and emit; others are marked FAILED. */
static size_t filter_batch(Tier *t,size_t *batch,size_t n) {
    size_t m=0;
    for(size_t i=0;i<n;i++) {
        size_t length; char *kl=serialize_forms(t,&batch[i],1,&length),*c=NULL;
        RcValue *form=t->entries[batch[i]].definition;
        if(kl) c=emit(t,&form,1,&length);
        if(c) batch[m++]=batch[i];
        else { t->entries[batch[i]].state=FAILED; t->failed++; t->emit_failures++; }
        free(kl); free(c);
    }
    return m;
}
static void promote(Tier *t,size_t *batch,size_t n) {
    RcValue *forms[BATCH_LIMIT];
    size_t kl_length=0; char *kl=serialize_forms(t,batch,n,&kl_length),*text=NULL;
    char key[33],*keep=NULL,*object=NULL,*log=NULL;
    void *handle=NULL;
    if(!kl) { n=filter_batch(t,batch,n); if(n)kl=serialize_forms(t,batch,n,&kl_length); }
    if(!kl)goto done;
    digest(kl,kl_length,"",0,key);
    keep=path_of(t->cache,key,".kl"); object=path_of(t->cache,key,".so"); log=path_of(t->cache,key,".log");
    if(!keep||!object||!log)goto done;
    if(!t->cache_owned && access(object,R_OK)==0 && same_file(keep,kl,kl_length)) {
        handle=open_object(t,object);
        if(handle)t->cache_hits++;
    }
    if(!handle) {
        double t0=now();
        for(size_t i=0;i<n;i++) forms[i]=t->entries[batch[i]].definition;
        size_t length=0; text=emit(t,forms,n,&length);
        if(!text) {
            /* Exclude each unemittable defun permanently; retry the rest under
             * its own key so later runs hit the cache for the reduced batch. */
            t->emit_s+=now()-t0;
            size_t m=filter_batch(t,batch,n);
            free(kl); free(keep); free(object); free(log);
            if(m) promote(t,batch,m);
            return;
        }
        double t1=now(); t->emit_s+=t1-t0;
        char *source=path_of(t->cache,key,".build.c");
        char unique[4300]; snprintf(unique,sizeof(unique),"%s.%ld.tmp",object,(long)getpid());
        bool built=false;
        if(source && write_file(source,text,length)) {
            int status=run_compiler(t,source,unique,log);
            t->compiles++;
            if(status==127) t->disabled=true; /* No compiler: interpret from now on. */
            /* Publish the object before its key text: readers trust a pair
             * only when the key text matches, and identical key text always
             * names identical code. Failures keep the C and log for diagnosis. */
            built=!status && !rename(unique,object) && write_file(keep,kl,kl_length);
            if(!status) { unlink(source); unlink(log); } else unlink(unique);
        }
        free(source);
        t->compile_s+=now()-t1;
        if(built)handle=open_object(t,object);
        if(t->cache_owned) { unlink(object); unlink(keep); }
    }
    if(!handle)goto done;
    double t2=now();
    bool (*entry)(RcContext*,RcModuleDefine)=NULL;
    *(void**)&entry=dlsym(handle,"rc_tier_module");
    if(t->nhandles==t->handle_cap) {
        size_t cap=t->handle_cap?t->handle_cap*2:16;
        void **grown=realloc(t->handles,cap*sizeof(*grown));
        if(grown) { t->handles=grown; t->handle_cap=cap; }
    }
    if(t->nhandles<t->handle_cap)t->handles[t->nhandles++]=handle; /* never closed */
    if(entry) {
        current=t; t->batch=batch; t->nbatch=n;
        bool ok=entry(t->ctx,define);
        current=NULL; t->batch=NULL; t->nbatch=0;
        if(!ok)rc_clear_error(t->ctx);
    }
    t->load_s+=now()-t2;
done:
    if(!handle && n)fail_batch(t,batch,n);
    free(kl); free(text); free(keep); free(object); free(log);
}
static void on_safepoint(RcContext *ctx,void *state) {
    Tier *t=state;
    if(!t->npending || t->busy)return;
    if(t->disabled) { t->npending=0; return; }
    t->busy=true;
    /* Promotion runs no interpreted code, so no hook can append meanwhile. */
    size_t *todo=t->pending,ntodo=t->npending;
    t->pending=NULL; t->npending=t->pending_cap=0;
    size_t batch[BATCH_LIMIT];
    for(size_t next=0;next<ntodo && !t->disabled;) {
        size_t n=0;
        for(;next<ntodo && n<BATCH_LIMIT;next++) {
            Entry *e=&t->entries[todo[next]];
            if(!e->fn || e->state!=PENDING)continue;
            if(rc_function_slot_borrowed(ctx,e->name)!=e->fn) { e->state=STALE; t->guard_misses++; }
            else batch[n++]=todo[next];
        }
        if(n) { t->batches++; promote(t,batch,n); }
        for(size_t i=0;i<n;i++) {
            Entry *e=&t->entries[batch[i]];
            /* Never offered (module lacked it): keep interpreting. */
            if(e->state==PENDING) { e->state=FAILED; t->failed++; }
        }
    }
    /* Retire promoted and superseded entries; failures stay registered so
     * they are never retried. */
    for(size_t i=0;i<ntodo;i++) {
        Entry *e=&t->entries[todo[i]];
        if(e->fn && (e->state==PROMOTED || e->state==STALE)) { size_t *slot=find_slot(t,e->fn); if(slot)retire(t,slot); }
        else if(e->fn && e->state==PENDING) e->state=FAILED;
    }
    free(todo);
    t->busy=false;
    if(rc_has_error(ctx))rc_clear_error(ctx);
}
static const RcTierHooks hooks={on_defun,on_call,on_safepoint};

/* Loading from the cache executes its objects: require a real directory
 * owned by this user and writable by nobody else. */
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
void rc_native_tier_attach(RcContext *ctx) {
    const char *mode=getenv("SHEN_RC_NATIVE");
    if(!mode || !*mode || !strcmp(mode,"0"))return;
    Tier *t=calloc(1,sizeof(*t)); if(!t)return;
    t->ctx=ctx; t->count_only=!strcmp(mode,"count");
    const char *threshold=getenv("SHEN_RC_NATIVE_THRESHOLD");
    t->threshold=threshold && *threshold ? strtoull(threshold,NULL,10) : RC_TIER_DEFAULT_THRESHOLD;
    const char *stats=getenv("SHEN_RC_NATIVE_STATS"); t->stats=stats && *stats && strcmp(stats,"0");
    t->cc=env_or("SHEN_RC_NATIVE_CC",RC_TIER_DEFAULT_CC);
    t->cflags=env_or("SHEN_RC_NATIVE_CFLAGS","-O1");
    /* ABI identity: embedded runtime headers, host build flags, compiler and
     * flags. Any change selects a different cache key. */
    {
        size_t n=strlen(t->cc)+strlen(t->cflags)+sizeof(RC_TIER_HOST_FLAGS)+64; char *abi=malloc(n);
        if(abi) {
            snprintf(abi,n,"%s\n%s\n%s\n%d\n",t->cc,t->cflags,RC_TIER_HOST_FLAGS,(int)SHEN_RC_IMMEDIATE_INTEGERS);
            /* RC_TIER_HEADER_DIGEST also covers the emitter sources. */
            char headers[33]; digest(abi,strlen(abi),RC_TIER_HEADER_DIGEST,strlen(RC_TIER_HEADER_DIGEST),headers);
            memcpy(t->abi,headers,sizeof(t->abi)); free(abi);
        } else t->disabled=true;
    }
    const char *cache=getenv("SHEN_RC_NATIVE_CACHE");
    if(cache && (!strcmp(cache,"off") || !strcmp(cache,"0"))) {
        char tmpl[]="/tmp/shen-rc-native-XXXXXX"; const char *tmp=getenv("TMPDIR");
        char *base=tmp && *tmp ? path_of(tmp,"shen-rc-native-XXXXXX","") : strdup(tmpl);
        if(base && mkdtemp(base)) { t->cache=base; t->cache_owned=true; } else { free(base); t->disabled=true; }
    } else if(cache && *cache) t->cache=strdup(cache);
    else {
        const char *xdg=getenv("XDG_CACHE_HOME"),*home=getenv("HOME");
        if(xdg && *xdg) t->cache=path_of(xdg,"shen-c/native","");
        else if(home && *home) t->cache=path_of(home,".cache/shen-c/native","");
        else t->disabled=true;
    }
    if(!t->disabled && !t->count_only && (!t->cache || mkdirs(t->cache) || !private_directory(t->cache) || !prepare_include(t)))
        t->disabled=true;
    rc_tier_attach(ctx,&hooks,t);
}
void rc_native_tier_detach(RcContext *ctx) {
    void *state; if(!rc_tier_hooks(ctx,&state))return;
    Tier *t=state; rc_tier_attach(ctx,NULL,NULL);
    for(size_t i=0;i<t->slot_cap;i++) if(t->slots[i] && t->slots[i]!=TOMBSTONE) retire(t,&t->slots[i]);
    if(t->stats) {
        fprintf(stderr,"shen-rc native-tier: mode=%s threshold=%llu defuns=%zu redefined=%zu promoted=%zu failed=%zu"
                " emit_failures=%zu guard_misses=%zu batches=%zu compiles=%zu cache_hits=%zu fallbacks=%zu"
                " disabled=%d emit_s=%.3f compile_s=%.3f load_s=%.3f\n",
                t->count_only?"count":"promote",(unsigned long long)t->threshold,t->defuns,t->redefined,t->promoted,
                t->failed,t->emit_failures,t->guard_misses,t->batches,t->compiles,t->cache_hits,t->fallbacks,
                (int)t->disabled,t->emit_s,t->compile_s,t->load_s);
        if(t->load_failures)fprintf(stderr,"shen-rc native-tier: load_failures=%zu last: %s\n",t->load_failures,t->load_error);
        fputs("shen-rc native-tier: calls-histogram",stderr);
        for(int k=0;k<HISTOGRAM;k++) if(t->histogram[k])
            fprintf(stderr," [%llu,%llu):%llu/%llu",(unsigned long long)(k?UINT64_C(1)<<(k-1):0),
                    (unsigned long long)(UINT64_C(1)<<k),(unsigned long long)t->histogram[k],(unsigned long long)t->calls[k]);
        fputc('\n',stderr);
    }
    if(t->cache_owned) {
        if(t->include) for(size_t i=0;i<sizeof(rc_tier_headers)/sizeof(*rc_tier_headers);i++) {
            char *file=path_of(t->include,rc_tier_headers[i].name,""); if(file)unlink(file); free(file);
        }
        if(t->include)rmdir(t->include);
        rmdir(t->cache);
    }
    free(t->entries); free(t->slots); free(t->pending); free(t->handles);
    free(t->cc); free(t->cflags); free(t->cache); free(t->include); free(t);
}
