#ifndef SHEN_RC_LEAN_H
#define SHEN_RC_LEAN_H
/* Lean C compiler front end (DESIGN B1): KLambda kernel defuns are resolved
 * to Core (alpha-renamed tree with classified call targets) and lowered to
 * LIR (direct-style ANF with join points, typed obj/bool/kref values).
 * lir_check validates every lowered function. Host-only: linked into
 * rc-build, never into libshenrc or generated programs. */
#include "rc.h"
#include <stdio.h>

typedef struct RcLean RcLean;
/* Kernel forms stay borrowed for the program's lifetime. lean_set lists the
 * defuns compiled lean (Known call targets); NULL means every kernel defun
 * that the seal list permits. NULL result: error set on ctx. */
RcLean *rc_lean_new(RcContext *ctx, RcValue *const *forms, size_t nforms,
                    const char *const *lean_set, size_t nset);
void rc_lean_free(RcLean *lean);
/* Print Core and LIR of defun name. False with the ctx error set when the
 * name is not a kernel defun or the function fails lowering or checking. */
bool rc_lean_dump(RcLean *lean, const char *name, FILE *out);
/* Lower and check every kernel defun. Writes one line per failure and a
 * summary line to out; false if any lowered function fails lir_check. A
 * function that uses an unsupported construct is counted as rejected (the
 * generic emitter compiles it), not as a failure. */
bool rc_lean_check_all(RcLean *lean, FILE *out);
/* Generated-kernel hooks (RcLeanEmitHook): the lean runtime prelude, and
 * the bootstrap statement that installs the lean symbol table and records
 * the canonical bindings (rc_lean_install) once every install is done. */
bool rc_lean_emit_prelude(RcLean *lean, FILE *out);
bool rc_lean_emit_bootstrap(RcLean *lean, FILE *out);
/* The explicit seal list (DESIGN §4, seal-list constraints). Never-seal
 * names are never Known call targets: they always resolve through their
 * binding (primitives guarded, everything else Global). Guarded kernel
 * defuns are Known only under a per-site binding guard. */
bool rc_lean_never_sealed(const char *name);
bool rc_lean_guarded_defun(const char *name);
#endif
