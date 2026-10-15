#ifndef SHEN_RC_H
#define SHEN_RC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Inputs are borrowed and value results owned, unless explicitly documented.
 * Every live value must belong to the same context. NULL denotes failure.
 * C callers must release owned temporaries, including on error paths. */
typedef struct RcContext RcContext;
typedef struct RcValue RcValue;
typedef enum { RC_CYCLES_ALLOWED, RC_ACYCLIC_ONLY } RcCyclePolicy;
typedef enum {
    RC_NIL, RC_NUMBER, RC_STRING, RC_SYMBOL, RC_PAIR, RC_VECTOR,
    RC_CLOSURE, RC_STREAM, RC_TAIL
} RcType;
typedef RcValue *(*RcNative)(RcContext *, RcValue *self, size_t argc,
                             RcValue *const *argv);
/* Owned callbacks may take an argument by moving its pointer out of argv[i]
 * and setting argv[i] to NULL. Runtime releases every remaining slot after
 * callback return, on success or failure. self and the slot array are borrowed. */
typedef RcValue *(*RcNativeOwned)(RcContext *, RcValue *self, size_t argc,
                                  RcValue **argv);
typedef struct {
    size_t live_objects, live_bytes, total_allocations, total_frees;
    size_t collections, cycle_objects_freed, peak_objects;
    /* Actual managed-value reference count increments/decrements, including
     * internal edge operations; ownership transfers do not increment these. */
    size_t retain_calls, release_calls;
    size_t pair_allocations, pair_reuse_attempts, pair_reuses, pair_reuse_misses;
    size_t pair_tail_transfers, pair_tail_shares;
    size_t capture_edges_created, live_capture_edges, peak_capture_edges;
} RcStats;

RcContext *rc_context_new(void);
/* Optional restricted semantics: vector writes that introduce cycles fail.
 * SHEN_RC_ACYCLIC_ONLY=1 forces this policy and compiles out the cycle collector.
 * The default build/context continues to support unrestricted mutable cycles. */
RcContext *rc_context_new_with_policy(RcCyclePolicy policy);
bool rc_context_is_acyclic(const RcContext *ctx);
/* Invalidates every value in the context, including outstanding C roots. */
void rc_context_free(RcContext *ctx);
/* Defaults adapt the allocation budget to the surviving graph at each
 * collection. This setter selects an exact fixed budget; zero disables
 * automatic collection. Explicit rc_collect always remains available. */
void rc_set_collection_interval(RcContext *ctx, size_t allocations);
void rc_collect(RcContext *ctx);
void rc_safepoint(RcContext *ctx);
RcStats rc_stats(const RcContext *ctx);
RcValue *rc_retain(RcValue *value);
void rc_release(RcValue *value);
RcType rc_type(const RcValue *value);
size_t rc_refcount(const RcValue *value);
RcValue *rc_nil(void); /* Immortal singleton; release is harmless. */
bool rc_is_nil(const RcValue *value);

RcValue *rc_number(RcContext *ctx, double value);
RcValue *rc_integer(RcContext *ctx, int64_t value);
double rc_number_value(const RcValue *value);
bool rc_number_is_integer(const RcValue *value);
bool rc_number_is_exact_integer(const RcValue *value);
bool rc_number_is_int64(const RcValue *value);
int64_t rc_integer_value(const RcValue *value);
RcValue *rc_string(RcContext *ctx, const char *text);
RcValue *rc_string_n(RcContext *ctx, const char *text, size_t length);
RcValue *rc_symbol(RcContext *ctx, const char *name);
/* Immutable compiled-module descriptor; descriptor and names must have static
 * storage duration. The returned array and its symbol values are borrowed from
 * ctx, valid until context destruction. Each context resolves each descriptor
 * once, through the ordinary intern table. Function/global bindings remain
 * mutable and must be resolved when used, never cached in this table. */
typedef struct { const char *const *names; size_t count; } RcSymbolTable;
RcValue *const *rc_symbol_table(RcContext *ctx, const RcSymbolTable *table);
/* Text pointers and capture access are borrowed, valid while owner lives. */
const char *rc_text(const RcValue *value);
size_t rc_text_length(const RcValue *value);
RcValue *rc_true(RcContext *ctx);
RcValue *rc_false(RcContext *ctx);
bool rc_is_true(const RcValue *value);
bool rc_is_false(const RcValue *value);
bool rc_is_boolean(const RcValue *value);
RcValue *rc_cons(RcContext *ctx, RcValue *head, RcValue *tail);
/* Consumes candidate, including on failure. head/tail remain borrowed. A unique
 * pair is recycled; shared/nonpair candidates allocate a fresh pair. Direct
 * head/tail aliases of candidate also allocate, preserving the original value.
 * No detached storage token or invalid collector-visible value is exposed. */
RcValue *rc_cons_reuse(RcContext *ctx, RcValue *owned_candidate,
                      RcValue *head, RcValue *tail);
/* Consumes candidate. On success returns its owned tail and writes an owned
 * pair reuse candidate to out. Unique pairs transfer their tail edge, become
 * valid (nil . nil) pairs, and release the old head. Shared pairs retain their
 * tail and remain unchanged. On error out is NULL and candidate is released.
 * Callers must retain any needed old head before this consuming projection. */
RcValue *rc_pair_take_tail_reuse(RcContext *ctx, RcValue *owned_candidate,
                                 RcValue **owned_reuse_out);
RcValue *rc_head(RcContext *ctx, RcValue *pair);
RcValue *rc_tail(RcContext *ctx, RcValue *pair);
/* Borrowed access for traversal; NULL when the value is not a pair. */
RcValue *rc_head_borrowed(const RcValue *pair);
RcValue *rc_tail_borrowed(const RcValue *pair);
RcValue *rc_vector(RcContext *ctx, size_t length);
size_t rc_vector_length(const RcValue *vector);
RcValue *rc_vector_get(RcContext *ctx, RcValue *vector, size_t index);
bool rc_vector_set(RcContext *ctx, RcValue *vector, size_t index, RcValue *value);
RcValue *rc_closure(RcContext *ctx, size_t arity, RcNative native,
                    size_t capture_count, RcValue *const *captures);
RcValue *rc_closure_owned(RcContext *ctx, size_t arity, RcNativeOwned native,
                          size_t capture_count, RcValue *const *captures);
bool rc_closure_is_native(const RcValue *closure, RcNative native);
RcValue *rc_capture(const RcValue *self, size_t index); /* Borrowed. */
size_t rc_arity(const RcValue *function);
RcValue *rc_apply(RcContext *ctx, RcValue *function, size_t argc,
                  RcValue *const *argv);
/* function is borrowed. All argument slots are consumed and set NULL, on
 * success or failure; callers must own one reference per non-NULL input slot.
 * This also works with borrowed callbacks: their args remain runtime-owned. */
RcValue *rc_apply_owned_args(RcContext *ctx, RcValue *function, size_t argc,
                             RcValue **owned_argv);
/* Return directly from a native callback to request a stack-safe tail call. */
RcValue *rc_tailcall(RcContext *ctx, RcValue *function, size_t argc,
                     RcValue *const *argv);
/* Same slot transfer contract as rc_apply_owned_args. */
RcValue *rc_tailcall_owned_args(RcContext *ctx, RcValue *function, size_t argc,
                                RcValue **owned_argv);
/* Consumes an OWNED value (including on failure). Boolean results pass through;
 * tail requests acquire a deferred boolean check, without forcing recursion. */
RcValue *rc_require_boolean(RcContext *ctx, RcValue *owned_value);
RcValue *rc_stream(RcContext *ctx, FILE *file, bool close_on_release);
RcValue *rc_stream_with_direction(RcContext *ctx, FILE *file,
                                 bool close_on_release, bool input);
bool rc_stream_is_input(const RcValue *stream);
FILE *rc_stream_file(const RcValue *stream); /* Borrowed. */
bool rc_stream_close(RcContext *ctx, RcValue *stream);

RcValue *rc_global_get(RcContext *ctx, RcValue *symbol);
bool rc_global_set(RcContext *ctx, RcValue *symbol, RcValue *value);
RcValue *rc_function_get(RcContext *ctx, RcValue *symbol);
/* Missing binding returns NULL without setting an error; result is owned. */
RcValue *rc_function_lookup(RcContext *ctx, RcValue *symbol);
bool rc_function_set(RcContext *ctx, RcValue *symbol, RcValue *function);
/* On work-buffer allocation failure returns false and sets the owning context's
 * error. Callers distinguish failure with rc_has_error. Scalar equality never
 * allocates. */
bool rc_equal(const RcValue *left, const RcValue *right);
RcValue *rc_error(RcContext *ctx, const char *format, ...);
bool rc_has_error(const RcContext *ctx);
const char *rc_error_message(const RcContext *ctx);
void rc_clear_error(RcContext *ctx);

#endif
