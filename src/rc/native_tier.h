#ifndef SHEN_RC_NATIVE_TIER_H
#define SHEN_RC_NATIVE_TIER_H
#include "rc.h"
/* Optional CLI-only tier: interpreted defuns that become hot are compiled
 * with the RC emitter and the system C compiler, loaded with dlopen and
 * published through rc_function_set while their binding is unchanged.
 * Configured by environment (off unless SHEN_RC_NATIVE is set):
 *   SHEN_RC_NATIVE=1|count     enable promotion, or count calls only
 *   SHEN_RC_NATIVE_THRESHOLD=N calls before promotion; 0 promotes at definition
 *   SHEN_RC_NATIVE_CC, SHEN_RC_NATIVE_CFLAGS  compiler and optimization flags
 *   SHEN_RC_NATIVE_CACHE=DIR|off  content-addressed shared object cache
 *   SHEN_RC_NATIVE_STATS=1     report counters on stderr at exit
 * Every failure keeps the interpreted definition. Loaded objects are never
 * closed. attach never fails the host; detach must precede rc_context_free. */
void rc_native_tier_attach(RcContext *ctx);
void rc_native_tier_detach(RcContext *ctx);
#endif
