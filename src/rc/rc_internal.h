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
/* Optional evaluator observer for a host-side native tier. Generated
 * applications never attach one. defun runs after an interpreted defun has
 * published fn (old is the previous binding pointer, compared only); call runs
 * at interpreted callback entry with self rooted by the dispatcher; safepoint
 * runs at eval-kl entry with no error pending, where publishing a function
 * binding is exactly as safe as an ordinary runtime redefinition. All inputs
 * are borrowed; hooks must leave no error set and must not evaluate code
 * other than through ordinary function publication. */
typedef struct {
    void (*defun)(RcContext *,void *state,RcValue *name,RcValue *definition,
                  RcValue *fn,const RcValue *old);
    void (*call)(RcContext *,void *state,RcValue *self);
    void (*safepoint)(RcContext *,void *state);
} RcTierHooks;
void rc_tier_attach(RcContext *,const RcTierHooks *,void *state);
const RcTierHooks *rc_tier_hooks(const RcContext *,void **state);
/* Context-local compiler configuration; native code remains process resident. */
int rc_execution_mode(const RcContext *);
void **rc_native_compile_slot(RcContext *);
size_t rc_interpreter_entries(const RcContext *);
bool rc_interpreter_enter(RcContext *);
/* Permanently permits interpreter entry after a failed native compile; false
 * under SHEN_RC_VERIFY_NATIVE, which keeps compiler failures fatal. */
bool rc_interpreter_allow_fallback(RcContext *);
#endif
