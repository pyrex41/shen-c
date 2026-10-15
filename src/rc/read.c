#include "read.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void whitespace(const char **p) {
    while (isspace((unsigned char)**p)) ++*p;
}
static bool decimal_token(const char *p) {
    if (*p=='+' || *p=='-') ++p;
    bool digits=false;
    while (isdigit((unsigned char)*p)) { digits=true; ++p; }
    if (*p=='.') { ++p; while (isdigit((unsigned char)*p)) { digits=true; ++p; } }
    if (!digits) return false;
    if (*p=='e' || *p=='E') {
        ++p; if (*p=='+' || *p=='-') ++p;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) ++p;
    }
    return !*p;
}
static RcValue *read_form(RcContext *ctx,const char **p,unsigned depth) {
    whitespace(p);
    if (!**p) return NULL;
    if (depth>2048) return rc_error(ctx,"KLambda reader: nesting exceeds 2048");
    if (**p==')') return rc_error(ctx,"KLambda reader: unexpected ')'");
    if (**p=='(') {
        ++*p;
        RcValue **items=NULL; size_t n=0,cap=0;
        RcValue *result=NULL;
        for (;;) {
            whitespace(p);
            if (**p==')') { ++*p; result=rc_nil(); break; }
            if (!**p) { rc_error(ctx,"KLambda reader: unclosed list"); break; }
            RcValue *item=read_form(ctx,p,depth+1);
            if (!item) break;
            if (n==cap) {
                if (cap>SIZE_MAX/2/sizeof(*items)) {
                    rc_release(item); rc_error(ctx,"KLambda reader: list too large"); break;
                }
                size_t next=cap ? cap*2 : 16;
                RcValue **grown=realloc(items,next*sizeof(*items));
                if (!grown) { rc_release(item); rc_error(ctx,"KLambda reader: out of memory"); break; }
                items=grown; cap=next;
            }
            items[n++]=item;
        }
        if (result) {
            while (n) {
                RcValue *next=rc_cons(ctx,items[n-1],result);
                rc_release(items[--n]); rc_release(result); result=next;
                if (!result) break;
            }
        }
        while (n) rc_release(items[--n]);
        free(items); return result;
    }
    if (**p=='"') {
        const char *start=++*p;
        while (**p && **p!='"') ++*p;
        if (!**p) return rc_error(ctx,"KLambda reader: unclosed string");
        RcValue *result=rc_string_n(ctx,start,(size_t)(*p-start));
        ++*p; return result;
    }
    const char *start=*p;
    while (**p && !isspace((unsigned char)**p) && **p!='(' && **p!=')' && **p!='"') ++*p;
    size_t length=(size_t)(*p-start);
    char *token=malloc(length+1);
    if (!token) return rc_error(ctx,"KLambda reader: out of memory");
    memcpy(token,start,length); token[length]=0;
    char *end=NULL; errno=0;
    double number=strtod(token,&end);
    RcValue *result;
    bool numeric=decimal_token(token) && end!=token && *end==0;
    if (numeric && errno==ERANGE) result=rc_error(ctx,"KLambda reader: number out of range: %s",token);
    else if (numeric) {
        char *integer_end=NULL; errno=0;
        long long integer=strtoll(token,&integer_end,10);
        if (!errno && integer_end!=token && !*integer_end) result=rc_integer(ctx,(int64_t)integer);
        else result=rc_number(ctx,number);
    } else result=rc_symbol(ctx,token);
    free(token); return result;
}
RcValue *rc_read(RcContext *ctx,const char **source) {
    if (!source || !*source) return rc_error(ctx,"KLambda reader: null source");
    return read_form(ctx,source,0);
}
