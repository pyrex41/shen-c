#include "rc.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

static RcValue *list(RcContext *ctx, size_t n) {
    RcValue *out=rc_nil();
    for(size_t i=0;i<n;i++) {
        RcValue *x=rc_integer(ctx,(int64_t)i), *next=rc_cons(ctx,x,out);
        assert(x&&next); rc_release(x); rc_release(out); out=next;
    }
    return out;
}
static RcValue *dag(RcContext *ctx, size_t n) {
    RcValue *out=rc_integer(ctx,42);
    for(size_t i=0;i<n;i++) {
        RcValue *next=rc_cons(ctx,out,out); assert(next); rc_release(out); out=next;
    }
    return out;
}
static RcValue *tree(RcContext *ctx,size_t depth) {
    if(!depth)return rc_integer(ctx,42);
    RcValue *a=tree(ctx,depth-1),*b=tree(ctx,depth-1),*out=rc_cons(ctx,a,b);
    assert(out);rc_release(a);rc_release(b);return out;
}
static RcValue *ring(RcContext *ctx, size_t n, int leaf) {
    RcValue **v=calloc(n,sizeof(*v)); assert(v);
    for(size_t i=0;i<n;i++) {v[i]=rc_vector(ctx,2);assert(v[i]);}
    for(size_t i=0;i<n;i++) {
        RcValue *x=rc_integer(ctx,i?0:leaf); assert(x);
        assert(rc_vector_set(ctx,v[i],0,v[(i+1)%n]));
        assert(rc_vector_set(ctx,v[i],1,x));rc_release(x);
    }
    RcValue *out=v[0];for(size_t i=1;i<n;i++)rc_release(v[i]);free(v);return out;
}
static void measure(const char *name,RcValue *a,RcValue *b,size_t count) {
    volatile size_t equal=0;clock_t start=clock();
    for(size_t i=0;i<count;i++)equal+=rc_equal(a,b);
    printf("%s %zu %.6f %zu\n",name,count,(double)(clock()-start)/CLOCKS_PER_SEC,(size_t)equal);
}
int main(int argc,char **argv) {
    (void)argv;RcContext *c=rc_context_new(),*other=rc_context_new();assert(c&&other);
    RcValue *a=rc_symbol(c,"a-distinct-long-symbol"),*b=rc_symbol(c,"b-distinct-long-symbol"),*cross=rc_symbol(other,"a-distinct-long-symbol");
    assert(rc_equal(a,a)&&!rc_equal(a,b)&&rc_equal(a,cross));
    assert(rc_equal(NULL,NULL)&&!rc_equal(NULL,a)&&!rc_equal(rc_nil(),a));
    RcValue *s=rc_string_n(c,"a\0b",3),*t=rc_string_n(other,"a\0b",3),*u=rc_string(c,"a");
    assert(rc_equal(s,t)&&!rc_equal(s,u)&&!rc_equal(s,a));
    if(argc>1) {
        RcValue *l64=list(c,64),*m64=list(c,64),*l1024=list(c,1024),*m1024=list(c,1024);
        measure("symbol-unequal",a,b,5000000);
        measure("string-equal",s,t,2000000);
        measure("list64",l64,m64,10000);
        measure("list1024",l1024,m1024,100);
        RcValue *r=ring(c,128,7),*q=ring(other,128,7),*d=dag(c,30),*e=dag(other,30);
        measure("ring128",r,q,1000);
        measure("dag30",d,e,1000);
        rc_context_free(c);rc_context_free(other);return 0;
    }
    int64_t ints[]={0,1,-1,INT64_MIN,INT64_MAX,INT64_C(4611686018427387903),INT64_C(-4611686018427387904),INT64_C(9007199254740993)};
    for(size_t i=0;i<sizeof(ints)/sizeof(*ints);i++) {
        RcValue *x=rc_integer(c,ints[i]),*y=rc_integer(other,ints[i]);assert(x&&y&&rc_equal(x,y));
        RcValue *f=rc_number(c,(double)ints[i]); assert(f);
        bool exact=(double)ints[i]>=-9223372036854775808.0&&(double)ints[i]<9223372036854775808.0&&(int64_t)(double)ints[i]==ints[i];
        assert(rc_equal(x,f)==exact&&rc_equal(f,x)==exact);
        rc_release(x);rc_release(y);rc_release(f);
    }
    RcValue *nan1=rc_number(c,NAN),*nan2=rc_number(c,NAN),*inf1=rc_number(c,INFINITY),*inf2=rc_number(other,INFINITY);
    assert(rc_equal(nan1,nan1)&&!rc_equal(nan1,nan2)&&rc_equal(inf1,inf2));
    RcValue *l=list(c,100000),*m=list(other,100000);assert(rc_equal(l,m));
    RcValue *d=dag(c,30),*e=dag(other,30);assert(rc_equal(d,e));
    RcValue *unshared=tree(c,12),*shared=dag(other,12);assert(rc_equal(unshared,shared));
    RcValue *deep=rc_nil(),*deep2=rc_nil();
    for(size_t i=0;i<100;i++) {
        RcValue *x=rc_cons(c,deep,rc_nil()),*y=rc_cons(other,deep2,rc_nil());assert(x&&y);
        rc_release(deep);rc_release(deep2);deep=x;deep2=y;
    }
    assert(rc_equal(deep,deep2));
    RcValue *r=ring(c,128,7),*q=ring(other,128,7),*bad=ring(c,128,8);
    assert(rc_equal(r,q)&&!rc_equal(r,bad));
    RcValue *v=rc_vector(c,1),*w=rc_vector(other,1),*p=rc_cons(c,a,v),*z=rc_cons(other,cross,w);
    assert(rc_vector_set(c,v,0,p)&&rc_vector_set(other,w,0,z));assert(rc_equal(p,z));
    assert(rc_vector_set(other,w,0,rc_nil()));assert(!rc_equal(p,z));
    RcValue *one=ring(c,1,3),*two=ring(c,2,3);
    /* Different graph shapes compare by their infinite unfolding. */
    RcValue *three=rc_integer(c,3);assert(three);
    assert(rc_vector_set(c,two,1,three));
    RcValue *second=rc_vector_get(c,two,0);assert(second);
    assert(rc_vector_set(c,second,1,three));rc_release(second);rc_release(three);
    assert(rc_equal(one,two));
    /* Mismatch reached only after inline queue/hash growth. */
    RcValue *late=ring(c,128,7),*walk=rc_retain(late);
    for(size_t i=0;i<100;i++){RcValue *next=rc_vector_get(c,walk,0);rc_release(walk);walk=next;}
    RcValue *wide=rc_vector(c,3);assert(wide&&rc_vector_set(c,walk,0,wide));
    rc_release(walk);rc_release(wide);assert(!rc_equal(late,q));
    assert(!rc_has_error(c)&&!rc_has_error(other));
    rc_context_free(c);rc_context_free(other);puts("RC equality: PASS");return 0;
}
