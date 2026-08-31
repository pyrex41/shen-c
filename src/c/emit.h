#ifndef SHEN_C_EMIT_H
#define SHEN_C_EMIT_H

#include <stdio.h>

#include "abi.h"

/*
 * Option 5 rung 1: shaken KLambda -> C NativeFunctions on shen_context /
 * Boehm. Each defun is a C function that intern/cons/applies primitives.
 * Self-tails become goto; other tails bounce through shen_tail_apply.
 * This is not eval_kl_object of a source string.
 */

typedef struct ShenEmitReport {
  long defuns;
  long toplevels;
  long lambdas;
} ShenEmitReport;

int shen_read_kl_path (const char* path, KLObject*** forms_out, long* n_out);

int shen_emit_program (FILE* out,
                       KLObject** kernel_forms, long nkernel,
                       KLObject** user_forms, long nuser,
                       const char* init_name,
                       const char* source_label,
                       ShenEmitReport* report);

#endif
