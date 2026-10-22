#ifndef SHEN_RC_EMIT_H
#define SHEN_RC_EMIT_H
#include "rc.h"
/* Optional lean C backend (src/rc/lean.c), set only by rc-build --lean for
 * the kernel phase (--emit-kernel, or an application's kernel forms).
 * prelude writes declarations after the runtime includes; bootstrap writes
 * statements that run once every bootstrap install (primitives, kernel
 * defuns, overwrites, numeric hash) is done and before the kernel's
 * top-level forms. Either may be NULL. */
typedef struct RcLeanEmitHook {
    void *state;
    bool (*prelude)(void *state, RcContext *ctx, FILE *out);
    bool (*bootstrap)(void *state, RcContext *ctx, FILE *out);
} RcLeanEmitHook;
/* All AST values are borrowed for the duration of emission. No evaluator is
 * invoked to compile a form. On failure the context contains the diagnostic. */
typedef struct {
    const char *init_name;
    const char *source_label;
    const char *bootstrap_name; /* C identifier, default rc_bootstrap_kernel */
    bool emit_main;
    /* Borrowed chronological reader sidecar records: (call SYMBOL (literal...)),
     * (definition DEFUN-AST), (macro SYMBOL), (datatype SYMBOL),
     * (property OWNER KEY VALUE), or
     * (global SYMBOL literal). Literal data is nil/number/string/symbol/pairs.
     * Definitions compile natively; callbacks resolve the kernel fn wrapper. */
    RcValue *source_events;
    bool source_metadata; /* Replay reader events and record root Shen definitions. */
    bool conservative_ownership; /* Disable liveness/capture/owned-call/reuse optimization for comparison. */
    /* Runtime module mode (native tier): kernel forms must all be defuns and
     * no user forms, events, metadata or main are allowed. Instead of a
     * bootstrap, the only exported symbol is
     *   bool <module_entry>(RcContext *, RcModuleDefine define)
     * which builds each native closure and hands it to define (borrowed name
     * and closure) instead of publishing it. No primitives are installed. */
    const char *module_entry;
    /* Single expression entry: RcValue *entry(ctx, borrowed literal vector).
     * Returns owned result, without bootstrap or printing. Output binding
     * array is malloc-owned; elements borrow the input AST. */
    const char *expression_entry;
    RcValue *function_parameters; /* Non-NULL: entry returns a closure, not its result. */
    RcValue ***literal_bindings;
    size_t *literal_count;
    const RcLeanEmitHook *lean; /* NULL: no lean code (every other mode) */
} RcEmitOptions;
typedef bool (*RcModuleDefine)(RcContext *, RcValue *name, RcValue *function);
typedef struct {
    long defuns, toplevels, lambdas;
} RcEmitReport;
bool rc_emit_program(RcContext *ctx, FILE *out,
                     RcValue *const *kernel, size_t nkernel,
                     RcValue *const *user, size_t nuser,
                     const RcEmitOptions *options, RcEmitReport *report);
/* Reads a whole file into owned forms. Caller releases each form and frees
 * the array. Clean empty files succeed with count=0 and forms=NULL. */
bool rc_read_path_ex(RcContext *ctx,const char *path,bool escaped,RcValue ***forms,size_t *count);
bool rc_read_path(RcContext *ctx,const char *path,RcValue ***forms,size_t *count);
#endif
