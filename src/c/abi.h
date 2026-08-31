#ifndef SHEN_C_ABI_H
#define SHEN_C_ABI_H

#include "context.h"
#include "kl.h"

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

KLObject* shen_intern (shen_context* ctx, const char* name);
KLObject* shen_cons (shen_context* ctx, KLObject* head, KLObject* tail);
KLObject* shen_hd (shen_context* ctx, KLObject* list);
KLObject* shen_tl (shen_context* ctx, KLObject* list);
KLObject* shen_empty_list (shen_context* ctx);

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
/* Tail-position apply: bounce into shen_apply when already inside a
 * generated NativeFunction. Each shen_apply saves/restores bounce state
 * so nested map/walk/trap-error cannot clobber an outer bounce. Self-tails
 * use goto. Non-tail generated natives (shen_register_defun /
 * shen_native_closure) hop to a fresh C stack when the current thread
 * stack is below a bound (wrapping-complete t* overflow). C primitives
 * and eval_kl_object user/closure application are not hopped. */
KLObject* shen_tail_apply (shen_context* ctx, KLObject* function_or_symbol,
                           Vector* arguments);

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

#endif
