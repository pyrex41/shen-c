#ifndef SHEN_RC_EVAL_H
#define SHEN_RC_EVAL_H
#include "rc.h"
/* Evaluate an already parsed KLambda expression. Input borrowed, result owned.
 * Dynamic definitions share the context function table with native functions.
 * Tail applications use the runtime trampoline, including mutual recursion. */
RcValue *rc_eval(RcContext *ctx, RcValue *expression);
bool rc_install_eval(RcContext *ctx);
#endif
