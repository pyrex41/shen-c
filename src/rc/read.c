#include "read.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
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
static int hex_digit(unsigned char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static RcValue *escaped_string(RcContext *ctx,const char **p) {
    const char *start=++*p,*end=start;
    while (*end && *end!='"') { if (*end=='\\' && end[1]) ++end; ++end; }
    if (!*end) return rc_error(ctx,"KLambda escaped reader: unclosed string");
    size_t limit=(size_t)(end-start),n=0;
    char *bytes=malloc(limit+1);
    if (!bytes) return rc_error(ctx,"KLambda escaped reader: out of memory");
    bool valid=true;
    for (const char *q=start;q<end;++q) {
        unsigned char c=(unsigned char)*q;
        if (c=='\\') {
            c=(unsigned char)*++q;
            switch(c) {
            case 'n': c='\n'; break; case 'r': c='\r'; break;
            case 't': c='\t'; break; case 'b': c='\b'; break; case 'f': c='\f'; break;
            case '\\': case '"': break;
            case 'x': {
                if (end-q<3 || hex_digit((unsigned char)q[1])<0 || hex_digit((unsigned char)q[2])<0) valid=false;
                else { c=(unsigned char)(hex_digit((unsigned char)q[1])*16+hex_digit((unsigned char)q[2])); q+=2; }
                break;
            }
            default: valid=false; break;
            }
        }
        if (!valid) break;
        bytes[n++]=(char)c;
    }
    RcValue *result=valid ? rc_string_n(ctx,bytes,n) : rc_error(ctx,"KLambda escaped reader: invalid byte escape");
    free(bytes); *p=end+1; return result;
}
static RcValue *read_form(RcContext *ctx,const char **p,unsigned depth,bool escaped) {
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
            RcValue *item=read_form(ctx,p,depth+1,escaped);
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
    if (escaped && !strncmp(*p,"#sym\"",5)) {
        *p+=4;
        RcValue *text=escaped_string(ctx,p);
        if (!text) return NULL;
        RcValue *result=memchr(rc_text(text),0,rc_text_length(text)) ?
            rc_error(ctx,"KLambda escaped reader: symbol contains zero byte") : rc_symbol(ctx,rc_text(text));
        rc_release(text); return result;
    }
    if (**p=='"') {
        if (escaped) return escaped_string(ctx,p);
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
    if (numeric && errno==ERANGE && (number==0 || !isfinite(number))) result=rc_error(ctx,"KLambda reader: number out of range: %s",token);
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
    return read_form(ctx,source,0,false);
}

RcValue *rc_read_escaped(RcContext *ctx,const char **source) {
    if (!source || !*source) return rc_error(ctx,"KLambda escaped reader: null source");
    return read_form(ctx,source,0,true);
}
