#ifndef SHEN_C_EMIT_RC_H
#define SHEN_C_EMIT_RC_H
#include "emit.h"
/* The reader/compiler process may use the legacy frontend allocator. The
 * generated translation unit uses only the independent RcValue ABI. */
int shen_emit_rc_program(FILE *out, KLObject **kernel, long nkernel,
                        KLObject **user, long nuser, const char *init,
                        const char *label, ShenEmitReport *report);
#endif
