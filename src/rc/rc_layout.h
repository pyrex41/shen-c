#ifndef SHEN_RC_LAYOUT_H
#define SHEN_RC_LAYOUT_H
/* Physical value layout, shared by the runtime and by generated code for
 * inline fast paths. It is part of the native-tier ABI digest, so a change
 * here invalidates cached native modules. Embedders keep using rc.h only. */
#include "rc.h"
#include <stddef.h>

/* Every value lives in a slab page (see RcPage) that records its context,
 * so values carry no context pointer, list links or size. A pair cell is the
 * header word, the scratch word and its two edges: 32 bytes. Other values use
 * one larger cell class. Only fields of a value's own payload may be touched:
 * a pair cell ends after as.pair. */
struct RcValue {
    /* One word: a 32-bit count (RC_IMMORTAL saturates), the type and flags.
     * Hot flags keep whole bytes; rarer certificates share the last byte. */
    uint32_t refs;
    uint8_t type; /* RcType, or RC_FREE_CELL on a free list */
    bool marked, reaches_vector;
    unsigned builtin_slot : 5; /* Canonical slot + 1, zero for ordinary values. */
    bool returns_second : 1; /* Immutable trusted exact identity-body certificate. */
    bool visited : 1; /* Graph-walk membership; clear outside a walk. */
    bool has_plan : 1; /* Pair with an evaluator side entry (eval_plan). */
    /* Trial counts are consumed before work links become live. Root discovery
     * only pushes its current object; transitive marking starts afterward.
     * Free cells link through work. */
    union { size_t trial; RcValue *work; };
    union {
        struct { double real; int64_t integer; bool exact; } number;
        struct {
            char *data; size_t length; uint64_t hash;
            RcValue *intern_next, *global, *function;
        } text;
        struct { RcValue *head, *tail; } pair;
        /* A source stays marked until freed; see rc_vector_set. */
        struct { RcValue **items; size_t length; bool cycle_source; RcValue *source_prev, *source_next; } vector;
        struct {
            RcValue *parent, **keys, **values; size_t count;
            RcValue *inline_binding[2];
            uint8_t key_classes;
        } env;
        struct {
            RcNative native;
            RcNativeOwned owned_native;
            size_t arity, capture_count, bound_count;
            RcValue **captures, **bound;
        } closure;
        struct { FILE *file; bool close, input; } stream;
        struct {
            RcValue *function; size_t argc; RcValue **argv; uint8_t guards;
            RcValue *inline_args[3];
        } tail;
    } as;
};
#define RC_FREE_CELL 0xff
#define RC_PAIR_CELL (offsetof(RcValue, as) + sizeof(((RcValue *)0)->as.pair))
_Static_assert(UINTPTR_MAX!=UINT64_MAX || RC_PAIR_CELL==32, "64-bit pair cells are 32 bytes");
_Static_assert(UINTPTR_MAX!=UINT64_MAX || sizeof(RcValue)==72,
               "64-bit RcValue storage must remain 72 bytes");
/* Three inline tail arguments fit in the pre-existing closure-sized union. */
_Static_assert(sizeof(((RcValue *)0)->as.tail) <= sizeof(((RcValue *)0)->as.closure),
               "inline tail arguments must not enlarge RcValue");
_Static_assert(sizeof(((RcValue *)0)->as.env) <= sizeof(((RcValue *)0)->as.closure),
               "inline environment binding must not enlarge RcValue");
/* Immortal values carry this sentinel count. Retain/release skip them with
 * one test; they are freed only at context teardown. Interned symbols are
 * immortal because the intern table already keeps them alive until then. A
 * counted object that reaches 2^32-1 owners saturates into an immortal: it is
 * then freed at teardown, never early. */
#define RC_IMMORTAL UINT32_MAX
/* Slab pages are aligned to their size, so a value's page, and from it the
 * context, is its address with the low bits cleared. Cells start after the
 * page header and are never returned to the system before teardown. */
#define RC_PAGE_BYTES ((size_t)64 * 1024)
typedef struct RcPage {
    RcContext *ctx;
    struct RcPage *next;
    size_t cell_bytes, used, limit; /* used: bump offset; limit: end offset */
    max_align_t align;
} RcPage;
/* The first fields of every context. */
typedef struct {
    bool error_active, acyclic;
    size_t since_collection, collection_interval;
    /* Lean C (lean_rt.h): the kernel's resolved symbols, and each symbol's
     * canonical function binding as recorded (and owned) by rc_lean_install
     * after the bootstrap installs (NULL when unbound). Both are NULL
     * until installed. lean_unsealed is SHEN_RC_UNSEALED, read once at
     * context creation: lean kernel calls then go through their symbols. */
    RcValue *const *lean_syms;
    RcValue *const *lean_canon;
    bool lean_unsealed;
} RcContextHot;
static inline RcContext *rc_value_context(const RcValue *v) {
    return ((const RcPage *)((uintptr_t)v & ~(uintptr_t)(RC_PAGE_BYTES - 1)))->ctx;
}
static inline bool rc_value_context_or_nil(RcContext *ctx, const RcValue *v) {
    return v->type == RC_NIL || rc_value_context(v) == ctx;
}
/* Canonical builtin slots, as in rc_internal.h's RcBuiltinSlot. */
enum { RC_FAST_HD = 0, RC_FAST_TL = 1, RC_FAST_CONSP = 2, RC_FAST_EQ = 5, RC_FAST_CONS = 14 };

/* Fast paths for generated code. Counter builds keep every operation in the
 * runtime so the counts stay exact. */
#if defined(RC_LAYOUT_FAST) && !(defined(SHEN_RC_STATS_COUNTERS) && SHEN_RC_STATS_COUNTERS)
#include "primitives.h"
static inline bool rc_fast_immediate(const RcValue *v) {
#if SHEN_RC_IMMEDIATE_INTEGERS
    return ((uintptr_t)v & 1u) != 0;
#else
    (void)v; return false;
#endif
}
static inline bool rc_fast_counted(const RcValue *v) {
    return v && !rc_fast_immediate(v) && v->refs != RC_IMMORTAL;
}
static inline RcValue *rc_fast_retain(RcValue *v) {
    if (rc_fast_counted(v)) v->refs++;
    return v;
}
static inline void rc_fast_release(RcValue *v) {
    if (!rc_fast_counted(v)) return;
    if (v->refs > 1) v->refs--; else rc_release(v);
}
/* A heap value of this context, of the given type. Nil has its own type. */
static inline bool rc_fast_is(RcContext *ctx, const RcValue *v, RcType type) {
    return v && !rc_fast_immediate(v) && v->type == type && v->type != RC_NIL && rc_value_context(v) == ctx;
}
/* The closure carries the canonical tag for slot. A stale tagged closure is
 * still that builtin, so the result is the same as the checked path. */
static inline bool rc_fast_builtin(RcContext *ctx, const RcValue *fn, unsigned slot) {
    return rc_fast_is(ctx, fn, RC_CLOSURE) && fn->builtin_slot == slot + 1;
}
static inline bool rc_fast_ok(RcContext *ctx) { return !((const RcContextHot *)ctx)->error_active; }
static inline void rc_fast_safepoint(RcContext *ctx) {
    const RcContextHot *hot = (const RcContextHot *)ctx;
    if (!hot->acyclic && hot->collection_interval && hot->since_collection >= hot->collection_interval) rc_safepoint(ctx);
}
static inline RcValue *rc_fast_function_get(RcContext *ctx, RcValue *symbol) {
    if (rc_fast_is(ctx, symbol, RC_SYMBOL) && symbol->as.text.function) return rc_fast_retain(symbol->as.text.function);
    return rc_function_get(ctx, symbol);
}
/* hd/tl: a head or tail that is itself a pending tail request takes the
 * checked path, which forces it outside tail position. */
static inline RcValue *rc_fast_hd_owned(RcContext *ctx, RcValue *fn, RcValue **args, bool tail) {
    RcValue *x = args[0];
    if (rc_fast_builtin(ctx, fn, RC_FAST_HD) && rc_fast_is(ctx, x, RC_PAIR) && rc_fast_ok(ctx)) {
        RcValue *h = x->as.pair.head;
        if (rc_fast_immediate(h) || !h || h->type != RC_TAIL) {
            rc_fast_retain(h); args[0] = NULL; rc_fast_release(x); return h;
        }
    }
    return rc_primitive_hd_owned(ctx, fn, args, tail);
}
static inline RcValue *rc_fast_tl_owned(RcContext *ctx, RcValue *fn, RcValue **args, bool tail) {
    RcValue *x = args[0];
    if (rc_fast_builtin(ctx, fn, RC_FAST_TL) && rc_fast_is(ctx, x, RC_PAIR) && rc_fast_ok(ctx)) {
        RcValue *t = x->as.pair.tail;
        if (rc_fast_immediate(t) || !t || t->type != RC_TAIL) {
            rc_fast_retain(t); args[0] = NULL; rc_fast_release(x); return t;
        }
    }
    return rc_primitive_tl_owned(ctx, fn, args, tail);
}
static inline RcValue *rc_fast_cons_owned(RcContext *ctx, RcValue *fn, RcValue **args, bool tail) {
    if (rc_fast_builtin(ctx, fn, RC_FAST_CONS) && rc_fast_ok(ctx)) {
        RcValue *head = args[0], *rest = args[1]; args[0] = NULL; args[1] = NULL;
        RcValue *v = rc_cons_owned(ctx, head, rest);
        if (!v) return NULL;
        if (!rc_fast_ok(ctx)) { rc_fast_release(v); return NULL; }
        rc_fast_safepoint(ctx);
        return v;
    }
    return rc_primitive_cons_owned(ctx, fn, args, tail);
}
static inline int rc_fast_consp_borrowed(RcContext *ctx, RcValue *fn, RcValue *const *args) {
    const RcValue *x = args[0];
    if (rc_fast_builtin(ctx, fn, RC_FAST_CONSP) && rc_fast_ok(ctx) &&
        (rc_fast_immediate(x) || (x && rc_value_context_or_nil(ctx, x))))
        return x && !rc_fast_immediate(x) && x->type == RC_PAIR;
    return rc_condition_consp_borrowed(ctx, fn, args);
}
static inline int rc_fast_consp_owned(RcContext *ctx, RcValue *fn, RcValue **args) {
    RcValue *x = args[0];
    if (rc_fast_builtin(ctx, fn, RC_FAST_CONSP) && rc_fast_ok(ctx) &&
        (rc_fast_immediate(x) || (x && rc_value_context_or_nil(ctx, x)))) {
        int result = !rc_fast_immediate(x) && x->type == RC_PAIR;
        args[0] = NULL; rc_fast_release(x); return result;
    }
    return rc_condition_consp_owned(ctx, fn, args);
}
/* = on values compared by identity: immediates, symbols and nil are equal
 * only when they are the same handle. Anything else takes rc_equal. */
static inline bool rc_fast_identity_class(const RcValue *v) {
    return v && (rc_fast_immediate(v) || v->type == RC_NIL || v->type == RC_SYMBOL);
}
/* Within one context a symbol or [] is equal only to itself: other symbols
 * have other names, and no other type compares equal to them. The other
 * operand must not be a pending tail request, which the checked path keeps. */
static inline bool rc_fast_unique_against(const RcValue *v, const RcValue *other) {
    return !rc_fast_immediate(v) && (v->type == RC_SYMBOL || v->type == RC_NIL) &&
           (rc_fast_immediate(other) || other->type != RC_TAIL);
}
static inline int rc_fast_eq_owned(RcContext *ctx, RcValue *fn, RcValue **args) {
    RcValue *a = args[0], *b = args[1];
    if (rc_fast_builtin(ctx, fn, RC_FAST_EQ) && rc_fast_ok(ctx) && a && b &&
        (rc_fast_immediate(a) || rc_value_context_or_nil(ctx, a)) &&
        (rc_fast_immediate(b) || rc_value_context_or_nil(ctx, b)) &&
        (a == b || (rc_fast_identity_class(a) && rc_fast_identity_class(b)) ||
         rc_fast_unique_against(a, b) || rc_fast_unique_against(b, a))) {
        args[0] = NULL; args[1] = NULL;
        rc_fast_release(a); rc_fast_release(b);
        return a == b;
    }
    return rc_condition_eq_owned(ctx, fn, args);
}
#define rc_retain(v) rc_fast_retain(v)
#define rc_release(v) rc_fast_release(v)
#define rc_function_get(c, s) rc_fast_function_get(c, s)
#define rc_primitive_hd_owned(c, f, a, t) rc_fast_hd_owned(c, f, a, t)
#define rc_primitive_tl_owned(c, f, a, t) rc_fast_tl_owned(c, f, a, t)
#define rc_primitive_cons_owned(c, f, a, t) rc_fast_cons_owned(c, f, a, t)
#define rc_condition_consp_borrowed(c, f, a) rc_fast_consp_borrowed(c, f, a)
#define rc_condition_consp_owned(c, f, a) rc_fast_consp_owned(c, f, a)
#define rc_condition_eq_owned(c, f, a) rc_fast_eq_owned(c, f, a)
#endif
#endif
