#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "rc.h"
#include "primitives.h"
#include "read.h"
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef SHEN_RC_VERSION
#define SHEN_RC_VERSION "0.3.0"
#endif

/* Generated from the bundled complete KL kernel by the independent RC emitter.
 * This installs native functions and executes the kernel initialization forms;
 * startup never reads or interprets kernel source files. */
extern bool rc_bootstrap_kernel(RcContext *ctx);

static void usage(FILE *out) {
    fputs("Usage: shen-rc [--version] [--help] <COMMAND> [<ARGS>]\n\n"
          "  repl                         Interactive Shen REPL (default).\n"
          "  script FILE [ARGS]           Load Shen source; *argv* is [FILE | ARGS].\n"
          "  compile-kl SOURCE OUTPUT [EVENTS]  Write escaped-v1 compiler AST and events.\n"
          "  eval [-q] [-e EXPR|-l FILE]...\n"
          "    -e, --eval EXPR            Evaluate Shen expressions and print results.\n"
          "    -l, --load FILE            Load Shen source through the kernel loader.\n"
          "    -q, --quiet                Suppress standard output using *hush*.\n\n"
          "The complete kernel is compiled into this executable.\n"
          "SHEN_RC_HOME optionally sets the initial Shen file directory;\n"
          "the default is the process working directory.\n",out);
}
static bool is(const char *a,const char *b) { return !strcmp(a,b); }
static bool set_owned(RcContext *ctx,const char *name,RcValue *value) {
    if (!value) return false;
    RcValue *symbol=rc_symbol(ctx,name);
    bool ok=symbol && rc_global_set(ctx,symbol,value);
    rc_release(symbol); rc_release(value); return ok;
}
static RcValue *get_global(RcContext *ctx,const char *name) {
    RcValue *symbol=rc_symbol(ctx,name);
    if (!symbol) return NULL;
    RcValue *value=rc_global_get(ctx,symbol); rc_release(symbol); return value;
}
static RcValue *call(RcContext *ctx,const char *name,size_t argc,RcValue *const *argv) {
    RcValue *symbol=rc_symbol(ctx,name);
    if (!symbol) return NULL;
    RcValue *function=rc_function_get(ctx,symbol); rc_release(symbol);
    if (!function) return NULL;
    RcValue *value=rc_apply(ctx,function,argc,argv); rc_release(function); return value;
}
static bool call_void(RcContext *ctx,const char *name) {
    RcValue *value=call(ctx,name,0,NULL);
    if (!value) return false;
    rc_release(value); return true;
}
static bool command_arguments(RcContext *ctx,int argc,char **argv) {
    RcValue *list=rc_nil();
    for (int i=argc;i>0;--i) {
        RcValue *text=rc_string(ctx,argv[i-1]);
        if (!text) { rc_release(list); return false; }
        RcValue *cell=rc_cons(ctx,text,list); rc_release(text); rc_release(list);
        if (!cell) return false;
        list=cell;
    }
    return set_owned(ctx,"*argv*",list);
}
static bool initial_directory(RcContext *ctx) {
    const char *home=getenv("SHEN_RC_HOME");
    if (!home || !*home) return true;
    size_t n=strlen(home);
    if (n>SIZE_MAX-2) { rc_error(ctx,"SHEN_RC_HOME is too long"); return false; }
    char *prefix=malloc(n+2);
    if (!prefix) { rc_error(ctx,"out of memory"); return false; }
    memcpy(prefix,home,n);
    if (home[n-1]!='/') prefix[n++]='/';
    prefix[n]=0;
    RcValue *value=rc_string(ctx,prefix); free(prefix);
    return set_owned(ctx,"*home-directory*",value);
}
static bool quiet(RcContext *ctx) {
    RcValue *value=get_global(ctx,"*hush*");
    if (!value) return false;
    bool result=rc_is_true(value); rc_release(value); return result;
}
static bool display(RcContext *ctx,RcValue *value) {
    if (quiet(ctx)) return true;
    if (rc_has_error(ctx)) return false;
    RcValue *suffix=rc_string(ctx,""), *mode=rc_symbol(ctx,"shen.a");
    if (!suffix || !mode) { rc_release(suffix); rc_release(mode); return false; }
    RcValue *argv[]={value,suffix,mode};
    RcValue *text=call(ctx,"shen.app",3,argv);
    rc_release(suffix); rc_release(mode);
    if (!text) return false;
    bool ok=rc_type(text)==RC_STRING;
    if (ok) {
        size_t n=rc_text_length(text);
        ok=fwrite(rc_text(text),1,n,stdout)==n && fputc('\n',stdout)!=EOF && !fflush(stdout);
    }
    rc_release(text);
    if (!ok) rc_error(ctx,"failed to print evaluation result");
    return ok;
}
static bool evaluate_source(RcContext *ctx,const char *source) {
    RcValue *text=rc_string(ctx,source);
    if (!text) return false;
    RcValue *forms=call(ctx,"read-from-string",1,&text); rc_release(text);
    if (!forms) return false;
    bool ok=true;
    RcValue *rest=forms;
    while (rc_type(rest)==RC_PAIR) {
        RcValue *form=rc_head_borrowed(rest);
        RcValue *value=call(ctx,"eval",1,&form);
        if (!value) { ok=false; break; }
        ok=display(ctx,value); rc_release(value);
        if (!ok) break;
        rest=rc_tail_borrowed(rest);
    }
    if (ok && !rc_is_nil(rest)) { rc_error(ctx,"kernel reader returned an improper form list"); ok=false; }
    rc_release(forms); return ok;
}
static bool load_source(RcContext *ctx,const char *filename) {
    RcValue *path=rc_string(ctx,filename);
    if (!path) return false;
    RcValue *value=call(ctx,"load",1,&path); rc_release(path);
    if (!value) return false;
    rc_release(value); return true;
}
/* escaped-v1 is an explicit compiler AST format. Literal strings and quoted
 * symbol tokens preserve every byte without introducing application syntax. */
static bool kl_string(FILE *out,const char *s,size_t n) {
    if (fputc('"',out)==EOF) return false;
    for (size_t i=0;i<n;++i) {
        unsigned char c=(unsigned char)s[i];
        if (c=='"' || c=='\\') { if (fputc('\\',out)==EOF || fputc(c,out)==EOF) return false; }
        else if (c<32 || c>=127) { if (fprintf(out,"\\x%02x",c)<0) return false; }
        else if (fputc(c,out)==EOF) return false;
    }
    return fputc('"',out)!=EOF;
}
static bool write_kl(RcContext *ctx,FILE *out,RcValue *form,unsigned depth) {
    if (depth>2048) { rc_error(ctx,"compile-kl: expression nesting exceeds 2048"); return false; }
    switch (rc_type(form)) {
    case RC_NIL: return fputs("()",out)>=0;
    case RC_STRING: return kl_string(out,rc_text(form),rc_text_length(form));
    case RC_NUMBER: {
        if (rc_number_is_exact_integer(form)) return fprintf(out,"%" PRId64,rc_integer_value(form))>=0;
        double value=rc_number_value(form);
        if (!isfinite(value)) { rc_error(ctx,"compile-kl: nonfinite numeric literal is not representable"); return false; }
        char number[64]; snprintf(number,sizeof(number),"%.17g",value);
        if (fputs(number,out)<0) return false;
        return strchr(number,'.') || strchr(number,'e') || strchr(number,'E') || fputs(".0",out)>=0;
    }
    case RC_SYMBOL: {
        const char *name=rc_text(form); bool token=*name!=0;
        for (size_t i=0;i<rc_text_length(form);++i)
            if (isspace((unsigned char)name[i]) || name[i]=='(' || name[i]==')' || name[i]=='"') token=false;
        if (token) {
            char *number_end=NULL;
            (void)strtod(name,&number_end);
            if (number_end!=name && !*number_end) token=false;
        }
        if (!token) return fputs("#sym",out)>=0 && kl_string(out,name,rc_text_length(form));
        return fwrite(name,1,rc_text_length(form),out)==rc_text_length(form);
    }
    case RC_PAIR: {
        if (fputc('(',out)==EOF) return false;
        RcValue *rest=form; bool first=true;
        while (rc_type(rest)==RC_PAIR) {
            if ((!first && fputc(' ',out)==EOF) || !write_kl(ctx,out,rc_head_borrowed(rest),depth+1)) return false;
            first=false; rest=rc_tail_borrowed(rest);
        }
        if (!rc_is_nil(rest)) { rc_error(ctx,"compile-kl: improper expression list"); return false; }
        return fputc(')',out)!=EOF;
    }
    default: rc_error(ctx,"compile-kl: unsupported literal in compiler output"); return false;
    }
}
enum ReaderHookKind { HOOK_CALL, HOOK_DEFUN, HOOK_MACRO, HOOK_DATATYPE, HOOK_SET, HOOK_PUT, HOOK_ADDRESS };
static const struct { const char *name; size_t arity; enum ReaderHookKind kind; } reader_hooks[]={
    {"shen.store-arity",2,HOOK_CALL}, {"shen.record-kl",2,HOOK_CALL},
    {"shen.record-external",2,HOOK_CALL}, {"shen.record-internal",3,HOOK_CALL},
    {"eval-kl",1,HOOK_DEFUN}, {"shen.record-macro",2,HOOK_MACRO},
    {"shen.remember-datatype",2,HOOK_DATATYPE}, {"set",2,HOOK_SET},
    {"put",4,HOOK_PUT}, {"address->",3,HOOK_ADDRESS}
};
#define READER_HOOK_COUNT (sizeof(reader_hooks)/sizeof(reader_hooks[0]))
typedef struct { RcValue *symbol[READER_HOOK_COUNT],*original[READER_HOOK_COUNT],*wrapper[READER_HOOK_COUNT],*events,*failure; size_t installed; } ReaderCapture;
static RcValue *value_list(RcContext *ctx,size_t n,RcValue *const *items) {
    RcValue *list=rc_nil();
    while (n) { RcValue *next=rc_cons(ctx,items[--n],list); rc_release(list); if (!next) return NULL; list=next; }
    return list;
}
static bool append_event(RcContext *ctx,RcValue *events,const char *tag,size_t n,RcValue *const *argv) {
    RcValue *kind=rc_symbol(ctx,tag), *args=value_list(ctx,n,argv);
    RcValue *event=kind && args ? rc_cons(ctx,kind,args) : NULL;
    rc_release(kind); rc_release(args);
    if (!event) return false;
    RcValue *previous=rc_vector_get(ctx,events,0);
    RcValue *next=previous ? rc_cons(ctx,event,previous) : NULL;
    rc_release(previous); rc_release(event);
    if (!next) return false;
    bool ok=rc_vector_set(ctx,events,0,next); rc_release(next); return ok;
}
static void unsupported_capture(RcContext *ctx,RcValue *self,const char *reason) {
    RcValue *message=rc_string(ctx,reason);
    if (!message) message=rc_retain(rc_capture(self,4));
    (void)rc_vector_set(ctx,rc_capture(self,1),2,message); rc_release(message);
}
static bool contains_definition(RcValue *value,unsigned depth) {
    if (depth>2048) return true;
    while (rc_type(value)==RC_PAIR) {
        RcValue *head=rc_head_borrowed(value);
        if (rc_type(head)==RC_SYMBOL && !strcmp(rc_text(head),"defun")) return true;
        if (contains_definition(head,depth+1)) return true;
        value=rc_tail_borrowed(value);
    }
    return false;
}
static RcValue *capture_reader_event(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *argv) {
    enum ReaderHookKind kind=(enum ReaderHookKind)rc_integer_value(rc_capture(self,2));
    RcValue *events=rc_capture(self,1), *name=rc_capture(self,3);
    RcValue *level=rc_vector_get(ctx,events,1);
    if (!level) return NULL;
    int64_t depth=rc_integer_value(level);
    bool root_definition=false;
    if (kind==HOOK_DEFUN) {
        RcValue *head=rc_head_borrowed(argv[0]);
        root_definition=rc_type(head)==RC_SYMBOL && !strcmp(rc_text(head),"defun");
    }
    bool suppress=kind==HOOK_CALL || kind==HOOK_MACRO || kind==HOOK_DATATYPE || kind==HOOK_PUT || root_definition;
    if (suppress) {
        RcValue *next=rc_integer(ctx,depth+1);
        bool ok=next && rc_vector_set(ctx,events,1,next); rc_release(next);
        if (!ok) { unsupported_capture(ctx,self,"compile-kl: cannot capture reader state"); rc_release(level); return NULL; }
    }
    RcValue *result=rc_apply(ctx,rc_capture(self,0),argc,argv);
    if (suppress) (void)rc_vector_set(ctx,events,1,level);
    rc_release(level);
    /* Failed operations can be caught by macros. Only successful effects are
     * recorded. Recreated standard handlers suppress their nested mutations. */
    if (!result || depth) return result;
    bool ok=true;
    if (kind==HOOK_CALL) {
        RcValue *args=value_list(ctx,argc,argv);
        if (!args) ok=false;
        else { RcValue *event_args[]={name,args}; ok=append_event(ctx,events,"call",2,event_args); rc_release(args); }
    } else if (kind==HOOK_DEFUN) {
        if (root_definition) {
            RcValue *body=rc_head_borrowed(rc_tail_borrowed(rc_tail_borrowed(rc_tail_borrowed(argv[0]))));
            if (contains_definition(body,0)) unsupported_capture(ctx,self,"compile-kl: nested definitions in reader-installed auxiliary functions are unsupported");
            else ok=append_event(ctx,events,"definition",1,argv);
        } else if (contains_definition(argv[0],0)) unsupported_capture(ctx,self,"compile-kl: nested persistent definitions in reader eval-kl are unsupported");
    } else if (kind==HOOK_MACRO || kind==HOOK_DATATYPE) {
        /* Standard registrations receive exactly the dynamic wrapper that fn
         * already owns. A custom closure cannot be replaced by its name. */
        RcValue *table=get_global(ctx,"shen.*lambdatable*");
        bool standard=false;
        for (RcValue *p=table; rc_type(p)==RC_PAIR; p=rc_tail_borrowed(p)) {
            RcValue *entry=rc_head_borrowed(p);
            if (rc_type(entry)==RC_PAIR && rc_equal(rc_head_borrowed(entry),argv[0]) && rc_tail_borrowed(entry)==argv[1]) { standard=true; break; }
        }
        rc_release(table);
        if (!standard) unsupported_capture(ctx,self,"compile-kl: custom macro/datatype callback persistence is unsupported");
        else ok=append_event(ctx,events,kind==HOOK_MACRO ? "macro" : "datatype",1,argv);
    } else if (kind==HOOK_SET) {
        if (rc_type(argv[0])!=RC_SYMBOL) unsupported_capture(ctx,self,"compile-kl: reader global name is not a symbol");
        else if (!strcmp(rc_text(argv[0]),"shen.*residue*")) {
            /* Parser residue and fresh names are observed after the whole
             * frontend phase, avoiding repeated snapshots of parser input. */
            ok=rc_vector_set(ctx,events,3,rc_capture(self,4));
        } else if (strcmp(rc_text(argv[0]),"shen.*gensym*")) ok=append_event(ctx,events,"global",2,argv);
    } else if (kind==HOOK_PUT || kind==HOOK_ADDRESS) {
        RcValue *properties=get_global(ctx,"*property-vector*");
        RcValue *target=kind==HOOK_PUT ? argv[3] : argv[0];
        if (!properties) ok=false;
        else if (properties==target) {
            if (kind==HOOK_PUT) ok=append_event(ctx,events,"property",3,argv);
            else unsupported_capture(ctx,self,"compile-kl: direct reader mutation of the persistent property vector is unsupported; use put");
        } else if (kind==HOOK_PUT) unsupported_capture(ctx,self,"compile-kl: custom persistent property vectors are unsupported");
        rc_release(properties);
    }
    if (!ok) unsupported_capture(ctx,self,"compile-kl: cannot capture reader state");
    return result;
}
static bool end_reader_capture(RcContext *ctx,ReaderCapture *capture) {
    bool ok=true;
    for (size_t i=0;i<capture->installed;++i) {
        RcValue *current=rc_function_lookup(ctx,capture->symbol[i]);
        if (current!=capture->wrapper[i]) { rc_error(ctx,"compile-kl: reader replaced a capture hook; persistent hook replacement is unsupported"); ok=false; }
        rc_release(current);
    }
    for (size_t i=capture->installed;i>0;--i)
        if (!rc_function_set(ctx,capture->symbol[i-1],capture->original[i-1])) ok=false;
    for (size_t i=0;i<READER_HOOK_COUNT;++i) { rc_release(capture->symbol[i]); rc_release(capture->original[i]); rc_release(capture->wrapper[i]); }
    capture->installed=0; rc_release(capture->failure);
    return ok;
}
static bool begin_reader_capture(RcContext *ctx,ReaderCapture *capture) {
    capture->events=rc_vector(ctx,4); capture->failure=rc_string(ctx,"compile-kl: reader state capture failed");
    RcValue *zero=rc_integer(ctx,0);
    bool initialized=capture->events && capture->failure && zero && rc_vector_set(ctx,capture->events,1,zero);
    rc_release(zero);
    if (!initialized) { end_reader_capture(ctx,capture); rc_release(capture->events); return false; }
    for (size_t i=0;i<READER_HOOK_COUNT;++i) {
        capture->symbol[i]=rc_symbol(ctx,reader_hooks[i].name);
        if (capture->symbol[i]) capture->original[i]=rc_function_get(ctx,capture->symbol[i]);
        RcValue *kind=capture->original[i] ? rc_integer(ctx,reader_hooks[i].kind) : NULL;
        RcValue *captures[]={capture->original[i],capture->events,kind,capture->symbol[i],capture->failure};
        capture->wrapper[i]=kind ? rc_closure(ctx,reader_hooks[i].arity,capture_reader_event,5,captures) : NULL;
        rc_release(kind);
        bool ok=capture->wrapper[i] && rc_function_set(ctx,capture->symbol[i],capture->wrapper[i]);
        if (!ok) { end_reader_capture(ctx,capture); rc_release(capture->events); return false; }
        ++capture->installed;
    }
    return true;
}
static bool snapshot_frontend_global(RcContext *ctx,RcValue *events,const char *text) {
    RcValue *name=rc_symbol(ctx,text), *value=name ? rc_global_get(ctx,name) : NULL;
    if (!value) { rc_release(name); return false; }
    RcValue *args[]={name,value}; bool ok=append_event(ctx,events,"global",2,args);
    rc_release(name); rc_release(value); return ok;
}
static bool write_reader_events(RcContext *ctx,FILE *out,RcValue *state) {
    RcValue *events=rc_vector_get(ctx,state,0);
    if (!events) return false;
    size_t n=0;
    for (RcValue *p=events; rc_type(p)==RC_PAIR; p=rc_tail_borrowed(p)) ++n;
    if (n>SIZE_MAX/sizeof(RcValue *)) { rc_release(events); rc_error(ctx,"compile-kl: too many reader events"); return false; }
    RcValue **ordered=n ? malloc(n*sizeof(*ordered)) : NULL;
    if (n && !ordered) { rc_release(events); rc_error(ctx,"compile-kl: out of memory"); return false; }
    size_t i=0;
    for (RcValue *p=events; rc_type(p)==RC_PAIR; p=rc_tail_borrowed(p)) ordered[i++]=rc_head_borrowed(p);
    bool ok=true;
    while (i && ok) ok=write_kl(ctx,out,ordered[--i],0) && fputc('\n',out)!=EOF;
    free(ordered); rc_release(events); return ok;
}
static char *output_identity(RcContext *ctx,const char *path) {
    char *resolved=realpath(path,NULL);
    if (resolved) return resolved;
    char *copy=strdup(path);
    if (!copy) { rc_error(ctx,"compile-kl: out of memory"); return NULL; }
    char *slash=strrchr(copy,'/'),*base=slash ? slash+1 : copy;
    const char *parent=".";
    if (slash) { *slash=0; parent=*copy ? copy : "/"; }
    char *directory=realpath(parent,NULL);
    if (!directory) { free(copy); rc_error(ctx,"compile-kl: cannot resolve output directory: %s",strerror(errno)); return NULL; }
    size_t n=strlen(directory),m=strlen(base);
    char *identity=n<=SIZE_MAX-m-2 ? malloc(n+m+2) : NULL;
    if (identity) snprintf(identity,n+m+2,"%s%s%s",directory,n==1 && directory[0]=='/' ? "" : "/",base);
    else rc_error(ctx,"compile-kl: out of memory");
    free(directory); free(copy); return identity;
}
static bool publish_output(RcContext *ctx,FILE *pending,const char *path) {
    if (fflush(pending) || fseek(pending,0,SEEK_SET)) return false;
    size_t n=strlen(path);
    if (n>SIZE_MAX-12) { rc_error(ctx,"compile-kl: output path is too long"); return false; }
    char *temporary=malloc(n+12);
    if (!temporary) { rc_error(ctx,"compile-kl: out of memory"); return false; }
    snprintf(temporary,n+12,"%s.tmp.XXXXXX",path);
    int fd=mkstemp(temporary);
    FILE *destination=fd<0 ? NULL : fdopen(fd,"wb");
    if (!destination) {
        if (fd>=0) { close(fd); unlink(temporary); }
        free(temporary); rc_error(ctx,"compile-kl: cannot create output: %s",strerror(errno)); return false;
    }
    bool ok=true; char bytes[16384]; size_t length;
    while ((length=fread(bytes,1,sizeof(bytes),pending)))
        if (fwrite(bytes,1,length,destination)!=length) { ok=false; break; }
    if (ferror(pending)) ok=false;
    if (fclose(destination)) ok=false;
    if (ok && rename(temporary,path)) ok=false;
    if (!ok) unlink(temporary);
    free(temporary); return ok;
}
static bool compile_source(RcContext *ctx,const char *source,const char *output,const char *events_output) {
    if (events_output) {
        char *program_path=output_identity(ctx,output), *event_path=output_identity(ctx,events_output);
        struct stat a,b;
        bool aliases=!stat(output,&a) && !stat(events_output,&b) && a.st_dev==b.st_dev && a.st_ino==b.st_ino;
        if (program_path && event_path && !strcmp(program_path,event_path)) aliases=true;
        bool valid=program_path && event_path && !aliases;
        free(program_path); free(event_path);
        if (!valid) { if (aliases) rc_error(ctx,"compile-kl: program and event outputs must differ"); return false; }
    }
    RcValue *path=rc_string(ctx,source);
    if (!path) return false;
    /* Absolute command paths must remain absolute even when Shen cd is set. */
    RcValue *home=NULL;
    if (*source=='/') {
        home=get_global(ctx,"*home-directory*");
        if (!home || !set_owned(ctx,"*home-directory*",rc_string(ctx,""))) { rc_release(home); rc_release(path); return false; }
    }
    ReaderCapture capture={0};
    if (!begin_reader_capture(ctx,&capture)) {
        if (home) set_owned(ctx,"*home-directory*",home);
        rc_release(path); return false;
    }
    RcValue *forms=call(ctx,"read-file",1,&path); rc_release(path);
    if (!end_reader_capture(ctx,&capture)) { rc_release(forms); forms=NULL; }
    if (home && !set_owned(ctx,"*home-directory*",home)) { rc_release(forms); forms=NULL; }
    if (!forms) { rc_release(capture.events); return false; }
    FILE *pending=tmpfile(), *metadata=events_output ? tmpfile() : NULL;
    bool ok=pending && (!events_output || metadata);
    if (!ok) rc_error(ctx,"compile-kl: cannot create temporary output: %s",strerror(errno));
    RcValue *unsupported=rc_vector_get(ctx,capture.events,2);
    if (unsupported && !rc_is_nil(unsupported)) { rc_error(ctx,"%s",rc_text(unsupported)); ok=false; }
    if (!unsupported) ok=false;
    rc_release(unsupported);
    RcValue *rest=forms;
    while (ok && rc_type(rest)==RC_PAIR) {
        RcValue *form=rc_head_borrowed(rest);
        /* The -h variant translates definitions without recording/evaluating
         * them; application top levels stay executable in the generated app. */
        RcValue *kl=call(ctx,"shen.shen->kl-h",1,&form);
        if (!kl) { ok=false; break; }
        ok=write_kl(ctx,pending,kl,0) && fputc('\n',pending)!=EOF;
        rc_release(kl);
        rest=rc_tail_borrowed(rest);
    }
    if (ok && !rc_is_nil(rest)) { rc_error(ctx,"compile-kl: kernel reader returned an improper form list"); ok=false; }
    rc_release(forms);
    if (ok && metadata) ok=snapshot_frontend_global(ctx,capture.events,"shen.*gensym*");
    RcValue *residue=rc_vector_get(ctx,capture.events,3);
    if (ok && metadata && residue && !rc_is_nil(residue)) ok=snapshot_frontend_global(ctx,capture.events,"shen.*residue*");
    rc_release(residue);
    if (ok && metadata) ok=write_reader_events(ctx,metadata,capture.events);
    rc_release(capture.events);
    if (ok) ok=publish_output(ctx,pending,output);
    if (ok && metadata) ok=publish_output(ctx,metadata,events_output);
    if (pending && fclose(pending)) ok=false;
    if (metadata && fclose(metadata)) ok=false;
    if (!ok && !rc_has_error(ctx)) rc_error(ctx,"compile-kl: failed to write compiler output");
    return ok;
}
static int eval_arguments(RcContext *ctx,int argc,char **argv,int start) {
    for (int i=start;i<argc;++i) {
        if (is(argv[i],"-q") || is(argv[i],"--quiet")) {
            if (!set_owned(ctx,"*hush*",rc_true(ctx))) return 1;
        } else {
            bool eval=is(argv[i],"-e") || is(argv[i],"--eval");
            if (!(eval ? evaluate_source(ctx,argv[i+1]) : load_source(ctx,argv[i+1]))) return 1;
            ++i;
        }
    }
    return 0;
}
static int repl(RcContext *ctx) {
    if (!call_void(ctx,"shen.credits")) return 1;
    for (;;) {
        if (!call_void(ctx,"shen.initialise_environment") || !call_void(ctx,"shen.prompt")) return 1;
        if (fflush(stdout)) { rc_error(ctx,"failed to flush standard output"); return 1; }
        /* The kernel reader handles complete multiline forms. Peek only at the
         * iteration boundary so a closed pipe exits without its empty-stream
         * error being retried by the kernel's otherwise infinite REPL loop. */
        int byte=fgetc(stdin);
        if (byte==EOF) {
            if (ferror(stdin)) { rc_error(ctx,"failed to read standard input: %s",strerror(errno)); return 1; }
            if (isatty(fileno(stdin))) fputc('\n',stdout);
            return 0;
        }
        if (ungetc(byte,stdin)==EOF) { rc_error(ctx,"failed to read standard input"); return 1; }
        if (!call_void(ctx,"shen.read-evaluate-print")) {
            fprintf(stderr,"shen-rc: %s\n",rc_error_message(ctx));
            rc_clear_error(ctx);
        }
    }
}
int main(int argc,char **argv) {
    if (argc==2 && (is(argv[1],"--version") || is(argv[1],"-v"))) {
        printf("Shen-C RC %s (native compiled kernel)\n",SHEN_RC_VERSION); return 0;
    }
    if (argc>=2 && (is(argv[1],"--help") || is(argv[1],"-h"))) { usage(stdout); return 0; }
    enum { REPL, SCRIPT, EVAL, COMPILE } command=REPL;
    int start=2;
    if (argc>1) {
        if (is(argv[1],"repl")) {
            if (argc!=2) { usage(stderr); return 2; }
        } else if (is(argv[1],"script")) {
            command=SCRIPT;
            if (argc<3) { fputs("shen-rc: script requires a file\n",stderr); return 2; }
        } else if (is(argv[1],"compile-kl")) {
            command=COMPILE;
            if (argc!=4 && argc!=5) { fputs("shen-rc: compile-kl requires SOURCE OUTPUT [EVENTS]\n",stderr); return 2; }
        } else if (is(argv[1],"eval")) command=EVAL;
        else if (is(argv[1],"-e") || is(argv[1],"--eval")) { command=EVAL; start=1; }
        else { fprintf(stderr,"shen-rc: invalid command: %s\n",argv[1]); usage(stderr); return 2; }
    }
    if (command==EVAL) {
        bool work=false;
        for (int i=start;i<argc;++i) {
            if (is(argv[i],"-q") || is(argv[i],"--quiet")) continue;
            if (!is(argv[i],"-e") && !is(argv[i],"--eval") && !is(argv[i],"-l") && !is(argv[i],"--load")) {
                fprintf(stderr,"shen-rc: invalid eval argument: %s\n",argv[i]); return 2;
            }
            if (++i>=argc) { fputs("shen-rc: eval option requires an argument\n",stderr); return 2; }
            work=true;
        }
        if (!work) { fputs("shen-rc: eval requires -e EXPR or -l FILE\n",stderr); return 2; }
    }
    RcContext *ctx=rc_context_new();
    if (!ctx) { fputs("shen-rc: cannot create runtime context\n",stderr); return 1; }
    int status=1;
    if (rc_bootstrap_kernel(ctx) && initial_directory(ctx) &&
        command_arguments(ctx,command==SCRIPT ? argc-2 : argc,command==SCRIPT ? argv+2 : argv)) {
        if (command==REPL) status=repl(ctx);
        else if (command==SCRIPT) status=load_source(ctx,argv[2]) ? 0 : 1;
        else if (command==COMPILE) status=compile_source(ctx,argv[2],argv[3],argc==5 ? argv[4] : NULL) ? 0 : 1;
        else status=eval_arguments(ctx,argc,argv,start);
    }
    if (status) fprintf(stderr,"shen-rc: %s\n",rc_has_error(ctx) ? rc_error_message(ctx) : "operation failed");
    rc_context_free(ctx); return status;
}
