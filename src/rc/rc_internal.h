#ifndef SHEN_RC_INTERNAL_H
#define SHEN_RC_INTERNAL_H
#include "rc.h"
/* Runtime/primitive implementation contract. The installer alone registers
 * freshly created original builtins; this is not an embedding API. */
typedef enum {
 RC_BUILTIN_HD, RC_BUILTIN_TL, RC_BUILTIN_CONSP, RC_BUILTIN_NUMBERP,
 RC_BUILTIN_STRINGP, RC_BUILTIN_EQ, RC_BUILTIN_ADD, RC_BUILTIN_SUB,
 RC_BUILTIN_MUL, RC_BUILTIN_DIV, RC_BUILTIN_GT, RC_BUILTIN_LT,
 RC_BUILTIN_GE, RC_BUILTIN_LE, RC_BUILTIN_CONS, RC_BUILTIN_COUNT
} RcBuiltinSlot;
void rc_builtin_cache_set(RcContext *, RcBuiltinSlot, RcValue *);
bool rc_builtin_identity_matches(RcContext *, RcBuiltinSlot, RcValue *);
bool rc_builtin_call_matches(RcContext *, RcBuiltinSlot, RcValue *, size_t,
                              RcValue *const *);
/* Validated exact canonical call slot, or -1. Includes every argument check;
 * callers may enter the shared private tiny body without rechecking guards. */
int rc_builtin_call_slot(RcContext *, RcValue *, size_t, RcValue *const *);
#endif
