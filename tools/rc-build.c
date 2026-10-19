#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "emit.h"
#include "release.h"

/* Standalone compiler host: RcValue reader and native emitter only. */
static void path(char *out,size_t cap,const char *dir,const char *file) {
  if(snprintf(out,cap,"%s/%s",dir,file)>=(int)cap) { fprintf(stderr,"rc-build: path too long\n"); exit(1); }
}
static int mkdirs(const char *s) {
  char b[4096]; if(strlen(s)>=sizeof(b)) return -1; strcpy(b,s);
  for(char *p=b+1;*p;p++) if(*p=='/') { *p=0; if(mkdir(b,0755)&&errno!=EEXIST) return -1; *p='/'; }
  return mkdir(b,0755)&&errno!=EEXIST?-1:0;
}
static void mkvalue(FILE *out,const char *s) {
  for(;*s;s++) { if(*s=='$') fputc('$',out); if(*s=='#') fputc('\\',out); fputc(*s,out); }
}
static void release_forms(RcValue **forms,size_t n) {
  for(size_t i=0;i<n;i++) rc_release(forms[i]); free(forms);
}
static bool append_path(RcContext *ctx,RcValue ***dst,size_t *size,const char *filename,bool escaped) {
  RcValue **forms=NULL; size_t n=0;
  if(!rc_read_path_ex(ctx,filename,escaped,&forms,&n)) return false;
  if(n>SIZE_MAX/sizeof(**dst)-*size) { release_forms(forms,n); rc_error(ctx,"too many input forms"); return false; }
  if(n) {
    RcValue **grown=realloc(*dst,(*size+n)*sizeof(**dst));
    if(!grown) { release_forms(forms,n); rc_error(ctx,"compiler out of memory"); return false; }
    *dst=grown; memcpy(*dst+*size,forms,n*sizeof(**dst)); *size+=n;
  }
  free(forms); return true;
}
/* A separate compiler metadata file cannot collide with ordinary user calls. */
static bool read_source_events(RcContext *ctx,const char *filename,bool escaped,RcValue **events) {
  RcValue **entries=NULL,*list=rc_nil(); size_t count=0;
  if(!rc_read_path_ex(ctx,filename,escaped,&entries,&count)) return false;
  for(size_t i=count;i>0;i--) {
    RcValue *next=rc_cons(ctx,entries[i-1],list);
    if(!next) { rc_release(list); release_forms(entries,count); return false; }
    rc_release(list); list=next;
  }
  release_forms(entries,count); *events=list; return true;
}
static bool emit_path(RcContext *ctx,const char *filename,RcValue **kernel,size_t nkernel,RcValue **user,size_t nuser,const RcEmitOptions *options) {
  char directory[4096];
  if(strlen(filename)>=sizeof(directory)) { rc_error(ctx,"output path too long"); return false; }
  strcpy(directory,filename); char *slash=strrchr(directory,'/');
  if(slash) { *slash=0; if(*directory&&mkdirs(directory)) { rc_error(ctx,"cannot create output directory %s",directory); return false; } }
  char pending[4200];snprintf(pending,sizeof(pending),"%s.%ld.new",filename,(long)getpid());
  FILE *out=fopen(pending,"w+");
  if(!out) { rc_error(ctx,"cannot write %s",filename); return false; }
  RcEmitReport report={0}; bool ok=rc_emit_program(ctx,out,kernel,nkernel,user,nuser,options,&report);
  if(ok&&fflush(out))ok=false;
  FILE *old=ok?fopen(filename,"rb"):NULL;bool same=old!=NULL;
  if(old){rewind(out);char a[8192],b[8192];size_t n,m;
    do{n=fread(a,1,sizeof(a),out);m=fread(b,1,sizeof(b),old);if(n!=m||memcmp(a,b,n)){same=false;break;}}while(n);
    if(ferror(old)||ferror(out))same=false;fclose(old);
  }
  if(fclose(out)&&ok) { rc_error(ctx,"cannot finish %s",filename); ok=false; }
  if(ok&&!same&&rename(pending,filename)){rc_error(ctx,"cannot publish %s",filename);ok=false;}
  if(same||!ok)unlink(pending);
  if(!ok)return false;
  fprintf(stderr,"rc-build: native defuns=%ld lambdas=%ld tops=%ld\n",report.defuns,report.lambdas,report.toplevels);
  return true;
}
static bool kernel_only(RcContext *ctx,const char *directory,const char *output,RcEmitOptions *options) {
  static const char *modules[]=SHEN_KERNEL_MODULES;
  RcValue **forms=NULL; size_t n=0; bool ok=false; char filename[4096],name[64];
  for(size_t i=0;i<sizeof(modules)/sizeof(*modules);i++) {
    snprintf(name,sizeof(name),"%s.kl",modules[i]); path(filename,sizeof(filename),directory,name);
    if(!append_path(ctx,&forms,&n,filename,false)) goto done;
  }
  options->emit_main=false;
  ok=emit_path(ctx,output,forms,n,NULL,0,options);
done:
  release_forms(forms,n); return ok;
}
static bool build_app(RcContext *ctx,const char *input,const char *output,RcEmitOptions *options,bool acyclic,const char *events_override) {
  char home[4096],src[4096],dest[4096],kernel[1024]="kernel.kl",init[1024]="",events_file[1024]="",source_format[1024]="",users[256][1024]; int nu=0;
  const char *env=getenv("SHEN_C_HOME"); if(!env||!*env) env=".";
  if(!realpath(env,home)) { rc_error(ctx,"cannot resolve compiler home %s",env); return false; }
  path(src,sizeof(src),input,"yggdrasil.manifest.txt"); FILE *mf=fopen(src,"r");
  if(!mf) { path(src,sizeof(src),input,"rc.manifest.txt"); mf=fopen(src,"r"); }
  if(mf) {
    char line[4096];
    while(fgets(line,sizeof(line),mf)) {
      line[strcspn(line,"\r\n")]=0; char *eq=strchr(line,'='); if(!eq||line[0]=='#') continue; *eq++=0;
      if(!strcmp(line,"kernel")) snprintf(kernel,sizeof(kernel),"%s",eq);
      else if(!strcmp(line,"init")) snprintf(init,sizeof(init),"%s",eq);
      else if(!strcmp(line,"source-events")) snprintf(events_file,sizeof(events_file),"%s",eq);
      else if(!strcmp(line,"source-format")) snprintf(source_format,sizeof(source_format),"%s",eq);
      else if(!strcmp(line,"source-arities")) { fclose(mf); rc_error(ctx,"obsolete source-arities metadata; use source-events"); return false; }
      else if(!strcmp(line,"user")) {
        if(nu>=256) { fclose(mf); rc_error(ctx,"too many user entries"); return false; }
        snprintf(users[nu++],1024,"%s",eq);
      } else if(!strcmp(line,"overlay")) { fclose(mf); rc_error(ctx,"legacy overlays require KL user= entries for RC emission"); return false; }
    }
    bool failed=ferror(mf); fclose(mf);
    if(failed) { rc_error(ctx,"failed reading manifest"); return false; }
  } else strcpy(users[nu++],"user.kl");
  RcValue **kf=NULL,**uf=NULL,*source_events=NULL; size_t nk=0,nt=0; bool ok=false;
  bool escaped=!strcmp(source_format,"escaped-v1");
  if(*source_format&&!escaped) { rc_error(ctx,"unknown source format %s",source_format); goto done; }
  path(src,sizeof(src),input,kernel);
  if(!append_path(ctx,&kf,&nk,src,false)) goto done;
  for(int i=0;i<nu;i++) { path(src,sizeof(src),input,users[i]); if(!append_path(ctx,&uf,&nt,src,escaped)) goto done; }
  if((*events_file||events_override)&&!options->source_metadata) { rc_error(ctx,"source-events requires --source-metadata"); goto done; }
  if(options->source_metadata) {
    if(events_override) {
      if(!read_source_events(ctx,events_override,escaped,&source_events)) goto done;
    } else if(*events_file) {
      path(src,sizeof(src),input,events_file);
      if(!read_source_events(ctx,src,escaped,&source_events)) goto done;
    }
    options->source_events=source_events;
  }
  options->emit_main=true; options->init_name=init;
  path(dest,sizeof(dest),output,"app.c");
  if(!emit_path(ctx,dest,kf,nk,uf,nt,options)) goto done;
  path(dest,sizeof(dest),output,"Makefile"); FILE *out=fopen(dest,"w");
  if(!out) { rc_error(ctx,"cannot write %s",dest); goto done; }
  fputs("# Generated app uses only the independent RC runtime.\nSHEN_C_HOME ?= ",out); mkvalue(out,home);
  fputs("\n"
        "CC ?= cc\n"
        "CFLAGS ?= -O3\n"
        "rc_empty :=\n"
        "rc_space := $(rc_empty) $(rc_empty)\n"
        "rc_escape = $(subst $(rc_space),\\$(rc_space),$(1))\n"
        "rc_quote = '$(subst ','\"'\"',$(1))'\n"
        "RC_FILES = rc.c primitives.c eval.c read.c emit.c native_compile.c\n"
        "RC_HEADERS = rc.h rc_internal.h primitives.h eval.h eval_plan.h read.h emit.h native_compile.h\n"
        "RC_DEPS = $(foreach file,$(RC_FILES) $(RC_HEADERS),$(call rc_escape,$(SHEN_C_HOME)/src/rc/$(file)))\n"
        "RC_EXPORT = $(if $(filter Darwin,$(shell uname -s)),-Wl$(rc_comma)-export_dynamic,-rdynamic -ldl)\n"
        "rc_comma := ,\n"
        "app: app.c $(RC_DEPS)\n"
        "\tpython3 $(call rc_quote,$(SHEN_C_HOME)/scripts/embed-tier-config.py) tier_config.h $(call rc_quote,$(CC)) $(call rc_quote,$(CPPFLAGS) $(CFLAGS)) $(foreach file,rc.h primitives.h eval.h,$(call rc_quote,$(SHEN_C_HOME)/src/rc/$(file))) -- $(foreach file,$(RC_FILES) emit.h,$(call rc_quote,$(SHEN_C_HOME)/src/rc/$(file)))\n"
        "\t$(CC) $(CPPFLAGS) $(CFLAGS) -std=c17 -I. -I$(call rc_quote,$(SHEN_C_HOME)/src/rc) app.c $(foreach file,$(RC_FILES),$(call rc_quote,$(SHEN_C_HOME)/src/rc/$(file))) -lm $(RC_EXPORT) -o app\n",out);
  if(acyclic) fputs("override CPPFLAGS += -DSHEN_RC_ACYCLIC_ONLY\n",out);
  if(fclose(out)) { rc_error(ctx,"cannot finish generated Makefile"); goto done; }
  pid_t pid=fork(); if(pid<0) { rc_error(ctx,"cannot fork make"); goto done; }
  if(!pid) { if(chdir(output)) { perror(output); _exit(1); } execlp("make","make","app",(char*)NULL); perror("make"); _exit(1); }
  int result; if(waitpid(pid,&result,0)<0) { rc_error(ctx,"cannot wait for make"); goto done; }
  if(!WIFEXITED(result)||WEXITSTATUS(result)) { rc_error(ctx,"generated application build failed"); goto done; }
  ok=true;
done:
  release_forms(kf,nk); release_forms(uf,nt); rc_release(source_events); return ok;
}
static void usage(FILE *out) {
  fputs("usage: rc-build [--conservative-ownership] [--acyclic] [--source-metadata] <input-dir> <output-dir>\n"
        "       rc-build [--conservative-ownership] [--acyclic] --emit-kernel <kl-dir> <output.c>\n"
        "  --source-events FILE: ordered reader metadata sidecar (application builds only).\n"
        "  --source-metadata: preserve Shen definition arity and source reflection metadata.\n"
        "  --acyclic: build applications without cycle collection; reject new reference cycles.\n",out);
}
int main(int argc,char **argv) {
  RcEmitOptions options={.emit_main=true,.bootstrap_name="rc_bootstrap_kernel"};
  int index=1; bool only_kernel=false,acyclic=false; const char *events_override=NULL;
#if defined(SHEN_RC_ACYCLIC_ONLY) && SHEN_RC_ACYCLIC_ONLY
  acyclic=true; /* A strict host always emits strict application builds. */
#endif
  for(;index<argc&&argv[index][0]=='-';index++) {
    if(!strcmp(argv[index],"--emit-kernel")) only_kernel=true;
    else if(!strcmp(argv[index],"--acyclic")) acyclic=true;
    else if(!strcmp(argv[index],"--source-metadata")) options.source_metadata=true;
    else if(!strcmp(argv[index],"--source-events")) {
      if(index+1>=argc) { usage(stderr); return 2; }
      events_override=argv[++index]; options.source_metadata=true;
    }
    else if(!strcmp(argv[index],"--conservative-ownership")) options.conservative_ownership=true;
    else if(!strcmp(argv[index],"--help")||!strcmp(argv[index],"-h")) { usage(stdout); return 0; }
    else if(!strcmp(argv[index],"--version")) { puts("shen-rc-build standalone 1"); return 0; }
    else { fprintf(stderr,"rc-build: unknown option %s\n",argv[index]); return 2; }
  }
  if(argc-index!=2||(only_kernel&&events_override)) { usage(stderr); return 2; }
  RcContext *ctx=rc_context_new(); if(!ctx) return 1;
  bool ok=only_kernel?kernel_only(ctx,argv[index],argv[index+1],&options):build_app(ctx,argv[index],argv[index+1],&options,acyclic,events_override);
  if(!ok) fprintf(stderr,"rc-build: %s\n",rc_error_message(ctx));
  rc_context_free(ctx); return ok?0:1;
}
