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
/* Exact built-in leaf calls avoid a managed tail request. The selected closure
 * is borrowed and must stay rooted across argument evaluation and this call.
 * All argument slots are consumed on success/failure; replacements, partials,
 * and overapplication use the ordinary stack-safe call path. */
RcValue *rc_primitive_call_owned(RcContext *ctx, RcValue *function,
                                size_t argc, RcValue **owned_args,
                                bool tail_position);
/* Shen representation for diagnostics and standalone program output. */
bool rc_print(FILE *out, RcValue *value);
#endif
