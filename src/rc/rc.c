#include "rc.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define RC_ASAN 1
# endif
#endif
#if defined(__SANITIZE_ADDRESS__)
# define RC_ASAN 1
#endif
#ifdef RC_ASAN
# include <sanitizer/asan_interface.h>
# define RC_POISON(p) __asan_poison_memory_region((p), sizeof(RcValue))
# define RC_UNPOISON(p) __asan_unpoison_memory_region((p), sizeof(RcValue))
#else
# define RC_POISON(p) ((void)(p))
# define RC_UNPOISON(p) ((void)(p))
#endif

#define RC_OBJECT_CACHE_LIMIT 1024

struct RcValue {
    RcType type;
    size_t refs, bytes, trial, visit_epoch;
    bool marked;
    RcContext *ctx;
    RcValue *prev, *next, *work;
    union {
        struct { double real; int64_t integer; bool exact; } number;
        struct {
            char *data; size_t length; uint64_t hash;
            RcValue *intern_next, *global, *function;
        } text;
        struct { RcValue *head, *tail; } pair;
        struct { RcValue **items; size_t length; } vector;
        struct {
            RcNative native;
            RcNativeOwned owned_native;
            size_t arity, capture_count, bound_count;
            RcValue **captures, **bound;
        } closure;
        struct { FILE *file; bool close, input; } stream;
        struct {
            RcValue *function; size_t argc; RcValue **argv; bool require_boolean;
            RcValue *inline_args[3];
        } tail;
    } as;
};
/* Three inline tail arguments fit in the pre-existing closure-sized union. */
_Static_assert(sizeof(((RcValue *)0)->as.tail) <= sizeof(((RcValue *)0)->as.closure),
               "inline tail arguments must not enlarge RcValue");
typedef struct RcResolvedSymbols {
    const RcSymbolTable *table;
    struct RcResolvedSymbols *next;
    RcValue *values[];
} RcResolvedSymbols;
struct RcContext {
    RcValue *objects;
    RcValue *object_cache[RC_OBJECT_CACHE_LIMIT];
    bool destroying;
    RcValue **symbols;
    size_t symbol_capacity, symbol_count;
    RcResolvedSymbols *compiled_symbols;
    RcValue *true_symbol, *false_symbol; /* Borrowed intern-table roots. */
    RcStats stats;
    size_t collection_interval, since_collection, graph_slots, visit_epoch;
    char *error;
    bool collecting, error_active, adaptive_collection, acyclic;
};
static RcValue nil_value = {.type = RC_NIL, .refs = SIZE_MAX};

RcValue *rc_error(RcContext *ctx, const char *format, ...) {
    if (!ctx) return NULL;
    ctx->error_active = true;
    va_list ap, copy;
    va_start(ap, format);
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    char *message = n < 0 ? NULL : malloc((size_t)n + 1);
    if (message) vsnprintf(message, (size_t)n + 1, format, ap);
    va_end(ap);
    free(ctx->error);
    ctx->error = message;
    /* Allocation failure must still be observable. */
    if (!message) {
        ctx->error = malloc(20);
        if (ctx->error) memcpy(ctx->error, "out of memory", 14);
    }
    return NULL;
}
bool rc_has_error(const RcContext *ctx) { return ctx && ctx->error_active; }
const char *rc_error_message(const RcContext *ctx) {
    return ctx && ctx->error ? ctx->error : ctx && ctx->error_active ? "out of memory" : "";
}
void rc_clear_error(RcContext *ctx) { if (ctx) { free(ctx->error); ctx->error = NULL; ctx->error_active = false; } }

static RcValue *allocate(RcContext *ctx, RcType type, size_t extra) {
    if (!ctx || extra > SIZE_MAX - sizeof(RcValue)) return rc_error(ctx, "allocation size overflow");
    RcValue *v;
    if (ctx->stats.cached_objects) {
        v = ctx->object_cache[--ctx->stats.cached_objects];
        ctx->stats.cached_bytes -= sizeof(*v);
        RC_UNPOISON(v);
        memset(v, 0, sizeof(*v));
    } else {
        v = calloc(1, sizeof(*v));
        if (!v) return rc_error(ctx, "out of memory");
        ctx->stats.system_allocations++;
    }
    v->type = type; v->refs = 1; v->ctx = ctx; v->bytes = sizeof(*v) + extra;
    v->next = ctx->objects;
    if (v->next) v->next->prev = v;
    ctx->objects = v;
    ctx->stats.live_objects++;
    ctx->stats.live_bytes += v->bytes;
    ctx->stats.total_allocations++;
    if (type == RC_PAIR) ctx->stats.pair_allocations++;
    if (type == RC_CLOSURE) {
        size_t captures = extra / sizeof(RcValue *);
        ctx->stats.capture_edges_created += captures;
        ctx->stats.live_capture_edges += captures;
        if (ctx->stats.live_capture_edges > ctx->stats.peak_capture_edges)
            ctx->stats.peak_capture_edges = ctx->stats.live_capture_edges;
    }
    ctx->since_collection++;
    switch (type) {
    case RC_PAIR: ctx->graph_slots += 2; break;
    case RC_VECTOR: case RC_CLOSURE: ctx->graph_slots += extra / sizeof(RcValue *); break;
    case RC_TAIL: ctx->graph_slots++; break;
    default: break;
    }
    if (ctx->stats.live_objects > ctx->stats.peak_objects) ctx->stats.peak_objects = ctx->stats.live_objects;
    return v;
}
RcContext *rc_context_new(void) {
    return rc_context_new_with_policy(RC_CYCLES_ALLOWED);
}
RcContext *rc_context_new_with_policy(RcCyclePolicy policy) {
    if (policy != RC_CYCLES_ALLOWED && policy != RC_ACYCLIC_ONLY) return NULL;
    RcContext *ctx = calloc(1, sizeof(*ctx));
    if (ctx) {
        ctx->collection_interval = 4096; ctx->adaptive_collection = true;
#if defined(SHEN_RC_ACYCLIC_ONLY) && SHEN_RC_ACYCLIC_ONLY
        ctx->acyclic = true;
#else
        ctx->acyclic = policy == RC_ACYCLIC_ONLY;
#endif
    }
    return ctx;
}
bool rc_context_is_acyclic(const RcContext *ctx) { return ctx && ctx->acyclic; }
RcValue *rc_retain(RcValue *v) {
    if (v && v != &nil_value) {
        if (v->refs == SIZE_MAX) abort();
        v->ctx->stats.retain_calls++;
        v->refs++;
    }
    return v;
}
typedef void (*EdgeFn)(RcValue *, void *);
static void edges(RcValue *v, EdgeFn fn, void *data) {
    switch (v->type) {
    case RC_PAIR: fn(v->as.pair.head, data); fn(v->as.pair.tail, data); break;
    case RC_VECTOR:
        for (size_t i = 0; i < v->as.vector.length; ++i) fn(v->as.vector.items[i], data);
        break;
    case RC_CLOSURE:
        for (size_t i = 0; i < v->as.closure.capture_count; ++i) fn(v->as.closure.captures[i], data);
        for (size_t i = 0; i < v->as.closure.bound_count; ++i) fn(v->as.closure.bound[i], data);
        break;
    case RC_TAIL:
        fn(v->as.tail.function, data);
        for (size_t i = 0; i < v->as.tail.argc; ++i) fn(v->as.tail.argv[i], data);
        break;
    default: break;
    }
}
static void unlink_object(RcValue *v) {
    RcContext *ctx = v->ctx;
    if (v->prev) v->prev->next = v->next; else ctx->objects = v->next;
    if (v->next) v->next->prev = v->prev;
    ctx->stats.live_objects--;
    ctx->stats.live_bytes -= v->bytes;
    ctx->stats.total_frees++;
    switch (v->type) {
    case RC_PAIR: ctx->graph_slots -= 2; break;
    case RC_VECTOR: ctx->graph_slots -= v->as.vector.length; break;
    case RC_CLOSURE:
        ctx->graph_slots -= v->as.closure.capture_count + v->as.closure.bound_count;
        ctx->stats.live_capture_edges -= v->as.closure.capture_count;
        break;
    case RC_TAIL: ctx->graph_slots -= 1 + v->as.tail.argc; break;
    default: break;
    }
}
static void raw_free(RcValue *v) {
    switch (v->type) {
    case RC_STRING: case RC_SYMBOL: free(v->as.text.data); break;
    case RC_VECTOR: free(v->as.vector.items); break;
    case RC_CLOSURE: free(v->as.closure.captures); free(v->as.closure.bound); break;
    case RC_TAIL: if (v->as.tail.argv != v->as.tail.inline_args) free(v->as.tail.argv); break;
    case RC_STREAM: if (v->as.stream.file && v->as.stream.close) fclose(v->as.stream.file); break;
    default: break;
    }
    RcContext *ctx = v->ctx;
    if (!ctx->destroying && ctx->stats.cached_objects < RC_OBJECT_CACHE_LIMIT) {
        ctx->object_cache[ctx->stats.cached_objects++] = v;
        ctx->stats.cached_bytes += sizeof(*v);
        if (ctx->stats.cached_bytes > ctx->stats.peak_cached_bytes)
            ctx->stats.peak_cached_bytes = ctx->stats.cached_bytes;
        RC_POISON(v);
    } else {
        ctx->stats.system_frees++;
        free(v);
    }
}
static void release_edge(RcValue *v, void *data) {
    if (!v || v == &nil_value) return;
    RcValue **work = data;
    if (!v->refs) abort();
    v->ctx->stats.release_calls++;
    if (--v->refs == 0) { v->work = *work; *work = v; }
}
void rc_release(RcValue *v) {
    RcValue *work = NULL;
    release_edge(v, &work);
    while (work) {
        RcValue *dead = work;
        work = dead->work;
        unlink_object(dead);
        edges(dead, release_edge, &work);
        raw_free(dead);
    }
}
#if !defined(SHEN_RC_ACYCLIC_ONLY) || !SHEN_RC_ACYCLIC_ONLY
static void subtract_edge(RcValue *v, void *unused) {
    (void)unused;
    if (v && v != &nil_value) { if (!v->trial) abort(); v->trial--; }
}
static void mark_edge(RcValue *v, void *data) {
    if (!v || v == &nil_value || v->marked) return;
    RcValue **work = data;
    v->marked = true; v->work = *work; *work = v;
}
static void release_live_edge(RcValue *v, void *unused) {
    (void)unused;
    if (v && v != &nil_value && v->marked) rc_release(v);
}
#endif
void rc_collect(RcContext *ctx) {
#if defined(SHEN_RC_ACYCLIC_ONLY) && SHEN_RC_ACYCLIC_ONLY
    (void)ctx;
#else
    if (!ctx || ctx->acyclic || ctx->collecting) return;
    ctx->collecting = true;
    ctx->stats.collections++;
    ctx->since_collection = 0;
    /* Residual refs after subtracting every heap edge are exactly the owned C
     * handles and context roots. Reachability from them is the live graph.
     * No stack addresses or payload bytes are interpreted as pointers. */
    for (RcValue *v = ctx->objects; v; v = v->next) { v->trial = v->refs; v->marked = false; }
    for (RcValue *v = ctx->objects; v; v = v->next) edges(v, subtract_edge, NULL);
    RcValue *work = NULL;
    for (RcValue *v = ctx->objects; v; v = v->next) if (v->trial) mark_edge(v, &work);
    while (work) { RcValue *v = work; work = v->work; edges(v, mark_edge, &work); }
    RcValue *dead = NULL;
    for (RcValue *v = ctx->objects, *next; v; v = next) {
        next = v->next;
        if (!v->marked) { unlink_object(v); v->work = dead; dead = v; ctx->stats.cycle_objects_freed++; }
    }
    /* Keep every dead object allocated until all edge classifications are done. */
    for (RcValue *v = dead; v; v = v->work) edges(v, release_live_edge, NULL);
    while (dead) { RcValue *next = dead->work; raw_free(dead); dead = next; }
    if (ctx->adaptive_collection) {
        /* Freeze the next budget from survivors, including pointer slots in
         * large vectors. Recomputing from the current growing heap on every
         * safepoint would indefinitely postpone collection of pure cycles. */
        size_t objects = ctx->stats.live_objects;
        size_t work = objects > SIZE_MAX - ctx->graph_slots ? SIZE_MAX : objects + ctx->graph_slots;
        size_t budget = work > SIZE_MAX / 4 ? SIZE_MAX : work * 4;
        ctx->collection_interval = budget < 4096 ? 4096 : budget;
    }
    ctx->collecting = false;
#endif
}
void rc_safepoint(RcContext *ctx) {
    if (ctx && !ctx->acyclic && ctx->collection_interval && ctx->since_collection >= ctx->collection_interval) rc_collect(ctx);
}
void rc_set_collection_interval(RcContext *ctx, size_t n) {
    if (ctx) { ctx->collection_interval = n; ctx->adaptive_collection = false; }
}
RcStats rc_stats(const RcContext *ctx) { RcStats zero = {0}; return ctx ? ctx->stats : zero; }
void rc_context_free(RcContext *ctx) {
    if (!ctx) return;
    while (ctx->compiled_symbols) {
        RcResolvedSymbols *next = ctx->compiled_symbols->next;
        free(ctx->compiled_symbols);
        ctx->compiled_symbols = next;
    }
    ctx->destroying = true;
    free(ctx->symbols);
    RcValue *v = ctx->objects;
    while (v) { RcValue *next = v->next; raw_free(v); v = next; }
    while (ctx->stats.cached_objects) {
        v = ctx->object_cache[--ctx->stats.cached_objects];
        RC_UNPOISON(v);
        free(v);
    }
    free(ctx->error); free(ctx);
}
RcType rc_type(const RcValue *v) { return v ? v->type : RC_NIL; }
size_t rc_refcount(const RcValue *v) { return v ? v->refs : 0; }
RcValue *rc_nil(void) { return &nil_value; }
bool rc_is_nil(const RcValue *v) { return v == &nil_value; }
static bool belongs(RcContext *ctx, const RcValue *v) { return v && (v == &nil_value || v->ctx == ctx); }
RcValue *rc_number(RcContext *ctx, double n) {
    RcValue *v = allocate(ctx, RC_NUMBER, 0);
    if (v) v->as.number.real = n;
    return v;
}
RcValue *rc_integer(RcContext *ctx, int64_t n) {
    RcValue *v = allocate(ctx, RC_NUMBER, 0);
    if (v) { v->as.number.integer = n; v->as.number.real = (double)n; v->as.number.exact = true; }
    return v;
}
double rc_number_value(const RcValue *v) { return v && v->type == RC_NUMBER ? v->as.number.real : NAN; }
bool rc_number_is_integer(const RcValue *v) {
    return v && v->type == RC_NUMBER && (v->as.number.exact || (isfinite(v->as.number.real) && trunc(v->as.number.real) == v->as.number.real));
}
bool rc_number_is_exact_integer(const RcValue *v) {
    return v && v->type == RC_NUMBER && v->as.number.exact;
}
bool rc_number_is_int64(const RcValue *v) {
    if (!rc_number_is_integer(v)) return false;
    return v->as.number.exact || (v->as.number.real >= -9223372036854775808.0 && v->as.number.real < 9223372036854775808.0);
}
int64_t rc_integer_value(const RcValue *v) {
    if (!v || v->type != RC_NUMBER) return 0;
    if (v->as.number.exact) return v->as.number.integer;
    double n = v->as.number.real;
    if (!isfinite(n) || n < -9223372036854775808.0 || n >= 9223372036854775808.0) return 0;
    return (int64_t)n;
}
static RcValue *text_value(RcContext *ctx, RcType type, const char *text, size_t n) {
    if (!text || n == SIZE_MAX) return rc_error(ctx, "invalid text length");
    char *copy = malloc(n + 1);
    if (!copy) return rc_error(ctx, "out of memory");
    memcpy(copy, text, n); copy[n] = '\0';
    RcValue *v = allocate(ctx, type, n + 1);
    if (!v) { free(copy); return NULL; }
    v->as.text.data = copy; v->as.text.length = n;
    return v;
}
RcValue *rc_string_n(RcContext *ctx, const char *s, size_t n) { return text_value(ctx, RC_STRING, s, n); }
RcValue *rc_string(RcContext *ctx, const char *s) { return s ? rc_string_n(ctx, s, strlen(s)) : rc_error(ctx, "NULL string"); }
static uint64_t symbol_hash(const char *name) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        hash ^= *p; hash *= UINT64_C(1099511628211);
    }
    return hash;
}
static bool resize_symbols(RcContext *ctx, size_t capacity) {
    if (capacity > SIZE_MAX / sizeof(*ctx->symbols)) { rc_error(ctx, "symbol table size overflow"); return false; }
    RcValue **buckets = calloc(capacity, sizeof(*buckets));
    if (!buckets) { rc_error(ctx, "out of memory"); return false; }
    for (size_t i = 0; i < ctx->symbol_capacity; ++i) {
        RcValue *v = ctx->symbols[i];
        while (v) {
            RcValue *next = v->as.text.intern_next;
            size_t bucket = (size_t)v->as.text.hash & (capacity - 1);
            v->as.text.intern_next = buckets[bucket]; buckets[bucket] = v;
            v = next;
        }
    }
    free(ctx->symbols); ctx->symbols = buckets; ctx->symbol_capacity = capacity;
    return true;
}
RcValue *rc_symbol(RcContext *ctx, const char *name) {
    if (!ctx || !name) return rc_error(ctx, "invalid symbol");
    uint64_t hash = symbol_hash(name);
    if (ctx->symbol_capacity) {
        size_t bucket = (size_t)hash & (ctx->symbol_capacity - 1);
        for (RcValue *v = ctx->symbols[bucket]; v; v = v->as.text.intern_next)
            if (v->as.text.hash == hash && !strcmp(v->as.text.data, name)) return rc_retain(v);
    }
    if (!ctx->symbol_capacity || ctx->symbol_count >= ctx->symbol_capacity - ctx->symbol_capacity / 4) {
        size_t capacity = ctx->symbol_capacity ? ctx->symbol_capacity * 2 : 64;
        if (capacity < ctx->symbol_capacity) return rc_error(ctx, "symbol table size overflow");
        if (!resize_symbols(ctx, capacity)) return NULL;
    }
    RcValue *v = text_value(ctx, RC_SYMBOL, name, strlen(name));
    if (!v) return NULL;
    size_t bucket = (size_t)hash & (ctx->symbol_capacity - 1);
    v->as.text.hash = hash;
    v->as.text.intern_next = ctx->symbols[bucket];
    ctx->symbols[bucket] = rc_retain(v); ctx->symbol_count++;
    /* Only a new interned identity needs name comparisons. These borrowed
     * pointers live exactly as long as the context's intern-table roots. */
    if (!strcmp(name, "true")) ctx->true_symbol = v;
    else if (!strcmp(name, "false")) ctx->false_symbol = v;
    return v;
}
RcValue *const *rc_symbol_table(RcContext *ctx, const RcSymbolTable *table) {
    if (!ctx || !table) { rc_error(ctx, "invalid compiled symbol table"); return NULL; }
    for (RcResolvedSymbols *entry = ctx->compiled_symbols; entry; entry = entry->next)
        if (entry->table == table) return entry->values;
    if ((table->count && !table->names) ||
        table->count > (SIZE_MAX - sizeof(RcResolvedSymbols)) / sizeof(RcValue *)) {
        rc_error(ctx, "invalid compiled symbol table size"); return NULL;
    }
    RcResolvedSymbols *entry = malloc(sizeof(*entry) + table->count * sizeof(*entry->values));
    if (!entry) { rc_error(ctx, "out of memory"); return NULL; }
    for (size_t i = 0; i < table->count; ++i) {
        RcValue *symbol = rc_symbol(ctx, table->names[i]);
        if (!symbol) { free(entry); return NULL; }
        /* Intern-table roots keep the array's borrowed pointers alive. This
         * cache adds no graph edges or roots for the exact cycle collector. */
        entry->values[i] = symbol;
        rc_release(symbol);
    }
    entry->table = table;
    entry->next = ctx->compiled_symbols;
    ctx->compiled_symbols = entry;
    return entry->values;
}
const char *rc_text(const RcValue *v) { return v && (v->type == RC_STRING || v->type == RC_SYMBOL) ? v->as.text.data : NULL; }
size_t rc_text_length(const RcValue *v) { return v && (v->type == RC_STRING || v->type == RC_SYMBOL) ? v->as.text.length : 0; }
RcValue *rc_true(RcContext *ctx) {
    return ctx && ctx->true_symbol ? rc_retain(ctx->true_symbol) : rc_symbol(ctx, "true");
}
RcValue *rc_false(RcContext *ctx) {
    return ctx && ctx->false_symbol ? rc_retain(ctx->false_symbol) : rc_symbol(ctx, "false");
}
bool rc_is_true(const RcValue *v) { return v && v->type == RC_SYMBOL && v == v->ctx->true_symbol; }
bool rc_is_false(const RcValue *v) { return v && v->type == RC_SYMBOL && v == v->ctx->false_symbol; }
bool rc_is_boolean(const RcValue *v) {
    return v && v->type == RC_SYMBOL && (v == v->ctx->true_symbol || v == v->ctx->false_symbol);
}
RcValue *rc_cons(RcContext *ctx, RcValue *head, RcValue *tail) {
    if (!belongs(ctx, head) || !belongs(ctx, tail)) return rc_error(ctx, "invalid cons value/context");
    RcValue *v = allocate(ctx, RC_PAIR, 0);
    if (v) { v->as.pair.head = rc_retain(head); v->as.pair.tail = rc_retain(tail); }
    return v;
}
RcValue *rc_cons_reuse(RcContext *ctx, RcValue *candidate, RcValue *head, RcValue *tail) {
    if (ctx) ctx->stats.pair_reuse_attempts++;
    if (!belongs(ctx, head) || !belongs(ctx, tail) ||
        (candidate && !belongs(ctx, candidate))) {
        if (ctx) ctx->stats.pair_reuse_misses++;
        rc_release(candidate);
        return rc_error(ctx, "invalid cons reuse value/context");
    }
    if (candidate && candidate->type == RC_PAIR && candidate->refs == 1 &&
        candidate != head && candidate != tail) {
        /* Protect replacement children before releasing any old graph edges.
         * The candidate stays a rooted, valid pair throughout; release never
         * invokes collection or user code. Even borrowed old children are safe. */
        rc_retain(head); rc_retain(tail);
        RcValue *old_head = candidate->as.pair.head, *old_tail = candidate->as.pair.tail;
        candidate->as.pair.head = head; candidate->as.pair.tail = tail;
        rc_release(old_head); rc_release(old_tail);
        ctx->stats.pair_reuses++;
        return candidate;
    }
    if (ctx) ctx->stats.pair_reuse_misses++;
    RcValue *value = rc_cons(ctx, head, tail);
    rc_release(candidate);
    return value;
}
RcValue *rc_pair_take_tail_reuse(RcContext *ctx, RcValue *candidate, RcValue **out) {
    if (out) *out = NULL;
    if (!out || !belongs(ctx, candidate) || candidate->type != RC_PAIR) {
        rc_release(candidate);
        return rc_error(ctx, "tail reuse projection expects a pair and output slot");
    }
    RcValue *tail = candidate->as.pair.tail;
    if (candidate->refs == 1) {
        RcValue *head = candidate->as.pair.head;
        /* Ownership of the old edge moves to the C result, with no decrement.
         * Before releasing any other edge, publish a valid pair containing nil
         * in both slots. The collector can visit it at every later safepoint. */
        candidate->as.pair.head = rc_nil(); candidate->as.pair.tail = rc_nil();
        rc_release(head);
        ctx->stats.pair_tail_transfers++;
    } else {
        rc_retain(tail);
        ctx->stats.pair_tail_shares++;
    }
    *out = candidate;
    return tail;
}
RcValue *rc_head(RcContext *ctx, RcValue *v) { return v && v->type == RC_PAIR ? rc_retain(v->as.pair.head) : rc_error(ctx, "hd expects a nonempty list"); }
RcValue *rc_tail(RcContext *ctx, RcValue *v) { return v && v->type == RC_PAIR ? rc_retain(v->as.pair.tail) : rc_error(ctx, "tl expects a nonempty list"); }
RcValue *rc_head_borrowed(const RcValue *v) { return v && v->type == RC_PAIR ? v->as.pair.head : NULL; }
RcValue *rc_tail_borrowed(const RcValue *v) { return v && v->type == RC_PAIR ? v->as.pair.tail : NULL; }
RcValue *rc_vector(RcContext *ctx, size_t length) {
    if (length > SIZE_MAX / sizeof(RcValue *)) return rc_error(ctx, "vector size overflow");
    RcValue **items = length ? malloc(length * sizeof(*items)) : NULL;
    if (length && !items) return rc_error(ctx, "out of memory");
    for (size_t i = 0; i < length; ++i) items[i] = rc_nil();
    RcValue *v = allocate(ctx, RC_VECTOR, length * sizeof(*items));
    if (!v) { free(items); return NULL; }
    v->as.vector.items = items; v->as.vector.length = length;
    return v;
}
size_t rc_vector_length(const RcValue *v) { return v && v->type == RC_VECTOR ? v->as.vector.length : 0; }
RcValue *rc_vector_get(RcContext *ctx, RcValue *v, size_t i) {
    return v && v->type == RC_VECTOR && i < v->as.vector.length ? rc_retain(v->as.vector.items[i]) : rc_error(ctx, "absvector index out of bounds");
}
typedef struct {
    RcContext *ctx;
    RcValue *target, *work;
    bool found;
} CycleSearch;
static void cycle_search_edge(RcValue *v, void *data) {
    CycleSearch *search = data;
    if (!v || v == &nil_value) return;
    if (v == search->target) { search->found = true; return; }
    if (v->visit_epoch == search->ctx->visit_epoch) return;
    v->visit_epoch = search->ctx->visit_epoch;
    v->work = search->work; search->work = v;
}
static bool would_create_cycle(RcContext *ctx, RcValue *target, RcValue *item) {
    /* An allocation-free graph walk. The next epoch invalidates leftover work
     * marks even on an early hit, and objects remain unchanged throughout. */
    if (++ctx->visit_epoch == 0) {
        for (RcValue *v = ctx->objects; v; v = v->next) v->visit_epoch = 0;
        ctx->visit_epoch = 1;
    }
    CycleSearch search = {ctx, target, NULL, false};
    cycle_search_edge(item, &search);
    while (search.work && !search.found) {
        RcValue *v = search.work; search.work = v->work;
        edges(v, cycle_search_edge, &search);
    }
    return search.found;
}
bool rc_vector_set(RcContext *ctx, RcValue *v, size_t i, RcValue *item) {
    if (!belongs(ctx, v) || v->type != RC_VECTOR || i >= v->as.vector.length || !belongs(ctx, item)) { rc_error(ctx, "invalid absvector assignment"); return false; }
    if (ctx->acyclic && would_create_cycle(ctx, v, item)) {
        rc_error(ctx, "acyclic mode rejects vector assignment that would create a cycle");
        return false;
    }
    RcValue *old = v->as.vector.items[i]; v->as.vector.items[i] = rc_retain(item); rc_release(old); return true;
}
static RcValue *closure_create(RcContext *ctx, size_t arity, RcNative native,
                               RcNativeOwned owned_native, size_t capture_count,
                               RcValue *const *captures) {
    if ((!native && !owned_native) || capture_count > SIZE_MAX / sizeof(RcValue *) || (capture_count && !captures)) return rc_error(ctx, "invalid closure");
    for (size_t i = 0; i < capture_count; ++i) if (!belongs(ctx, captures[i])) return rc_error(ctx, "invalid closure capture/context");
    RcValue **copy = capture_count ? malloc(capture_count * sizeof(*copy)) : NULL;
    if (capture_count && !copy) return rc_error(ctx, "out of memory");
    RcValue *v = allocate(ctx, RC_CLOSURE, capture_count * sizeof(*copy));
    if (!v) { free(copy); return NULL; }
    v->as.closure.native = native; v->as.closure.arity = arity;
    v->as.closure.owned_native = owned_native;
    v->as.closure.captures = copy; v->as.closure.capture_count = capture_count;
    for (size_t i = 0; i < capture_count; ++i) copy[i] = rc_retain(captures[i]);
    return v;
}
RcValue *rc_closure(RcContext *ctx, size_t arity, RcNative native,
                    size_t capture_count, RcValue *const *captures) {
    return closure_create(ctx, arity, native, NULL, capture_count, captures);
}
RcValue *rc_closure_owned(RcContext *ctx, size_t arity, RcNativeOwned native,
                          size_t capture_count, RcValue *const *captures) {
    return closure_create(ctx, arity, NULL, native, capture_count, captures);
}
bool rc_closure_is_native(const RcValue *v, RcNative native) {
    return v && v->type == RC_CLOSURE && v->as.closure.native == native &&
           !v->as.closure.owned_native;
}
RcValue *rc_capture(const RcValue *v, size_t i) {
    return v && v->type == RC_CLOSURE && i < v->as.closure.capture_count ? v->as.closure.captures[i] : NULL;
}
size_t rc_arity(const RcValue *v) {
    return v && v->type == RC_CLOSURE ? v->as.closure.arity - v->as.closure.bound_count : 0;
}
static RcValue *partial(RcContext *ctx, RcValue *fn, size_t argc, RcValue *const *argv) {
    size_t n = fn->as.closure.bound_count + argc;
    if (n > SIZE_MAX / sizeof(RcValue *)) return rc_error(ctx, "partial application size overflow");
    RcValue **bound = n ? malloc(n * sizeof(*bound)) : NULL;
    if (n && !bound) return rc_error(ctx, "out of memory");
    RcValue *v = closure_create(ctx, fn->as.closure.arity, fn->as.closure.native,
                                fn->as.closure.owned_native,
                                fn->as.closure.capture_count, fn->as.closure.captures);
    if (!v) { free(bound); return NULL; }
    for (size_t i = 0; i < fn->as.closure.bound_count; ++i) bound[i] = rc_retain(fn->as.closure.bound[i]);
    for (size_t i = 0; i < argc; ++i) bound[fn->as.closure.bound_count + i] = rc_retain(argv[i]);
    v->as.closure.bound = bound; v->as.closure.bound_count = n;
    v->bytes += n * sizeof(*bound); ctx->stats.live_bytes += n * sizeof(*bound);
    ctx->graph_slots += n;
    return v;
}
RcValue *rc_tailcall(RcContext *ctx, RcValue *fn, size_t argc, RcValue *const *argv) {
    if (!belongs(ctx, fn) || argc > SIZE_MAX / sizeof(RcValue *) || (argc && !argv)) return rc_error(ctx, "invalid application");
    for (size_t i = 0; i < argc; ++i) if (!belongs(ctx, argv[i])) return rc_error(ctx, "invalid argument/context");
    RcValue **copy = argc > 3 ? malloc(argc * sizeof(*copy)) : NULL;
    if (argc > 3 && !copy) return rc_error(ctx, "out of memory");
    RcValue *v = allocate(ctx, RC_TAIL, argc > 3 ? argc * sizeof(*copy) : 0);
    if (!v) { free(copy); return NULL; }
    if (argc && argc <= 3) copy = v->as.tail.inline_args;
    v->as.tail.function = rc_retain(fn); v->as.tail.argc = argc; v->as.tail.argv = copy;
    ctx->graph_slots += argc;
    for (size_t i = 0; i < argc; ++i) copy[i] = rc_retain(argv[i]);
    return v;
}
static void consume_arguments(size_t argc, RcValue **argv) {
    if (!argv) return;
    for (size_t i = 0; i < argc; ++i) {
        RcValue *v = argv[i]; argv[i] = NULL; rc_release(v);
    }
}
RcValue *rc_tailcall_owned_args(RcContext *ctx, RcValue *fn, size_t argc, RcValue **argv) {
    if (!belongs(ctx, fn) || argc > SIZE_MAX / sizeof(RcValue *) || (argc && !argv)) {
        consume_arguments(argc, argv);
        return rc_error(ctx, "invalid owned application");
    }
    for (size_t i = 0; i < argc; ++i) if (!belongs(ctx, argv[i])) {
        consume_arguments(argc, argv);
        return rc_error(ctx, "invalid owned argument/context");
    }
    RcValue **copy = argc > 3 ? malloc(argc * sizeof(*copy)) : NULL;
    if (argc > 3 && !copy) {
        consume_arguments(argc, argv);
        return rc_error(ctx, "out of memory");
    }
    RcValue *v = allocate(ctx, RC_TAIL, argc > 3 ? argc * sizeof(*copy) : 0);
    if (!v) { free(copy); consume_arguments(argc, argv); return NULL; }
    if (argc && argc <= 3) copy = v->as.tail.inline_args;
    v->as.tail.function = rc_retain(fn); v->as.tail.argc = argc; v->as.tail.argv = copy;
    ctx->graph_slots += argc;
    for (size_t i = 0; i < argc; ++i) { copy[i] = argv[i]; argv[i] = NULL; }
    return v;
}
RcValue *rc_require_boolean(RcContext *ctx, RcValue *value) {
    if (!value) {
        if (!rc_has_error(ctx)) rc_error(ctx, "missing boolean result");
        return NULL;
    }
    if (belongs(ctx, value) && value->type == RC_TAIL) {
        if (value->refs > 1 && !value->as.tail.require_boolean) {
            RcValue *copy = rc_tailcall(ctx, value->as.tail.function,
                                        value->as.tail.argc, value->as.tail.argv);
            rc_release(value);
            if (!copy) return NULL;
            value = copy;
        }
        value->as.tail.require_boolean = true;
        return value;
    }
    if (belongs(ctx, value) && rc_is_boolean(value)) return value;
    rc_release(value);
    return rc_error(ctx, "boolean expected");
}
/* args == NULL is a boolean continuation. Adjacent identical guards collapse,
 * so arbitrarily long chains of tail and/or calls use one pending guard. */
typedef struct ApplyPending { RcValue *args; struct ApplyPending *next; } ApplyPending;
static RcValue *apply_request(RcContext *ctx, RcValue *request) {
    RcValue *result = NULL;
    ApplyPending *pending = NULL;
    while (request) {
        if (request->as.tail.require_boolean && (!pending || pending->args)) {
            ApplyPending *p = malloc(sizeof(*p));
            if (!p) { rc_error(ctx, "out of memory"); goto fail; }
            p->args = NULL; p->next = pending; pending = p;
        }
        RcValue *target = request->as.tail.function;
        RcValue *resolved = NULL;
        if (target->type == RC_SYMBOL) {
            resolved = rc_function_get(ctx, target);
            if (!resolved) goto fail;
            target = resolved;
        }
        if (target->type != RC_CLOSURE) { rc_error(ctx, "attempt to apply a non-function"); rc_release(resolved); goto fail; }
        size_t available = request->as.tail.argc;
        size_t needed = rc_arity(target);
        if (available < needed) {
            result = partial(ctx, target, available, request->as.tail.argv);
        } else {
            if (available > needed) {
                /* The excess arguments wait until all tail bounces complete. */
                ApplyPending *p = malloc(sizeof(*p));
                if (!p) { rc_error(ctx, "out of memory"); rc_release(resolved); goto fail; }
                p->args = rc_tailcall(ctx, rc_nil(), available - needed, request->as.tail.argv + needed);
                if (!p->args) { free(p); rc_release(resolved); goto fail; }
                p->next = pending; pending = p;
            }
            size_t n = target->as.closure.arity;
            if (n > SIZE_MAX / sizeof(RcValue *)) { rc_error(ctx, "call size overflow"); rc_release(resolved); goto fail; }
            RcValue *small_actual[8];
            RcValue **actual = n <= 8 ? small_actual : malloc(n * sizeof(*actual));
            if (!actual) { rc_error(ctx, "out of memory"); rc_release(resolved); goto fail; }
            size_t bound = target->as.closure.bound_count;
            if (target->as.closure.owned_native) {
                /* Bound arguments remain closure-owned and are shared here.
                 * Unbound request edges become owned C slots without changing
                 * counts. Nil replaces transferred graph slots before callback
                 * entry, so any callback safepoint sees a valid heap and roots. */
                for (size_t i = 0; i < bound; ++i) actual[i] = rc_retain(target->as.closure.bound[i]);
                for (size_t i = 0; i < needed; ++i) {
                    if (request->refs == 1) {
                        actual[bound + i] = request->as.tail.argv[i];
                        request->as.tail.argv[i] = rc_nil();
                    } else {
                        /* Tail values may themselves be captured/shared.
                         * Executing one must not consume its reusable args. */
                        actual[bound + i] = rc_retain(request->as.tail.argv[i]);
                    }
                }
                result = target->as.closure.owned_native(ctx, target, n, actual);
                consume_arguments(n, actual);
            } else {
                for (size_t i = 0; i < bound; ++i) actual[i] = target->as.closure.bound[i];
                for (size_t i = 0; i < needed; ++i) actual[bound + i] = request->as.tail.argv[i];
                result = target->as.closure.native(ctx, target, n, actual);
            }
            if (actual != small_actual) free(actual);
        }
        rc_release(resolved);
        rc_release(request); request = NULL;
        if (!result) goto fail;
        if (rc_has_error(ctx)) { rc_release(result); result = NULL; goto fail; }
        rc_safepoint(ctx);
        if (result->type == RC_TAIL) { request = result; result = NULL; continue; }
        while (pending && !pending->args) {
            ApplyPending *p = pending; pending = p->next; free(p);
            result = rc_require_boolean(ctx, result);
            if (!result) goto fail;
        }
        if (!pending) return result;
        ApplyPending *p = pending; pending = p->next;
        request = p->args;
        /* Replace the dummy function edge with this owned result. */
        request->as.tail.function = result; result = NULL;
        free(p);
    }
fail:
    rc_release(request); rc_release(result);
    while (pending) { ApplyPending *next = pending->next; rc_release(pending->args); free(pending); pending = next; }
    return NULL;
}
/* Small exact calls need no managed request or heap argument arrays. The
 * callee is rooted before entry: callbacks may replace its symbol binding and
 * collect. Owned caller slots are cleared before any callback can inspect them.
 * Partial/overapplication and larger calls retain the general request path. */
static RcValue *apply_small(RcContext *ctx, RcValue *fn, size_t argc,
                            RcValue *const *argv, RcValue **owned, bool *handled) {
    *handled = false;
    if (argc > 8 || !belongs(ctx, fn) || (argc && !argv)) return NULL;
    for (size_t i = 0; i < argc; ++i) if (!belongs(ctx, argv[i])) return NULL;
    RcValue *target = fn;
    if (target->type == RC_SYMBOL) target = target->as.text.function;
    if (!target || target->type != RC_CLOSURE || target->as.closure.bound_count ||
        argc != target->as.closure.arity) return NULL;
    *handled = true;
    rc_retain(target);
    RcValue *actual[8];
    RcValue *result;
    for (size_t i = 0; i < argc; ++i) {
        actual[i] = owned ? owned[i] : rc_retain(argv[i]);
        if (owned) owned[i] = NULL;
    }
    if (target->as.closure.owned_native)
        result = target->as.closure.owned_native(ctx, target, argc, actual);
    else result = target->as.closure.native(ctx, target, argc, actual);
    consume_arguments(argc, actual);
    rc_release(target);
    if (!result) return NULL;
    if (rc_has_error(ctx)) { rc_release(result); return NULL; }
    rc_safepoint(ctx);
    return result->type == RC_TAIL ? apply_request(ctx, result) : result;
}
RcValue *rc_apply(RcContext *ctx, RcValue *fn, size_t argc, RcValue *const *argv) {
    bool handled;
    RcValue *result = apply_small(ctx, fn, argc, argv, NULL, &handled);
    return handled ? result : apply_request(ctx, rc_tailcall(ctx, fn, argc, argv));
}
RcValue *rc_apply_owned_args(RcContext *ctx, RcValue *fn, size_t argc, RcValue **argv) {
    bool handled;
    RcValue *result = apply_small(ctx, fn, argc, argv, argv, &handled);
    return handled ? result : apply_request(ctx, rc_tailcall_owned_args(ctx, fn, argc, argv));
}
RcValue *rc_stream_with_direction(RcContext *ctx, FILE *file, bool close, bool input) {
    if (!file) return rc_error(ctx, "invalid stream");
    RcValue *v = allocate(ctx, RC_STREAM, 0);
    if (v) { v->as.stream.file = file; v->as.stream.close = close; v->as.stream.input = input; }
    return v;
}
RcValue *rc_stream(RcContext *ctx, FILE *file, bool close) { return rc_stream_with_direction(ctx, file, close, file == stdin); }
FILE *rc_stream_file(const RcValue *v) { return v && v->type == RC_STREAM ? v->as.stream.file : NULL; }
bool rc_stream_is_input(const RcValue *v) { return v && v->type == RC_STREAM && v->as.stream.input; }
bool rc_stream_close(RcContext *ctx, RcValue *v) {
    if (!belongs(ctx, v) || v->type != RC_STREAM || !v->as.stream.file) { rc_error(ctx, "close expects an open stream"); return false; }
    FILE *file = v->as.stream.file;
    v->as.stream.file = NULL;
    if (v->as.stream.close && fclose(file)) { rc_error(ctx, "stream close failed"); return false; }
    return true;
}
static bool set_symbol_slot(RcContext *ctx, RcValue *symbol, RcValue *value, bool function) {
    if (!belongs(ctx, symbol) || symbol->type != RC_SYMBOL || !belongs(ctx, value)) { rc_error(ctx, "invalid binding/context"); return false; }
    /* These are context roots, not graph edges: the context owns their counts.
     * Interned symbols survive until context teardown, so slots stay addressable. */
    RcValue **slot = function ? &symbol->as.text.function : &symbol->as.text.global;
    RcValue *old = *slot; *slot = rc_retain(value); rc_release(old); return true;
}
RcValue *rc_global_get(RcContext *ctx, RcValue *symbol) {
    if (!belongs(ctx, symbol) || symbol->type != RC_SYMBOL) return rc_error(ctx, "value expects a symbol");
    return symbol->as.text.global ? rc_retain(symbol->as.text.global) : rc_error(ctx, "variable %s has no value", rc_text(symbol));
}
bool rc_global_set(RcContext *ctx, RcValue *symbol, RcValue *value) { return ctx && set_symbol_slot(ctx, symbol, value, false); }
RcValue *rc_function_lookup(RcContext *ctx, RcValue *symbol) {
    if (!belongs(ctx, symbol) || symbol->type != RC_SYMBOL) return NULL;
    return rc_retain(symbol->as.text.function);
}
RcValue *rc_function_get(RcContext *ctx, RcValue *symbol) {
    RcValue *v = rc_function_lookup(ctx, symbol);
    return v ? v : rc_error(ctx, "undefined function %s", rc_text(symbol) ? rc_text(symbol) : "<non-symbol>");
}
bool rc_function_set(RcContext *ctx, RcValue *symbol, RcValue *fn) {
    if (!fn || fn->type != RC_CLOSURE) { rc_error(ctx, "function binding expects a closure"); return false; }
    return ctx && set_symbol_slot(ctx, symbol, fn, true);
}

typedef struct EqualPair { const RcValue *left, *right; } EqualPair;
static bool scalar_equal(const RcValue *a, const RcValue *b) {
    if (a == b) return true;
    if (!a || !b || a->type != b->type) return false;
    if (a->type == RC_NUMBER) {
        if (a->as.number.exact && b->as.number.exact) return a->as.number.integer == b->as.number.integer;
        if (a->as.number.exact || b->as.number.exact) {
            const RcValue *i = a->as.number.exact ? a : b, *d = a->as.number.exact ? b : a;
            double n = d->as.number.real;
            return isfinite(n) && n >= -9223372036854775808.0 && n < 9223372036854775808.0 && trunc(n) == n && (int64_t)n == i->as.number.integer;
        }
        return a->as.number.real == b->as.number.real;
    }
    if (a->type == RC_STRING || a->type == RC_SYMBOL)
        return a->as.text.length == b->as.text.length && !memcmp(a->as.text.data, b->as.text.data, a->as.text.length);
    return false;
}
bool rc_equal(const RcValue *left, const RcValue *right) {
    if (left == right || !left || !right || left->type != right->type ||
        (left->type != RC_PAIR && left->type != RC_VECTOR)) return scalar_equal(left, right);
    RcContext *ctx = left->ctx;
    size_t count = 0, capacity = 32, cursor = 0;
    EqualPair *pairs = malloc(capacity * sizeof(*pairs));
    if (!pairs) { rc_error(ctx, "out of memory comparing values"); return false; }
    pairs[count++] = (EqualPair){left, right};
    bool equal = true;
    while (cursor < count && equal) {
        EqualPair p = pairs[cursor++];
        const RcValue *a = p.left, *b = p.right;
        if (a == b) continue;
        if (!a || !b || a->type != b->type) { equal = false; break; }
        if (a->type != RC_PAIR && a->type != RC_VECTOR) { equal = scalar_equal(a, b); continue; }
        size_t n = a->type == RC_PAIR ? 2 : a->type == RC_VECTOR ? a->as.vector.length : 0;
        if ((a->type != RC_PAIR && a->type != RC_VECTOR) || (a->type == RC_VECTOR && n != b->as.vector.length)) { equal = false; break; }
        for (size_t i = 0; i < n && equal; ++i) {
            EqualPair child;
            if (a->type == RC_PAIR) child = i ? (EqualPair){a->as.pair.tail, b->as.pair.tail} : (EqualPair){a->as.pair.head, b->as.pair.head};
            else child = (EqualPair){a->as.vector.items[i], b->as.vector.items[i]};
            bool seen = false;
            for (size_t k = 0; k < count; ++k) if (pairs[k].left == child.left && pairs[k].right == child.right) { seen = true; break; }
            if (seen) continue;
            if (count == capacity) {
                if (capacity > SIZE_MAX / 2 / sizeof(*pairs)) { rc_error(ctx, "comparison size overflow"); equal = false; break; }
                size_t next = capacity * 2;
                EqualPair *grown = realloc(pairs, next * sizeof(*pairs));
                if (!grown) { rc_error(ctx, "out of memory comparing values"); equal = false; break; }
                pairs = grown; capacity = next;
            }
            pairs[count++] = child;
        }
    }
    free(pairs); return equal;
}
