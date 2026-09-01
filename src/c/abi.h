#ifndef SHEN_C_ABI_H
#define SHEN_C_ABI_H

#include <stddef.h>
#include <stdint.h>

#include "context.h"
#include "kl.h"

/* Bump when overlay emit / install contract changes so stale modules
 * silently fall back to the loaded walker. */
#define SHEN_OVERLAY_FORMAT "shen-c-aot-overlay-1"

/*
 * Option 5 rung 1 embedding/codegen ABI. Generated defuns are C
 * NativeFunctions that intern/cons/apply primitives on shen_context /
 * Boehm. Do not wrap eval_kl_object on a source string as "codegen".
 * eval-kl remains for needs-eval shakes. Rung 2 (non-GC, no eval-kl) is
 * not this header.
 */

void shen_boot (shen_context* ctx, const char* home_path);
/* Re-apply C port overwrites after shaken defuns are installed (same
 * order as load_shen_kl_files: kernel first, then C). */
void shen_apply_port_overwrites (void);
/* After kernel KL boot: overwrite defuns with generated NativeFunctions.
 * Strong symbol is linked only into bin/shen-c (rust install_all).
 * libshenc has a weak no-op so runme-aot-app / tests do not pull kernel AOT.
 * SHEN_C_NO_AOT skips the call. Not overlay-after-load. */
void shen_kernel_aot_install_all (void);

KLObject* shen_intern (shen_context* ctx, const char* name);
KLObject* shen_cons (shen_context* ctx, KLObject* head, KLObject* tail);
KLObject* shen_hd (shen_context* ctx, KLObject* list);
KLObject* shen_tl (shen_context* ctx, KLObject* list);
KLObject* shen_empty_list (shen_context* ctx);
/* Boxed exact-arity prims for emit (klcompile inlinable set, minus
 * vector? which is a kernel defun). Long+long is the t* (+ Infs 1) path. */
KLObject* shen_add (shen_context* ctx, KLObject* x, KLObject* y);
KLObject* shen_sub (shen_context* ctx, KLObject* x, KLObject* y);
KLObject* shen_mul (shen_context* ctx, KLObject* x, KLObject* y);
KLObject* shen_div (shen_context* ctx, KLObject* x, KLObject* y);
KLObject* shen_lt (shen_context* ctx, KLObject* x, KLObject* y);
KLObject* shen_gt (shen_context* ctx, KLObject* x, KLObject* y);
KLObject* shen_lte (shen_context* ctx, KLObject* x, KLObject* y);
KLObject* shen_gte (shen_context* ctx, KLObject* x, KLObject* y);
KLObject* shen_eq (shen_context* ctx, KLObject* x, KLObject* y);
KLObject* shen_cons_p (shen_context* ctx, KLObject* x);
KLObject* shen_number_p (shen_context* ctx, KLObject* x);
KLObject* shen_string_p (shen_context* ctx, KLObject* x);
KLObject* shen_symbol_p (shen_context* ctx, KLObject* x);
KLObject* shen_absvector_p (shen_context* ctx, KLObject* x);

KLObject* shen_number_l (shen_context* ctx, long x);
KLObject* shen_number_d (shen_context* ctx, double x);
long shen_number_l_value (shen_context* ctx, KLObject* number_object);
int shen_is_number_l (shen_context* ctx, KLObject* object);

KLObject* shen_string (shen_context* ctx, const char* string);
const char* shen_string_value (shen_context* ctx, KLObject* string_object);

KLObject* shen_symbol (shen_context* ctx, const char* name);
KLObject* shen_symbol_function (shen_context* ctx, KLObject* symbol_object);

Vector* shen_vector (shen_context* ctx, long size);
void shen_vector_set (shen_context* ctx, Vector* vector, long index,
                      KLObject* object);
KLObject* shen_vector_get (shen_context* ctx, Vector* vector, long index);
KLObject** shen_arguments (shen_context* ctx, KLObject* function_object,
                           Vector* arguments);

void shen_register_defun (shen_context* ctx, const char* name, long arity,
                          NativeFunction* native_function);
KLObject* shen_apply (shen_context* ctx, KLObject* function_or_symbol,
                      Vector* arguments);
/* Named exact-arity AOT call: intern_static cache + stack arg array,
 * no Vector. Partial / extra arity falls back to shen_apply. */
KLObject* shen_apply_direct (shen_context* ctx, const char* name, long n,
                             KLObject** arguments);
/* Tail-position apply: bounce into shen_apply when already inside a
 * generated NativeFunction. Each shen_apply saves/restores bounce state
 * so nested map/walk/trap-error cannot clobber an outer bounce. Self-tails
 * use goto. Non-tail generated natives (shen_register_defun /
 * shen_native_closure) hop to a fresh C stack when the current thread
 * stack is below a bound (wrapping-complete t* overflow). C primitives
 * and eval_kl_object user/closure application are not hopped. */
KLObject* shen_tail_apply (shen_context* ctx, KLObject* function_or_symbol,
                           Vector* arguments);
KLObject* shen_tail_apply_direct (shen_context* ctx, const char* name, long n,
                                  KLObject** arguments);

KLObject* shen_true (shen_context* ctx);
KLObject* shen_false (shen_context* ctx);
int shen_is_boolean (shen_context* ctx, KLObject* object);
int shen_boolean_value (shen_context* ctx, KLObject* boolean_object);

KLObject* shen_native_closure (shen_context* ctx, long arity,
                               NativeFunction* native_function,
                               Vector* captures);
Vector* shen_native_captures (shen_context* ctx, KLObject* function_object);

typedef KLObject* (*shen_trap_body) (void* data);
typedef KLObject* (*shen_trap_handler) (KLObject* exception, void* data);

KLObject* shen_trap_error (shen_context* ctx, shen_trap_body body,
                           shen_trap_handler handler, void* data);
void shen_simple_error (shen_context* ctx, const char* message);
KLObject* shen_error_to_string (shen_context* ctx, KLObject* exception_object);

KLObject* shen_eval_kl (shen_context* ctx, KLObject* object);

typedef struct ShenOverlayNameArity {
  const char* name;
  long arity;
} ShenOverlayNameArity;

typedef struct ShenOverlayModule {
  const char* label;
  const char* format;
  uint64_t source_fnv;
  uint64_t kernel_fnv;
  const ShenOverlayNameArity* compiled;
  size_t ncompiled;
  void (*install) (shen_context* ctx);
} ShenOverlayModule;

uint64_t shen_fnv64 (const unsigned char* bytes, size_t n);
int shen_fnv64_file (const char* path, uint64_t* hash_out);
uint64_t shen_kernel_digest (const char* kernel_dir);

void shen_overlay_set_kernel_dir (const char* kernel_dir);
void shen_register_overlay (const ShenOverlayModule* module);
int shen_install_overlay (shen_context* ctx, const ShenOverlayModule* module);
int shen_install_overlay_if_match (shen_context* ctx,
                                   const ShenOverlayModule* module,
                                   const unsigned char* live_src,
                                   size_t live_len);
void shen_wrap_load_for_overlays (void);

#endif
