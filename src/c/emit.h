#ifndef SHEN_C_EMIT_H
#define SHEN_C_EMIT_H

#include <stdint.h>
#include <stdio.h>

#include "abi.h"

/*
 * Option 5 rung 1: shaken KLambda -> C NativeFunctions on shen_context /
 * Boehm. Exact-arity + - cons hd tl become ABI helpers; named calls
 * shen_apply_direct (intern cache + stack args). Self-tails become goto;
 * other tails bounce through shen_tail_apply_direct. This is not
 * eval_kl_object of a source string.
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

int shen_emit_program_ex (FILE* out,
                          KLObject** kernel_forms, long nkernel,
                          KLObject** user_forms, long nuser,
                          const char* init_name,
                          const char* source_label,
                          const char* extra_in_main,
                          ShenEmitReport* report);

/* Overlay module: sidecar defuns + install + (name, arity) + hashes.
 * Not a second main. Not eval_kl_object of the .shen string. */
int shen_emit_overlay (FILE* out,
                       KLObject** forms, long nforms,
                       const char* module_ident,
                       const char* label,
                       uint64_t source_fnv,
                       uint64_t kernel_fnv,
                       ShenEmitReport* report);

#endif
