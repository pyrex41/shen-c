#ifndef SHEN_RC_NATIVE_COMPILE_H
#define SHEN_RC_NATIVE_COMPILE_H
#include "rc.h"
/* Borrowed expression, owned result. Never interprets or initializes kernel. */
RcValue *rc_native_eval(RcContext *,RcValue *);
void rc_native_compile_free(void *);
#endif
