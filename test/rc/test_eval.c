#include "eval.h"
#include "primitives.h"
#include "read.h"
#ifdef NDEBUG
#undef NDEBUG /* Test checks and their setup must run in Release builds. */
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static RcValue *run(RcContext *ctx, const char *source) {
    RcValue *form=rc_read(ctx,&source);
    assert(form);
    assert(!rc_read(ctx,&source) && !rc_has_error(ctx));
    RcValue *value=rc_eval(ctx,form);
    rc_release(form);
    if (!value) fprintf(stderr,"evaluation failed: %s\n",rc_error_message(ctx));
    assert(value && !rc_has_error(ctx));
    return value;
}
static void number(RcContext *ctx,const char *source,double expected) {
    RcValue *value=run(ctx,source);
    assert(rc_type(value)==RC_NUMBER && rc_number_value(value)==expected);
    rc_release(value);
}
static void fails(RcContext *ctx,const char *source,const char *message) {
    RcValue *form=rc_read(ctx,&source); assert(form);
    RcValue *value=rc_eval(ctx,form); rc_release(form);
    assert(!value && rc_has_error(ctx));
    assert(strstr(rc_error_message(ctx),message));
    rc_clear_error(ctx);
}
/* Model a legal callback removing the external owner of a borrowed public
 * input. rc_eval's boundary root must protect every remaining AST descendant. */
static RcValue *external_input;
static RcValue *drop_external_input(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)self;(void)args;assert(argc==0&&external_input);
    rc_release(external_input);external_input=NULL;rc_collect(ctx);return rc_nil();
}
static RcValue *new_self(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)self;(void)args;assert(argc==1);return rc_integer(ctx,99);
}
static RcValue *replace_eval_self(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *args) {
    (void)self;(void)args;assert(argc==0);
    RcValue *symbol=rc_symbol(ctx,"old-eval-self"),*fn=rc_closure(ctx,1,new_self,0,NULL);
    assert(symbol&&fn&&rc_function_set(ctx,symbol,fn));rc_release(symbol);rc_release(fn);
    rc_collect(ctx);return rc_nil();
}
static void install_callback(RcContext *ctx,const char *name,RcNative native) {
    RcValue *symbol=rc_symbol(ctx,name),*fn=rc_closure(ctx,0,native,0,NULL);
    assert(symbol&&fn&&rc_function_set(ctx,symbol,fn));rc_release(symbol);rc_release(fn);
}
static void borrowed_roots(RcContext *ctx) {
    install_callback(ctx,"drop-eval-input",drop_external_input);
    install_callback(ctx,"replace-eval-self",replace_eval_self);
    const char *forms[]={
        "(do (drop-eval-input) (let X 40 (+ X 2)))",
        "(trap-error (do (drop-eval-input) (simple-error \"caught\")) (lambda E (let X 40 (+ X 2))))"
    };
    for(size_t i=0;i<sizeof(forms)/sizeof(*forms);i++) {
        const char *source=forms[i];external_input=rc_read(ctx,&source);assert(external_input);
        RcValue *out=rc_eval(ctx,external_input);assert(!external_input&&out&&rc_integer_value(out)==42);rc_release(out);
    }
    RcValue *value=run(ctx,"(defun old-eval-self (X) (do (replace-eval-self) (let Y (+ X 2) Y)))");rc_release(value);
    number(ctx,"(old-eval-self 40)",42);number(ctx,"(old-eval-self 0)",99);
    value=run(ctx,"(let X 40 (let Y 2 (freeze (+ X Y))))");assert(rc_type(value)==RC_CLOSURE);
    rc_collect(ctx);RcValue *out=rc_apply(ctx,value,0,NULL);assert(out&&rc_integer_value(out)==42);
    rc_release(out);rc_release(value);
}
int main(void) {
    RcContext *ctx=rc_context_new(); assert(ctx);
    assert(rc_install_primitives(ctx) && rc_install_eval(ctx));
    borrowed_roots(ctx);
    number(ctx,"(let X 7 ((lambda Y (+ X Y)) 3))",10);
    number(ctx,"(let X 7 (let F (freeze X) (let X 9 (F))))",7);
    number(ctx,"(let F (lambda X (lambda Y (+ X Y))) ((F 3) 4))",7);
    RcValue *value=run(ctx,"(defun add3 (X Y Z) (+ (+ X Y) Z))"); rc_release(value);
    number(ctx,"(let F (add3 1) (F 2 3))",6);
    value=run(ctx,"(defun F (X) (+ X 1))"); rc_release(value);
    number(ctx,"(let F (lambda X X) (F 3))",3);
    number(ctx,"(let if (lambda X X) (if 3))",3);
    number(ctx,"(cond (false 1) (true 2))",2);
    value=run(ctx,"(defun do (X Y) X)"); rc_release(value);
    number(ctx,"(do 10 20)",10);
    value=run(ctx,"(defun do (X Y) Y)"); rc_release(value);
    number(ctx,"(do 10 20)",20);
    value=run(ctx,"(defun chosen (X) 1)"); rc_release(value);
    number(ctx,"(chosen (let Ignored (defun chosen (X) 2) 0))",1);
    number(ctx,"(chosen 0)",2);
    value=run(ctx,"(defun indirect (Y) 1)"); rc_release(value);
    number(ctx,"(let Alias indirect (Alias (do (defun indirect (Y) 2) 0)))",1);
    number(ctx,"(indirect 0)",2);
    value=run(ctx,"(defun indirect (Y) 3)"); rc_release(value);
    number(ctx,"((if true indirect missing.indirect) (do (defun indirect (Y) 4) 0))",3);
    number(ctx,"(indirect 0)",4);
    number(ctx,"(type 8 (arbitrary unbound type expression))",8);
    number(ctx,"(trap-error (simple-error \"caught\") (lambda E (if (= (error-to-string E) \"caught\") 9 0)))",9);
    number(ctx,"(eval-kl (cons + (cons 2 (cons 3 ()))))",5);
    value=run(ctx,"(and false (simple-error \"not evaluated\"))"); assert(!rc_is_true(value)); rc_release(value);
    value=run(ctx,"(or true (simple-error \"not evaluated\"))"); assert(rc_is_true(value)); rc_release(value);
    fails(ctx,"(if 1 2 3)","boolean");
    fails(ctx,"(and true 1)","boolean");
    fails(ctx,"(cond (false 1))","no true");
    fails(ctx,"(defun bad (X X) X)","duplicate");
    fails(ctx,"(let X 1)","expects");
    fails(ctx,"(unknown.rc.function 1)","function");
    value=run(ctx,"(defun down (N A) (if (= N 0) A (let K (- N 1) (down K (+ A 1)))))"); rc_release(value);
    number(ctx,"(down 100000 0)",100000);
    value=run(ctx,"(defun even.rc (N) (if (= N 0) true (odd.rc (- N 1))))"); rc_release(value);
    value=run(ctx,"(defun odd.rc (N) (if (= N 0) false (even.rc (- N 1))))"); rc_release(value);
    value=run(ctx,"(even.rc 100001)"); assert(!rc_is_true(value)); rc_release(value);
    value=run(ctx,"(defun and.rc (N) (if (= N 0) true (and true (and.rc (- N 1)))))"); rc_release(value);
    value=run(ctx,"(and.rc 100000)"); assert(rc_is_true(value)); rc_release(value);
    value=run(ctx,"(defun or.rc (N) (if (= N 0) false (or false (or.rc (- N 1)))))"); rc_release(value);
    value=run(ctx,"(or.rc 100000)"); assert(!rc_is_true(value)); rc_release(value);
    value=run(ctx,"(defun bad.bool.rc (N) (if (= N 0) 7 (and true (bad.bool.rc (- N 1)))))"); rc_release(value);
    fails(ctx,"(bad.bool.rc 100000)","boolean");
    number(ctx,"(trap-error (bad.bool.rc 100000) (lambda E 91))",91);
    number(ctx,"(trap-error (or false (bad.bool.rc 100000)) (lambda E 92))",92);
    number(ctx,"(trap-error (and true (trap-error (simple-error \"inner\") (lambda E 7))) (lambda E 93))",93);
    number(ctx,"(trap-error (trap-error (and true 7) (lambda E (simple-error \"rethrow\"))) (lambda E (if (= (error-to-string E) \"rethrow\") 94 0)))",94);
    fails(ctx,"((lambda X (and true (lambda Y Y))) 0 1)","boolean");
    fails(ctx,"((lambda X (or false (lambda Y Y))) 0 1)","boolean");
    const char *input="\"a\\n\nb\" nan inf 0x10 + . ()";
    value=rc_read(ctx,&input); assert(value && rc_type(value)==RC_STRING && !strcmp(rc_text(value),"a\\n\nb")); rc_release(value);
    const char *symbols[]={"nan","inf","0x10","+","."};
    for (size_t i=0;i<5;++i) { value=rc_read(ctx,&input); assert(value && rc_type(value)==RC_SYMBOL && !strcmp(rc_text(value),symbols[i])); rc_release(value); }
    value=rc_read(ctx,&input); assert(value && rc_is_nil(value)); rc_release(value);
    assert(!rc_read(ctx,&input) && !rc_has_error(ctx));
    input="\"a\\\"\\x00\\\\\\xff\" #sym\"123\" #sym\"a b(\\\"\"";
    value=rc_read_escaped(ctx,&input);
    const char encoded_bytes[]={'a','"',0,'\\',(char)255};
    assert(value && rc_type(value)==RC_STRING && rc_text_length(value)==sizeof(encoded_bytes) && !memcmp(rc_text(value),encoded_bytes,sizeof(encoded_bytes)));
    rc_release(value);
    value=rc_read_escaped(ctx,&input); assert(value && rc_type(value)==RC_SYMBOL && !strcmp(rc_text(value),"123")); rc_release(value);
    value=rc_read_escaped(ctx,&input); assert(value && rc_type(value)==RC_SYMBOL && !strcmp(rc_text(value),"a b(\"")); rc_release(value);
    assert(!rc_read_escaped(ctx,&input) && !rc_has_error(ctx));
    input="\"bad\\q\""; assert(!rc_read_escaped(ctx,&input) && rc_has_error(ctx)); rc_clear_error(ctx);
    input="#sym\"bad\\x00symbol\""; assert(!rc_read_escaped(ctx,&input) && rc_has_error(ctx)); rc_clear_error(ctx);
    input="4.9406564584124654e-324"; value=rc_read(ctx,&input);
    assert(value && rc_type(value)==RC_NUMBER && rc_number_value(value)>0 && rc_number_value(value)<1e-307);
    rc_release(value);
    input="1e-9999"; assert(!rc_read(ctx,&input) && rc_has_error(ctx)); rc_clear_error(ctx);
    input="(1 2"; assert(!rc_read(ctx,&input) && rc_has_error(ctx)); rc_clear_error(ctx);
    input="\"unterminated"; assert(!rc_read(ctx,&input) && rc_has_error(ctx)); rc_clear_error(ctx);
    /* A closure captures an environment containing a vector that points back
     * to the closure. Repeated evaluation must leave no unrooted RC graph. */
    input="(let V (absvector 1) (let F (lambda X (<-address V 0)) (address-> V 0 F)))";
    RcValue *cycle_form=rc_read(ctx,&input); assert(cycle_form);
    rc_collect(ctx); size_t baseline=rc_stats(ctx).live_objects;
    for (size_t i=0;i<100;++i) {
        value=rc_eval(ctx,cycle_form); assert(value); rc_release(value);
        rc_collect(ctx); assert(rc_stats(ctx).live_objects==baseline);
    }
    rc_release(cycle_form);
    input="(let V (absvector 1) ((lambda X (simple-error \"owned failure\")) V))";
    RcValue *error_form=rc_read(ctx,&input); assert(error_form);
    rc_collect(ctx); baseline=rc_stats(ctx).live_objects;
    for (size_t i=0;i<100;++i) {
        assert(!rc_eval(ctx,error_form) && rc_has_error(ctx)); rc_clear_error(ctx);
        rc_collect(ctx); assert(rc_stats(ctx).live_objects==baseline);
    }
    rc_release(error_form);
    /* Stack argv, heap argv fallback, lexical snapshots and early failure. */
    number(ctx,"(let if (lambda X (+ X 1)) (if 41))",42);
    number(ctx,"(let Alias + (Alias (let Ignored (defun + (X Y) 99) 20) 22))",42);
    number(ctx,"(+ 20 22)",99);
    assert(rc_install_primitives(ctx));
    number(ctx,"(do (defun many (A B C D E F G H I J) (+ A J)) (many 20 0 0 0 0 0 0 0 0 22))",42);
    number(ctx,"(trap-error (many 1 2 (simple-error \"argv fault\") 4 5 6 7 8 9 10) (lambda E 42))",42);
    number(ctx,"(do (set *eval-arg-effect* 0) (do (trap-error (missing-before-args (set *eval-arg-effect* 1)) (lambda E 42)) (value *eval-arg-effect*)))",0);
    RcContext *foreign_ctx=rc_context_new(); assert(foreign_ctx);
    const char *foreign_input="(let X 42 X)";
    RcValue *foreign_form=rc_read(foreign_ctx,&foreign_input);assert(foreign_form);
    assert(!rc_eval(ctx,foreign_form)&&rc_has_error(ctx));rc_clear_error(ctx);
    rc_release(foreign_form);rc_context_free(foreign_ctx);
    rc_collect(ctx); rc_context_free(ctx);
    puts("RC evaluator: PASS");
    return 0;
}
