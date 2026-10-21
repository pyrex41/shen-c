#ifndef SHEN_RC_EVAL_H
#define SHEN_RC_EVAL_H
#include "rc.h"
/* Evaluate an already parsed KLambda expression. Input borrowed, result owned.
 * Dynamic definitions share the context function table with native functions.
 * Tail applications use the runtime trampoline, including mutual recursion.
 * Native compile-before-execute is the default. SHEN_RC_EXECUTION=interpret
 * at context creation explicitly selects the diagnostic interpreter. Uncached native code needs
 * a system compiler. A failed or timed-out C compile interprets that one form
 * instead, unless SHEN_RC_VERIFY_NATIVE makes it an error. */
RcValue *rc_eval(RcContext *ctx, RcValue *expression);
/* Compiler-failure fallback: a function of parameters over body, or with NULL
 * parameters the value of expression. The caller has enabled interpreter entry. */
RcValue *rc_interpret_fallback(RcContext *ctx, RcValue *expression, RcValue *parameters);
bool rc_install_eval(RcContext *ctx);
#endif
