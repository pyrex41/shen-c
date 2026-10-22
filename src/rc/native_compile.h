#ifndef SHEN_RC_NATIVE_COMPILE_H
#define SHEN_RC_NATIVE_COMPILE_H
#include "rc.h"
/* Borrowed expression, owned result. Never initializes the kernel. Interprets a
 * form only when its C compile fails or times out outside SHEN_RC_VERIFY_NATIVE. */
RcValue *rc_native_eval(RcContext *,RcValue *);
void rc_native_compile_free(void *);
#endif
