#define _POSIX_C_SOURCE 200809L
#include "rc_phase_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

typedef struct {
    const char *phase;
    char load[256];
    double cpu, child_cpu;
    int focus;
} Frame;
static Frame stack[1024];
static unsigned depth;
static FILE *output;
static int initialized;
static void initialize(void);
static double cpu(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) abort();
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void quoted(const char *s) {
    fputc('"',output);
    for (;*s;s++) {
        unsigned char c=(unsigned char)*s;
        if(c=='"'||c=='\\') fputc('\\',output);
        if(c<32) fprintf(output,"\\u%04x",c); else fputc(c,output);
    }
    fputc('"',output);
}
static void event(const char *kind,const char *load) {
    fprintf(output,"{\"event\":\"%s\",\"load\":",kind);
    quoted(load); fputs("}\n",output); fflush(output);
}
static void leave(int aborted) {
    if(!depth) abort();
    Frame *f=&stack[--depth];
    double elapsed=cpu()-f->cpu;
    if(depth) stack[depth-1].child_cpu+=elapsed;
    fprintf(output,"{\"event\":\"span\",\"phase\":"); quoted(f->phase);
    fputs(",\"load\":",output); quoted(f->load);
    fprintf(output,",\"depth\":%u,\"cpu\":%.9f,\"exclusive_cpu\":%.9f,\"aborted\":%s}\n",
            depth,elapsed,elapsed-f->child_cpu,aborted?"true":"false");
    if(f->focus) { event("focus-end",f->load); raise(SIGSTOP); }
}
static void finish(void) {
    if(!initialized) return;
    unsigned open=depth;
    while(depth) leave(depth!=1);
    fprintf(output,"{\"event\":\"finished\",\"open_scopes\":%u}\n",open);
    fclose(output);
}
void sp_enter(const char *phase,const char *load) {
    initialize();
    if(depth==1024) abort();
    Frame *f=&stack[depth]; memset(f,0,sizeof(*f)); f->phase=phase;
    snprintf(f->load,sizeof(f->load),"%s",load?load:stack[depth-1].load);
    const char *focus=getenv("SHEN_PROFILE_FOCUS");
    f->focus=load && focus && !strcmp(load,focus);
    if(f->focus) { event("focus-start",f->load); raise(SIGSTOP); }
    f->cpu=cpu(); depth++;
}
void sp_leave(void) { leave(0); }
unsigned sp_mark(void) { initialize(); return depth; }
void sp_rewind(unsigned mark) { if(mark>depth) abort(); while(depth>mark) leave(1); }
static void initialize(void) {
    if(initialized) return;
    const char *path=getenv("SHEN_PROFILE_FILE");
    if(!path || !(output=fopen(path,"w"))) abort();
    initialized=1; atexit(finish);
    stack[depth++]=(Frame){.phase="other",.cpu=0};
}
