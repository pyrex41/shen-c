#ifndef SHEN_RC_INTERNAL_H
#define SHEN_RC_INTERNAL_H
#include "rc.h"
/* Original cn primitive only: borrowed same-context strings, owned result.
 * Allocate the final payload once, preserving embedded NULs and aliases. */
RcValue *rc_string_concat(RcContext *,const RcValue *,const RcValue *);
/* Exploration counters compile away in production. These count payload work,
 * not exclusive CPU time; reset only in diagnostic drivers between phases. */
typedef enum {
 RC_FUSION_STRINGS, RC_FUSION_STRING_BYTES, RC_FUSION_STRING_COPIES,
 RC_FUSION_CONCATS, RC_FUSION_CONCAT_TEMP_BYTES,
 RC_FUSION_TAILS, RC_FUSION_TAIL_BYTES,
 RC_FUSION_VECTORS, RC_FUSION_VECTOR_SLOTS,
 RC_FUSION_VECTOR_GETS, RC_FUSION_VECTOR_SETS, RC_FUSION_COUNT
} RcFusionCounter;
typedef struct { size_t count[RC_FUSION_COUNT]; } RcFusionStats;
static inline const char *rc_fusion_name(size_t i) {
 static const char *const names[]={"strings","string_payload_bytes","string_copy_bytes",
  "concat_calls","concat_temporary_bytes","tail_calls","tail_copy_bytes",
  "vectors","vector_slots","vector_get_calls","vector_set_calls"};
 return i<RC_FUSION_COUNT ? names[i] : "invalid";
}
#ifdef SHEN_RC_FUSION_DIAGNOSTICS
void rc_fusion_record(RcContext *,RcFusionCounter,size_t);
RcFusionStats rc_fusion_stats(const RcContext *);
void rc_fusion_reset(RcContext *);
#else
#define rc_fusion_record(ctx, counter, amount) ((void)0)
static inline RcFusionStats rc_fusion_stats(const RcContext *ctx) {
 (void)ctx;RcFusionStats result={{0}};return result;
}
static inline void rc_fusion_reset(RcContext *ctx) { (void)ctx; }
#endif
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
/* The caller roots list. Only immutable pair-spine metadata is cached; no
 * callbacks, allocation or collection occurs. out is written only on OK. */
typedef enum { RC_LIST_LENGTH_OK, RC_LIST_LENGTH_IMPROPER,
               RC_LIST_LENGTH_OVERFLOW } RcListLengthStatus;
RcListLengthStatus rc_list_length_cached(RcValue *list,size_t *out);
/* Consumes an owned evaluator argument result. Same-context values pass;
 * tail requests acquire a deferred context check without forcing recursion.
 * Completion preserves the generic owned-call operand error diagnostic. */
RcValue *rc_eval_require_argument_context(RcContext *,RcValue *);
/* Private interpreted callback layout: [body, environment, parameter keys...].
 * Validates context, callback identity, original arity and exact capture count.
 * The returned span is borrowed; the active runtime call must keep self rooted
 * until callback return. Partial closures preserve the original capture shape. */
RcValue *const *rc_eval_captures(RcContext *, RcValue *, RcNative, size_t arity);
/* Only interpreted callbacks may use this trusted entry: make_function has validated
 * immutable parameter keys/captured parent, and runtime dispatch has checked
 * every argv value against ctx. Still checks the exact callback/capture layout
 * and storage multiplication bounds. Returns owned ENV and borrowed body while
 * the active self remains rooted. The public rc_env_frame keeps all validation. */
RcValue *rc_eval_env_frame(RcContext *, RcValue *, RcNative, size_t arity,
                           RcValue *const *argv, RcValue **body);
/* Quiet borrowed reads. Caller roots the symbol/vector and proves context,
 * bounds and the absence of callbacks/mutation until the borrow ends. */
RcValue *rc_function_slot_borrowed(RcContext *,RcValue *symbol);
RcValue *rc_vector_slot_borrowed(RcValue *vector,size_t index);
/* One-entry function-dependency cache, private to guarded Prolog bodies.
 * Keys are exact intern roots; every rc_function_set invalidates the cache. */
bool rc_prolog_guard_get(RcContext *,RcValue *const *keys,unsigned *mask);
void rc_prolog_guard_set(RcContext *,RcValue *const *keys,unsigned mask);
/* Prepared evaluation private contracts. Local coordinates are emitted only
 * from exact immutable lexical binders. No callback/collection occurs here.
 * Plans contain code/parameter names and original AST descendants only, so
 * attaching one does not introduce a new vector path or a backedge to self. */
bool rc_eval_plan_environment(const RcValue *environment);
RcValue *rc_eval_plan_get(RcValue *expression);
void rc_eval_plan_set(RcValue *expression,RcValue *plan);
bool rc_eval_plan_heat(RcValue *expression);
void rc_eval_plan_promote(RcValue *self,RcNative prepared,RcValue *plan);
#endif
