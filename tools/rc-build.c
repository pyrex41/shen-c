#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "emit_rc.h"

/* The compiler frontend is allowed to link libshenc/Boehm. Generated app.c
 * and its Makefile depend exclusively on the new src/rc runtime. */
static void path(char *out,size_t cap,const char *dir,const char *file) {
  if(snprintf(out,cap,"%s/%s",dir,file)>=(int)cap) { fprintf(stderr,"rc-build: path too long\n"); exit(1); }
}
static int mkdirs(const char *s) {
  char b[4096]; if(strlen(s)>=sizeof(b)) return -1; strcpy(b,s);
  for(char *p=b+1;*p;p++) if(*p=='/') { *p=0; if(mkdir(b,0755)&&errno!=EEXIST) return -1; *p='/'; }
  return mkdir(b,0755)&&errno!=EEXIST?-1:0;
}
/* A Make variable value preserves spaces; escaping them here would make a
 * backslash part of the compiler's quoted filename. Dollar/hash need Make
 * escaping. Prerequisite escaping and shell quoting happen separately below. */
static void mkvalue(FILE *out,const char *s) {
  for(;*s;s++) { if(*s=='$') fputc('$',out); if(*s=='#') fputc('\\',out); fputc(*s,out); }
}
static void addforms(KLObject ***dst,long *size,KLObject **forms,long n) {
  *dst=realloc(*dst,(size_t)(*size+n)*sizeof(**dst));
  if(n&&!*dst) { fprintf(stderr,"rc-build: allocation failed\n"); exit(1); }
  memcpy(*dst+*size,forms,(size_t)n*sizeof(**dst)); *size+=n;
}
int main(int argc,char **argv) {
  if(argc!=3) { fprintf(stderr,"usage: rc-build <kernel-and-user-dir> <out-dir>\n"); return 2; }
  char home[4096],src[4096],dest[4096],kernel[1024]="kernel.kl",init[1024]="",users[256][1024]; int nu=0;
  const char *env=getenv("SHEN_C_HOME"); if(!env||!*env) env=".";
  if(!realpath(env,home)) { perror(env); return 1; }
  path(src,sizeof(src),argv[1],"yggdrasil.manifest.txt"); FILE *mf=fopen(src,"r");
  if(!mf) { path(src,sizeof(src),argv[1],"rc.manifest.txt"); mf=fopen(src,"r"); }
  if(mf) {
    char line[4096];
    while(fgets(line,sizeof(line),mf)) {
      line[strcspn(line,"\r\n")]=0; char *eq=strchr(line,'='); if(!eq||line[0]=='#') continue; *eq++=0;
      if(!strcmp(line,"kernel")) snprintf(kernel,sizeof(kernel),"%s",eq);
      else if(!strcmp(line,"init")) snprintf(init,sizeof(init),"%s",eq);
      else if(!strcmp(line,"user")) { if(nu>=256) { fprintf(stderr,"rc-build: too many user entries\n"); return 1; } snprintf(users[nu++],1024,"%s",eq); }
      else if(!strcmp(line,"overlay")) { fprintf(stderr,"rc-build: legacy overlays cannot be linked into RC artifacts; provide compiled KL via user= entries\n"); return 1; }
    }
    fclose(mf);
  } else { strcpy(users[nu++],"user.kl"); }
  shen_boot(&shen_root_context,home);
  KLObject **kf=NULL,**uf=NULL; long nk=0,nt=0;
  path(src,sizeof(src),argv[1],kernel);
  if(shen_read_kl_path(src,&kf,&nk)) { fprintf(stderr,"rc-build: cannot read %s\n",src); return 1; }
  for(int i=0;i<nu;i++) {
    KLObject **forms=NULL; long n=0; path(src,sizeof(src),argv[1],users[i]);
    if(shen_read_kl_path(src,&forms,&n)) { fprintf(stderr,"rc-build: cannot read %s\n",src); return 1; }
    addforms(&uf,&nt,forms,n);
  }
  if(mkdirs(argv[2])) { perror(argv[2]); return 1; }
  path(dest,sizeof(dest),argv[2],"app.c"); FILE *out=fopen(dest,"w"); if(!out) { perror(dest); return 1; }
  ShenEmitReport report={0}; int status=shen_emit_rc_program(out,kf,nk,uf,nt,init,argv[1],&report); fclose(out);
  if(status) { fprintf(stderr,"rc-build: emission failed\n"); return 1; }
  path(dest,sizeof(dest),argv[2],"Makefile"); out=fopen(dest,"w"); if(!out) { perror(dest); return 1; }
  fputs("# Generated app uses only the independent RC runtime.\nSHEN_C_HOME ?= ",out); mkvalue(out,home);
  fputs("\n"
        "CC ?= cc\n"
        "CFLAGS ?= -O2\n"
        "rc_empty :=\n"
        "rc_space := $(rc_empty) $(rc_empty)\n"
        "rc_escape = $(subst $(rc_space),\\$(rc_space),$(1))\n"
        "rc_quote = '$(subst ','\"'\"',$(1))'\n"
        "RC_FILES = rc.c primitives.c eval.c read.c\n"
        "RC_HEADERS = rc.h primitives.h eval.h read.h\n"
        "RC_DEPS = $(foreach file,$(RC_FILES) $(RC_HEADERS),$(call rc_escape,$(SHEN_C_HOME)/src/rc/$(file)))\n"
        "app: app.c $(RC_DEPS)\n"
        "\t$(CC) $(CFLAGS) -std=c17 -I$(call rc_quote,$(SHEN_C_HOME)/src/rc) app.c $(foreach file,$(RC_FILES),$(call rc_quote,$(SHEN_C_HOME)/src/rc/$(file))) -lm -o app\n",out);
  fclose(out);
  pid_t pid=fork(); if(pid<0) { perror("fork"); return 1; }
  if(!pid) { if(chdir(argv[2])) { perror(argv[2]); _exit(1); } execlp("make","make","app",(char*)NULL); perror("make"); _exit(1); }
  int result; if(waitpid(pid,&result,0)<0) { perror("waitpid"); return 1; }
  if(!WIFEXITED(result)||WEXITSTATUS(result)) return 1;
  fprintf(stderr,"rc-build: native defuns=%ld lambdas=%ld tops=%ld\n",report.defuns,report.lambdas,report.toplevels);
  return 0;
}
