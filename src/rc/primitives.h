#ifndef SHEN_RC_PRIMITIVES_H
#define SHEN_RC_PRIMITIVES_H
#include "rc.h"
/* Register KL primitives and initialize host globals. eval-kl is installed by
 * rc_install_eval. Reinstall port hooks after loading kernel definitions. */
bool rc_install_primitives(RcContext *ctx);
bool rc_install_overwrites(RcContext *ctx);
/* Consumes candidate on every path. The snapshotted callee is checked before
 * reusing a cell; redefined/partial cons functions keep ordinary call semantics.
 * head/tail/function are borrowed. A fallback tail call stays stack safe. */
RcValue *rc_cons_call_reuse(RcContext *ctx, RcValue *function,
                            RcValue *owned_candidate, RcValue *head,
                            RcValue *tail, bool tail_position);
/* Final primitive tl read can detach a unique pair into an owned reusable
 * token. Source/dynamic tl replacements are called normally. */
RcValue *rc_tail_call_reuse(RcContext *ctx, RcValue *function,
                            RcValue *owned_candidate,
                            RcValue **owned_reuse_out);
/* Shen representation for diagnostics and standalone program output. */
bool rc_print(FILE *out, RcValue *value);
#endif
