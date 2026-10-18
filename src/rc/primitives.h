#ifndef SHEN_RC_PRIMITIVES_H
#define SHEN_RC_PRIMITIVES_H
#include "rc.h"
/* Register KL primitives and initialize host globals. eval-kl is installed by
 * rc_install_eval. Reinstall port hooks after loading kernel definitions. */
bool rc_install_primitives(RcContext *ctx);
bool rc_install_overwrites(RcContext *ctx);
/* Install a port-local hashkey wrapper after the pinned kernel boots.  Numeric
 * keys use the equality-compatible canonical spelling; all other values and
 * later redefinitions retain the kernel's ordinary path. */
bool rc_install_numeric_hash_owned(RcContext *ctx, RcNativeOwned expected);
bool rc_install_numeric_hash_borrowed(RcContext *ctx, RcNative expected);
/* Exact, unbound built-in do returns its second argument unchanged. */
bool rc_primitive_is_do(const RcValue *function);
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
/* Private owned-call boundaries. The caller holds a strong function owner
 * until completion and supplies a private scratch argv span with no observable
 * outside alias. Slots are consumed on every path; public borrowed/owned apply
 * contracts continue to use their independent runtime snapshots. */
RcValue *rc_apply_rooted_owned_args(RcContext *,RcValue *,size_t,RcValue **);
RcValue *rc_primitive_call_rooted_owned(RcContext *,RcValue *,size_t,RcValue **,bool);
/* Fixed-arity builtin specializations. Function remains a borrowed snapshot;
 * argument arrays contain one owned slot (two for eq) and are consumed on
 * every path. Identity mismatches use ordinary checked dispatch. */
RcValue *rc_primitive_hd_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_tl_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_consp_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_numberp_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_stringp_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_eq_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_cons_owned(RcContext *,RcValue *,RcValue **,bool);
/* Fixed-arity arithmetic/comparison variants use the same checked ownership
 * contract as the tiny entry points above, with two owned argument slots. */
RcValue *rc_primitive_add_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_sub_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_mul_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_div_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_gt_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_lt_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_ge_owned(RcContext *,RcValue *,RcValue **,bool);
RcValue *rc_primitive_le_owned(RcContext *,RcValue *,RcValue **,bool);
/* Private compiler condition boundaries: consume owned arguments and return
 * 0/1, -1 with an error, or -2 for a non-boolean completion. Replacements
 * use ordinary non-tail checked apply; the emitter selects the original
 * if/cond or boolean RHS validation diagnostic for -2. */
int rc_condition_consp_owned(RcContext *,RcValue *,RcValue **);
int rc_condition_numberp_owned(RcContext *,RcValue *,RcValue **);
int rc_condition_stringp_owned(RcContext *,RcValue *,RcValue **);
int rc_condition_eq_owned(RcContext *,RcValue *,RcValue **);
int rc_condition_gt_owned(RcContext *,RcValue *,RcValue **);
int rc_condition_lt_owned(RcContext *,RcValue *,RcValue **);
int rc_condition_ge_owned(RcContext *,RcValue *,RcValue **);
int rc_condition_le_owned(RcContext *,RcValue *,RcValue **);
/* Internal emitted intrinsic for the complete pinned assoc AST only. deps are
 * interned symbols assoc, =, cons?, hd, tl; args/deps/handled must be non-NULL.
 * On deoptimization, args and errors
 * are untouched. On handled success/failure, both owned args are consumed. */
RcValue *rc_assoc_try_owned(RcContext *,RcValue *self,RcValue **args,
                            RcValue *const *deps,bool *handled);
/* Exact pinned Prolog bodies only. Misses leave arguments/errors untouched;
 * hits consume all slots. Dependency checks precede a callback-free borrowed
 * walk. The compiler predicate certifies the currently selected pvar body. */
RcValue *rc_pvar_try_owned(RcContext *,RcValue *self,RcValue **args,
                           RcValue *const *deps,bool *handled);
RcValue *rc_lazyderef_try_owned(RcContext *,RcValue *self,RcValue **args,
    RcValue *const *deps,bool (*certified_pvar)(RcValue *),bool *handled);
/* Shen representation for diagnostics and standalone program output. */
bool rc_print(FILE *out, RcValue *value);
#endif
