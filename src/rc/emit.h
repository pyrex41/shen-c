#ifndef SHEN_RC_EMIT_H
#define SHEN_RC_EMIT_H
#include "rc.h"
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
} RcEmitOptions;
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
