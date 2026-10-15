#ifndef SHEN_RC_PRIMITIVES_H
#define SHEN_RC_PRIMITIVES_H
#include "rc.h"
/* Register KL primitives and initialize host globals. eval-kl is installed by
 * rc_install_eval. Reinstall port hooks after loading kernel definitions. */
bool rc_install_primitives(RcContext *ctx);
bool rc_install_overwrites(RcContext *ctx);
/* Shen representation for diagnostics and standalone program output. */
bool rc_print(FILE *out, RcValue *value);
#endif
