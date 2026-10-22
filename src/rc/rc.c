#include "rc.h"
#include "rc_internal.h"
#include "native_compile.h"
#include "rc_layout.h"

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
# define RC_POISON(p, n) __asan_poison_memory_region((p), (n))
# define RC_UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
# define RC_POISON(p, n) ((void)(p), (void)(n))
# define RC_UNPOISON(p, n) ((void)(p), (void)(n))
#endif

/* Per-operation retain/release counters cost a context load and a write on
 * every count change. Release builds omit them; diagnostic builds define
 * SHEN_RC_STATS_COUNTERS=1. All other statistics are always maintained. */
#ifndef SHEN_RC_STATS_COUNTERS
#define SHEN_RC_STATS_COUNTERS 0
#endif

/* Normalized completion guards: boolean already validates context. Context
 * followed by boolean keeps context's earlier diagnostic; a later context
 * check after boolean is redundant. */
enum { GUARD_BOOLEAN=1, GUARD_CONTEXT=2 };
static unsigned compose_guards(unsigned first,unsigned after) {
    return !first ? after : first==GUARD_CONTEXT ? first|after : first;
}
typedef struct RcResolvedSymbols {
    const RcSymbolTable *table;
    struct RcResolvedSymbols *next;
    RcValue *values[];
} RcResolvedSymbols;
_Static_assert(RC_BUILTIN_COUNT < 31, "builtin tags must fit in five bits");
/* The count, type, builtin tag and flags share the first header word. */
_Static_assert(offsetof(RcValue, trial) == 8,
               "count, type and flags must fit one 8-byte header word");
struct RcContext {
    RcContextHot hot; /* First: generated fast paths read it. */
    void *native_compile_state;
    size_t interpreter_entries;
    bool verify_native, interpreter_fallback;
    int execution_mode;
    /* Slab pages, newest first, and one free list and bump page per class. */
    struct RcPage *pages;
    struct RcPage *bump[2];
    RcValue *free_cells[2];
    size_t free_bytes;
    /* Evaluator side table: pair -> (eval plan, call count), linear probing. */
    struct RcPlanEntry *plans; size_t plan_capacity, plan_count;
    /* Scratch list of visited values for bounded graph walks. */
    RcValue **walk_nodes; size_t walk_capacity;
    bool destroying;
    RcValue **symbols;
    size_t symbol_capacity, symbol_count;
    RcResolvedSymbols *compiled_symbols;
    /* Direct-mapped lookup over compiled_symbols; entries are borrowed. With
     * runtime-loaded modules the list grows, and every native call reads it. */
    RcResolvedSymbols *compiled_symbol_cache[64];
    RcValue *true_symbol, *false_symbol; /* Borrowed intern-table roots. */
    /* Open-addressed set of shared immortal closures (rc_closure_static).
     * Teardown frees them with every other cell in the slab pages. */
    RcValue **static_closures;
    size_t static_capacity, static_count;
    /* Owned context roots, bounded by the specialized builtin set. */
    RcValue *builtin_roots[RC_BUILTIN_COUNT];
    /* One bounded dependency certificate. Keys borrow permanent intern roots;
     * no managed values or extra collector edges are retained here. */
    size_t function_generation,prolog_guard_generation;
    RcValue *prolog_guard_keys[3];
    unsigned prolog_guard_mask;
    /* Native hash: function generation + 1 at which its kernel dependencies
     * were last found unchanged (0: never). */
    size_t native_hash_generation;
    RcStats stats;
#if SHEN_RC_STATS_COUNTERS
    /* Reuse-headroom diagnostics (SHEN_RC_PERCEUS_STATS); see
     * docs/performance/tight-perceus-log.md. Counter builds only. */
    struct {
        size_t alloc_by_type[RC_ENV + 1], death_root[RC_ENV + 1], death_cascade[RC_ENV + 1];
        size_t release_to_zero, pair_alloc_after_pair_free, cycle_source_marks;
        size_t full_collections, collect_scanned, collect_sources, collect_source_reach;
        size_t collect_live_vector_reachers;
        bool pair_freed_since_alloc;
    } perceus;
#endif
    size_t graph_slots, cycle_sources;
    /* Live cycle-source vectors, linked through their payloads; the cycle
     * collector starts only from these. gc_nodes is its reusable scratch. */
    RcValue *sources;
    RcValue **gc_nodes; size_t gc_capacity;
    char *error;
    uintptr_t stack_limit;
    bool collecting, adaptive_collection;
    /* Optional evaluator observer; NULL unless a host attaches a tier. */
    const RcTierHooks *tier_hooks; void *tier_state;
};
static RcValue nil_value = {.type = RC_NIL, .refs = RC_IMMORTAL};
_Static_assert(offsetof(struct RcContext, hot) == 0, "hot context fields come first");
_Static_assert(RC_FAST_HD == RC_BUILTIN_HD && RC_FAST_TL == RC_BUILTIN_TL && RC_FAST_CONSP == RC_BUILTIN_CONSP &&
               RC_FAST_EQ == RC_BUILTIN_EQ && RC_FAST_CONS == RC_BUILTIN_CONS, "fast builtin slots match RcBuiltinSlot");
struct RcPlanEntry { RcValue *key, *plan; size_t calls; };
static RcContext *ctx_of(const RcValue *v) { return rc_value_context(v); }

#if SHEN_RC_IMMEDIATE_INTEGERS
_Static_assert(_Alignof(RcValue) >= 2, "heap values must have a clear low bit");
#endif
/* These opaque handles are never dereferenced. Unsigned shifts preserve all
 * payload bits without signed overflow, including the negative boundary. */
static bool is_immediate(const RcValue *v) {
#if SHEN_RC_IMMEDIATE_INTEGERS
    return ((uintptr_t)v & UINT64_C(1)) != 0;
#else
    (void)v;
    return false;
#endif
}
static int64_t immediate_integer(const RcValue *v) {
    uint64_t payload = (uint64_t)(uintptr_t)v >> 1;
    return (int64_t)(payload & UINT64_C(0x3fffffffffffffff)) -
           (int64_t)(payload & UINT64_C(0x4000000000000000));
}
static bool is_heap(const RcValue *v) {
    return v && v != &nil_value && !is_immediate(v);
}
/* Nil carries the immortal sentinel, so one test also covers it. */
static bool is_counted(const RcValue *v) {
    return v && !is_immediate(v) && v->refs != RC_IMMORTAL;
}
static bool may_reach_vector(const RcValue *v) {
    return is_heap(v) && v->reaches_vector;
}


RcValue *rc_error(RcContext *ctx, const char *format, ...) {
    if (!ctx) return NULL;
    ctx->hot.error_active = true;
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
bool rc_has_error(const RcContext *ctx) { return ctx && ctx->hot.error_active; }
void rc_set_stack_limit(RcContext *ctx, const void *limit) { if (ctx) ctx->stack_limit = (uintptr_t)limit; }
bool rc_stack_check(RcContext *ctx) {
    if(!ctx->stack_limit)return true;
#if defined(__GNUC__) || defined(__clang__)
    /* ASan may place local objects on its fake stack. Inspect the real frame. */
    uintptr_t address=(uintptr_t)__builtin_frame_address(0);
#else
    volatile char probe=0;uintptr_t address=(uintptr_t)&probe;
#endif
    if(address>=ctx->stack_limit)return true;
    rc_error(ctx, "stack exhausted: recursion is too deep");
    return false;
}
const char *rc_error_message(const RcContext *ctx) {
    return ctx && ctx->error ? ctx->error : ctx && ctx->hot.error_active ? "out of memory" : "";
}
void rc_clear_error(RcContext *ctx) { if (ctx) { free(ctx->error); ctx->error = NULL; ctx->hot.error_active = false; } }

static size_t cell_bytes(RcType type) { return type == RC_PAIR ? RC_PAIR_CELL : sizeof(RcValue); }
static RcValue *new_cell(RcContext *ctx, int cls, size_t bytes) {
    RcPage *page = ctx->bump[cls];
    if (!page || page->used + bytes > page->limit) {
        page = aligned_alloc(RC_PAGE_BYTES, RC_PAGE_BYTES);
        if (!page) return NULL;
        page->ctx = ctx; page->cell_bytes = bytes;
        page->used = (sizeof(RcPage) + 63) & ~(size_t)63;
        page->limit = RC_PAGE_BYTES - (RC_PAGE_BYTES - page->used) % bytes;
        page->next = ctx->pages; ctx->pages = page; ctx->bump[cls] = page;
        ctx->stats.system_allocations++;
        RC_POISON((char *)page + page->used, RC_PAGE_BYTES - page->used);
    }
    RcValue *v = (RcValue *)((char *)page + page->used);
    page->used += bytes;
    RC_UNPOISON(v, bytes);
    return v;
}
static RcValue *allocate(RcContext *ctx, RcType type, size_t extra) {
    if (!ctx || extra > SIZE_MAX - sizeof(RcValue)) return rc_error(ctx, "allocation size overflow");
    int cls = type == RC_PAIR ? 0 : 1;
    size_t bytes = cell_bytes(type);
    RcValue *v = ctx->free_cells[cls];
    if (v) {
        RC_UNPOISON(v, bytes);
        ctx->free_cells[cls] = v->work;
        ctx->stats.cached_objects--; ctx->free_bytes -= bytes;
    } else {
        v = new_cell(ctx, cls, bytes);
        if (!v) return rc_error(ctx, "out of memory");
    }
    /* Constructors initialize every field they read. Collector trial values
     * are reset before a collection, and work links are set before insertion.
     * Reset the lifetime-wide defaults that must not survive cell reuse. */
    v->marked = false; v->reaches_vector = false; v->builtin_slot = 0; v->returns_second = false;
    v->visited = false; v->has_plan = false;
    v->type = type; v->refs = 1;
    ctx->stats.live_objects++;
    ctx->stats.live_bytes += bytes + extra;
    ctx->stats.total_allocations++;
    if (type == RC_PAIR) ctx->stats.pair_allocations++;
#if SHEN_RC_STATS_COUNTERS
    ctx->perceus.alloc_by_type[type]++;
    if (type == RC_PAIR) {
        ctx->perceus.pair_alloc_after_pair_free += ctx->perceus.pair_freed_since_alloc;
        ctx->perceus.pair_freed_since_alloc = false;
    }
#endif
    if (type == RC_CLOSURE) {
        size_t captures = extra / sizeof(RcValue *);
        ctx->stats.capture_edges_created += captures;
        ctx->stats.live_capture_edges += captures;
        if (ctx->stats.live_capture_edges > ctx->stats.peak_capture_edges)
            ctx->stats.peak_capture_edges = ctx->stats.live_capture_edges;
    }
    ctx->hot.since_collection++;
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
        ctx->hot.collection_interval = 4096; ctx->adaptive_collection = true;
        const char *strict=getenv("SHEN_RC_VERIFY_NATIVE");ctx->verify_native=strict&&strcmp(strict,"0");
        const char *unsealed=getenv("SHEN_RC_UNSEALED");ctx->hot.lean_unsealed=unsealed&&*unsealed&&strcmp(unsealed,"0");
        const char *backend=getenv("SHEN_RC_EXECUTION");
        ctx->execution_mode=!backend||!strcmp(backend,"native")?0:!strcmp(backend,"interpret")?1:-1;
#if defined(SHEN_RC_ACYCLIC_ONLY) && SHEN_RC_ACYCLIC_ONLY
        ctx->hot.acyclic = true;
#else
        ctx->hot.acyclic = policy == RC_ACYCLIC_ONLY;
#endif
    }
    return ctx;
}
bool rc_context_is_acyclic(const RcContext *ctx) { return ctx && ctx->hot.acyclic; }
RcValue *rc_retain(RcValue *v) {
    if (is_counted(v)) {
#if SHEN_RC_STATS_COUNTERS
        ctx_of(v)->stats.retain_calls++;
#endif
        v->refs++;
    }
    return v;
}
typedef void (*EdgeFn)(RcValue *, void *);
static bool walk_push(RcContext *ctx, size_t *n, RcValue *v) {
    if (*n == ctx->walk_capacity) {
        size_t capacity = ctx->walk_capacity ? ctx->walk_capacity * 2 : 256;
        if (capacity > SIZE_MAX / sizeof(RcValue *)) return false;
        RcValue **grown = realloc(ctx->walk_nodes, capacity * sizeof(*grown));
        if (!grown) return false;
        ctx->walk_nodes = grown; ctx->walk_capacity = capacity;
    }
    ctx->walk_nodes[(*n)++] = v;
    return true;
}
/* Evaluator plans hang off pairs in a side table, so pair cells stay small.
 * Only the diagnostic interpreter creates entries. */
static size_t plan_hash(const RcValue *key, size_t capacity) {
    uint64_t h = (uint64_t)(uintptr_t)key * UINT64_C(0x9e3779b97f4a7c15);
    return (size_t)(h >> 17) & (capacity - 1);
}
static struct RcPlanEntry *plan_entry(const RcValue *pair) {
    RcContext *ctx = ctx_of(pair);
    if (!pair->has_plan || !ctx->plan_capacity) return NULL;
    for (size_t i = plan_hash(pair, ctx->plan_capacity);; i = (i + 1) & (ctx->plan_capacity - 1)) {
        if (ctx->plans[i].key == pair) return &ctx->plans[i];
        if (!ctx->plans[i].key) return NULL;
    }
}
static RcValue *plan_of(const RcValue *pair) {
    struct RcPlanEntry *e = plan_entry(pair);
    return e ? e->plan : NULL;
}
static bool plan_grow(RcContext *ctx) {
    size_t capacity = ctx->plan_capacity ? ctx->plan_capacity * 2 : 64;
    struct RcPlanEntry *table = calloc(capacity, sizeof(*table));
    if (!table) return false;
    for (size_t i = 0; i < ctx->plan_capacity; ++i) {
        struct RcPlanEntry *e = &ctx->plans[i];
        if (!e->key) continue;
        size_t j = plan_hash(e->key, capacity);
        while (table[j].key) j = (j + 1) & (capacity - 1);
        table[j] = *e;
    }
    free(ctx->plans); ctx->plans = table; ctx->plan_capacity = capacity;
    return true;
}
/* Returns the pair's entry, creating an empty one; NULL when out of memory. */
static struct RcPlanEntry *plan_insert(RcValue *pair) {
    struct RcPlanEntry *e = plan_entry(pair);
    if (e) return e;
    RcContext *ctx = ctx_of(pair);
    if ((ctx->plan_count + 1) * 2 > ctx->plan_capacity && !plan_grow(ctx)) return NULL;
    size_t i = plan_hash(pair, ctx->plan_capacity);
    while (ctx->plans[i].key) i = (i + 1) & (ctx->plan_capacity - 1);
    ctx->plans[i] = (struct RcPlanEntry){pair, NULL, 0};
    ctx->plan_count++; pair->has_plan = true;
    return &ctx->plans[i];
}
/* Removes the pair's entry and returns its owned plan, if any. */
static RcValue *plan_remove(RcValue *pair) {
    struct RcPlanEntry *e = plan_entry(pair);
    pair->has_plan = false;
    if (!e) return NULL;
    RcContext *ctx = ctx_of(pair);
    RcValue *plan = e->plan;
    size_t mask = ctx->plan_capacity - 1, i = (size_t)(e - ctx->plans);
    /* Backward-shift deletion keeps every probe chain unbroken. */
    for (size_t j = (i + 1) & mask; ctx->plans[j].key; j = (j + 1) & mask) {
        size_t home = plan_hash(ctx->plans[j].key, ctx->plan_capacity);
        if (((j - home) & mask) >= ((j - i) & mask)) { ctx->plans[i] = ctx->plans[j]; i = j; }
    }
    ctx->plans[i] = (struct RcPlanEntry){0};
    ctx->plan_count--;
    if (plan) ctx->graph_slots--;
    return plan;
}
static void edges(RcValue *v, EdgeFn fn, void *data) {
    switch (rc_type(v)) {
    case RC_PAIR: fn(v->as.pair.head, data); fn(v->as.pair.tail, data); if (v->has_plan) { RcValue *plan = plan_of(v); if (plan) fn(plan, data); } break;
    case RC_VECTOR:
        for (size_t i = 0; i < v->as.vector.length; ++i) fn(v->as.vector.items[i], data);
        break;
    case RC_ENV:
        fn(v->as.env.parent,data);
        for (size_t i = 0; i < v->as.env.count; ++i) fn(v->as.env.values[i],data);
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
static void source_link(RcContext *ctx, RcValue *v) {
    v->as.vector.cycle_source = true;
    v->as.vector.source_prev = NULL; v->as.vector.source_next = ctx->sources;
    if (ctx->sources) ctx->sources->as.vector.source_prev = v;
    ctx->sources = v; ctx->cycle_sources++;
}
static void source_unlink(RcContext *ctx, RcValue *v) {
    RcValue *prev = v->as.vector.source_prev, *next = v->as.vector.source_next;
    if (prev) prev->as.vector.source_next = next; else ctx->sources = next;
    if (next) next->as.vector.source_prev = prev;
    v->as.vector.cycle_source = false; ctx->cycle_sources--;
}
/* Payload bytes beyond the cell, as each constructor passed to allocate. */
static size_t extra_bytes(const RcValue *v) {
    switch (v->type) {
    case RC_STRING: case RC_SYMBOL: return v->as.text.length + 1;
    case RC_VECTOR: return v->as.vector.length * sizeof(RcValue *);
    case RC_ENV: return v->as.env.count > 1 ? 2 * v->as.env.count * sizeof(RcValue *) : 0;
    case RC_CLOSURE: return (v->as.closure.capture_count + v->as.closure.bound_count) * sizeof(RcValue *);
    case RC_TAIL: return v->as.tail.argc > 3 ? v->as.tail.argc * sizeof(RcValue *) : 0;
    default: return 0;
    }
}
/* Unaccounts a dying value. Its plan side entry, if any, stays until
 * raw_free, so the dead pair still reports that edge to edges(). */
static void unlink_object(RcValue *v) {
    RcContext *ctx = ctx_of(v);
    ctx->stats.live_objects--;
    ctx->stats.live_bytes -= cell_bytes(v->type) + extra_bytes(v);
    ctx->stats.total_frees++;
    switch (rc_type(v)) {
    case RC_PAIR: ctx->graph_slots -= 2; break;
    case RC_VECTOR:
        ctx->graph_slots -= v->as.vector.length;
        if (v->as.vector.cycle_source) source_unlink(ctx, v);
        break;
    case RC_ENV: ctx->graph_slots -= 1 + v->as.env.count; break;
    case RC_CLOSURE:
        ctx->graph_slots -= v->as.closure.capture_count + v->as.closure.bound_count;
        ctx->stats.live_capture_edges -= v->as.closure.capture_count;
        break;
    case RC_TAIL: ctx->graph_slots -= 1 + v->as.tail.argc; break;
    default: break;
    }
}
static void raw_free(RcValue *v) {
    switch (rc_type(v)) {
    case RC_PAIR:
        /* The plan edge was released with the other edges; drop the entry. */
        if (v->has_plan) (void)plan_remove(v);
        break;
    case RC_STRING: case RC_SYMBOL: free(v->as.text.data); break;
    case RC_VECTOR: free(v->as.vector.items); break;
    case RC_ENV: if (v->as.env.count > 1) free(v->as.env.keys); break;
    case RC_CLOSURE: free(v->as.closure.captures); free(v->as.closure.bound); break;
    case RC_TAIL: if (v->as.tail.argv != v->as.tail.inline_args) free(v->as.tail.argv); break;
    case RC_STREAM: if (v->as.stream.file && v->as.stream.close) fclose(v->as.stream.file); break;
    default: break;
    }
    RcContext *ctx = ctx_of(v);
    if (ctx->destroying) return;
    int cls = v->type == RC_PAIR ? 0 : 1;
    size_t bytes = cell_bytes(v->type);
    v->type = RC_FREE_CELL;
    v->work = ctx->free_cells[cls]; ctx->free_cells[cls] = v;
    ctx->stats.cached_objects++; ctx->free_bytes += bytes;
    if (ctx->free_bytes > ctx->stats.peak_cached_bytes) ctx->stats.peak_cached_bytes = ctx->free_bytes;
    RC_POISON(v, bytes);
}
static void release_edge(RcValue *v, void *data) {
    if (!is_counted(v)) return;
    RcValue **work = data;
    if (!v->refs) abort();
#if SHEN_RC_STATS_COUNTERS
    ctx_of(v)->stats.release_calls++;
#endif
    if (--v->refs == 0) {
#if SHEN_RC_STATS_COUNTERS
        ctx_of(v)->perceus.release_to_zero++;
#endif
        v->work = *work; *work = v;
    }
}
void rc_release(RcValue *v) {
    RcValue *work = NULL;
    release_edge(v, &work);
#if SHEN_RC_STATS_COUNTERS
    bool root = true;
#endif
    while (work) {
        RcValue *dead = work;
        work = dead->work;
#if SHEN_RC_STATS_COUNTERS
        /* The first death is the owner's own drop: the point where Perceus
         * could reuse the cell. Later ones are its release cascade. */
        if (root) ctx_of(dead)->perceus.death_root[dead->type]++;
        else ctx_of(dead)->perceus.death_cascade[dead->type]++;
        root = false;
        if (dead->type == RC_PAIR) ctx_of(dead)->perceus.pair_freed_since_alloc = true;
#endif
        unlink_object(dead);
        edges(dead, release_edge, &work);
        raw_free(dead);
    }
}
#if SHEN_RC_STATS_COUNTERS && (!defined(SHEN_RC_ACYCLIC_ONLY) || !SHEN_RC_ACYCLIC_ONLY)
/* Diagnostic only: how much of the heap a collector that started from the
 * live cycle sources, and followed only vector-reaching edges, would visit. */
typedef struct { RcContext *ctx; size_t n; bool failed; } SourceReach;
static void source_reach_edge(RcValue *v, void *data) {
    SourceReach *r = data;
    if (!may_reach_vector(v) || v->visited) return;
    v->visited = true;
    if (!walk_push(r->ctx, &r->n, v)) r->failed = true;
}
static void perceus_source_reach(RcContext *ctx) {
    SourceReach r = {ctx, 0, false};
    ctx->perceus.collect_scanned += ctx->stats.live_objects;
    for (RcValue *v = ctx->sources; v; v = v->as.vector.source_next) {
        ctx->perceus.collect_sources++;
        source_reach_edge(v, &r);
    }
    for (size_t i = 0; i < r.n; ++i) edges(ctx->walk_nodes[i], source_reach_edge, &r);
    for (size_t i = 0; i < r.n; ++i) ctx->walk_nodes[i]->visited = false;
    ctx->perceus.collect_source_reach += r.n;
    ctx->perceus.full_collections++;
}
#endif
#if !defined(SHEN_RC_ACYCLIC_ONLY) || !SHEN_RC_ACYCLIC_ONLY
static bool on_cycle(RcContext *ctx, RcValue *source);
static bool gc_push(RcContext *ctx, size_t *n, RcValue *v) {
    if (*n == ctx->gc_capacity) {
        size_t capacity = ctx->gc_capacity ? ctx->gc_capacity * 2 : 1024;
        if (capacity > SIZE_MAX / sizeof(RcValue *)) return false;
        RcValue **grown = realloc(ctx->gc_nodes, capacity * sizeof(*grown));
        if (!grown) return false;
        ctx->gc_nodes = grown; ctx->gc_capacity = capacity;
    }
    ctx->gc_nodes[(*n)++] = v;
    return true;
}
typedef struct { RcContext *ctx; size_t n; bool failed; } GcScan;
/* Only vector-reaching objects can lie on a cycle, and every cycle contains a
 * live source. So a garbage cycle lies inside G: the vector-reaching objects
 * reachable from the sources. Any edge into G from outside it comes from a
 * vector-reaching object that no source reaches, which is live or acyclically
 * rooted; it stays in the trial count as an external reference. */
static void gc_discover_edge(RcValue *v, void *data) {
    GcScan *scan = data;
    if (!may_reach_vector(v) || v->visited) return;
    v->visited = true;
    v->trial = v->refs == RC_IMMORTAL ? SIZE_MAX : v->refs; v->marked = false;
    if (!gc_push(scan->ctx, &scan->n, v)) scan->failed = true;
}
static void gc_subtract_edge(RcValue *v, void *data) {
    (void)data;
    if (!may_reach_vector(v) || !v->visited) return;
    if (!v->trial) abort();
    v->trial--;
}
static void gc_mark_edge(RcValue *v, void *data) {
    RcContext *ctx = ((GcScan *)data)->ctx;
    if (!may_reach_vector(v) || !v->visited || v->marked) return;
    v->marked = true;
    if (!gc_push(ctx, &((GcScan *)data)->n, v)) ((GcScan *)data)->failed = true;
}
/* Dead objects are exactly the unmarked members of G. Every other heap child
 * keeps its edge count, so a dead object releases its edge to it. */
static void gc_release_edge(RcValue *v, void *data) {
    (void)data;
    if (!is_heap(v)) return;
    if (v->visited && !v->marked) return;
    rc_release(v);
}
static void collect_from_sources(RcContext *ctx) {
    GcScan scan = {ctx, 0, false};
    for (RcValue *v = ctx->sources; v && !scan.failed; v = v->as.vector.source_next) gc_discover_edge(v, &scan);
    for (size_t i = 0; i < scan.n && !scan.failed; ++i) edges(ctx->gc_nodes[i], gc_discover_edge, &scan);
    /* Out of scratch memory: collect nothing. Only walk state has changed. */
    if (scan.failed) { for (size_t i = 0; i < scan.n; ++i) ctx->gc_nodes[i]->visited = false; return; }
    size_t n = scan.n;
    for (size_t i = 0; i < n; ++i) edges(ctx->gc_nodes[i], gc_subtract_edge, ctx);
    /* Residual trial counts are the references from outside G: C handles,
     * context roots and live outside objects. Mark from them, using the
     * scratch array past the first n entries as the work stack. */
    GcScan mark = {ctx, n, false};
    for (size_t i = 0; i < n; ++i) {
        RcValue *v = ctx->gc_nodes[i];
        if (v->trial && !v->marked) { v->marked = true; if (!gc_push(ctx, &mark.n, v)) { mark.failed = true; break; } }
        while (mark.n > n && !mark.failed) { RcValue *w = ctx->gc_nodes[--mark.n]; edges(w, gc_mark_edge, &mark); }
    }
    if (mark.failed) {
        /* Treat everything as live; only walk state was written. */
        for (size_t i = 0; i < n; ++i) ctx->gc_nodes[i]->visited = false;
        return;
    }
    /* Count the garbage only a cycle kept alive, including acyclic objects
     * outside G that die when the cycle releases them, as a full-heap trial
     * deletion would. */
    size_t frees = ctx->stats.total_frees;
    RcValue *dead = NULL;
    for (size_t i = 0; i < n; ++i) {
        RcValue *v = ctx->gc_nodes[i];
        if (!v->marked) { unlink_object(v); v->work = dead; dead = v; }
        else v->visited = false; /* Dead cells keep the bit until freed. */
    }
    /* Keep every dead object allocated until all edge classifications are done. */
    for (RcValue *v = dead; v; v = v->work) edges(v, gc_release_edge, ctx);
    while (dead) { RcValue *next = dead->work; raw_free(dead); dead = next; }
    ctx->stats.cycle_objects_freed += ctx->stats.total_frees - frees;
    /* A source that no longer lies on a cycle can drop its mark: the cycle
     * invariant concerns only cycles through it. Writes re-mark it. The
     * check is bounded so a heap with many sources stays linear. */
    size_t checked = 0;
    for (RcValue *v = ctx->sources, *next; v && checked < 64; v = next, ++checked) {
        next = v->as.vector.source_next;
        if (!on_cycle(ctx, v)) source_unlink(ctx, v);
    }
}
#endif
void rc_collect(RcContext *ctx) {
#if defined(SHEN_RC_ACYCLIC_ONLY) && SHEN_RC_ACYCLIC_ONLY
    (void)ctx;
#else
    if (!ctx || ctx->hot.acyclic || ctx->collecting) return;
    ctx->collecting = true;
    ctx->stats.collections++;
    ctx->hot.since_collection = 0;
    /* Only vector writes can introduce cycles. Such writes mark their source
     * until it is freed. A cycle's closing source must still exist while that
     * cycle exists, so zero sources proves the entire heap acyclic. Surviving
     * rooted cycles keep their sources counted across collections. */
    if (!ctx->cycle_sources) {
        ctx->stats.acyclic_collections++;
    } else {
#if SHEN_RC_STATS_COUNTERS
        perceus_source_reach(ctx);
#endif
        collect_from_sources(ctx);
    }
    if (ctx->adaptive_collection) {
        /* Freeze the next budget from survivors, including pointer slots in
         * large vectors. Recomputing from the current growing heap on every
         * safepoint would indefinitely postpone collection of pure cycles. */
        size_t objects = ctx->stats.live_objects;
        size_t work = objects > SIZE_MAX - ctx->graph_slots ? SIZE_MAX : objects + ctx->graph_slots;
        size_t budget = work > SIZE_MAX / 4 ? SIZE_MAX : work * 4;
        ctx->hot.collection_interval = budget < 4096 ? 4096 : budget;
    }
    ctx->collecting = false;
#endif
}
void rc_safepoint(RcContext *ctx) {
    if (ctx && !ctx->hot.acyclic && ctx->hot.collection_interval && ctx->hot.since_collection >= ctx->hot.collection_interval) rc_collect(ctx);
}
void rc_set_collection_interval(RcContext *ctx, size_t n) {
    if (ctx) { ctx->hot.collection_interval = n; ctx->adaptive_collection = false; }
}
bool rc_stats_counts_ownership(void) { return SHEN_RC_STATS_COUNTERS != 0; }
RcStats rc_stats(const RcContext *ctx) {
    RcStats result = {0};
    if (ctx) {
        result = ctx->stats;
        result.cached_bytes = ctx->free_bytes;
    }
    return result;
}
int rc_execution_mode(const RcContext *ctx){return ctx->execution_mode;}
void **rc_native_compile_slot(RcContext *ctx){return &ctx->native_compile_state;}
size_t rc_interpreter_entries(const RcContext *ctx){return ctx->interpreter_entries;}
bool rc_interpreter_enter(RcContext *ctx){
 ctx->interpreter_entries++;
 if(ctx->verify_native||(ctx->execution_mode!=1&&!ctx->interpreter_fallback)){rc_error(ctx,"strict native execution entered interpreter");return false;}return true;
}
bool rc_interpreter_allow_fallback(RcContext *ctx){
 if(ctx->verify_native)return false;ctx->interpreter_fallback=true;return true;
}
#if SHEN_RC_STATS_COUNTERS
static void perceus_report(const RcContext *ctx) {
    if (!getenv("SHEN_RC_PERCEUS_STATS")) return;
    static const char *const names[] = {"nil","number","string","symbol","pair","vector","closure","stream","tail","env"};
    const RcStats *s = &ctx->stats;
    fprintf(stderr, "RC_PERCEUS {\"total_allocations\":%zu,\"pair_allocations\":%zu,\"pair_reuse_attempts\":%zu,\"pair_reuses\":%zu,\"pair_tail_transfers\":%zu,"
            "\"retain_calls\":%zu,\"release_calls\":%zu,\"release_to_zero\":%zu,\"pair_alloc_after_pair_free\":%zu,"
            "\"collections\":%zu,\"acyclic_collections\":%zu,\"full_collections\":%zu,\"collect_scanned\":%zu,\"collect_live_vector_reachers\":%zu,"
            "\"collect_sources\":%zu,\"collect_source_reach\":%zu,\"cycle_source_marks\":%zu,\"cycle_objects_freed\":%zu,\"peak_objects\":%zu",
            s->total_allocations, s->pair_allocations, s->pair_reuse_attempts, s->pair_reuses, s->pair_tail_transfers,
            s->retain_calls, s->release_calls, ctx->perceus.release_to_zero, ctx->perceus.pair_alloc_after_pair_free,
            s->collections, s->acyclic_collections, ctx->perceus.full_collections, ctx->perceus.collect_scanned, ctx->perceus.collect_live_vector_reachers,
            ctx->perceus.collect_sources, ctx->perceus.collect_source_reach, ctx->perceus.cycle_source_marks, s->cycle_objects_freed, s->peak_objects);
    for (int t = 0; t <= RC_ENV; t++)
        fprintf(stderr, ",\"%s\":[%zu,%zu,%zu]", names[t], ctx->perceus.alloc_by_type[t], ctx->perceus.death_root[t], ctx->perceus.death_cascade[t]);
    fputs("}\n", stderr);
}
#endif
void rc_context_free(RcContext *ctx) {
    if (!ctx) return;
#if SHEN_RC_STATS_COUNTERS
    perceus_report(ctx);
#endif
    rc_native_compile_free(ctx->native_compile_state);
    free((void *)ctx->hot.lean_canon);
    while (ctx->compiled_symbols) {
        RcResolvedSymbols *next = ctx->compiled_symbols->next;
        free(ctx->compiled_symbols);
        ctx->compiled_symbols = next;
    }
    ctx->destroying = true;
    free(ctx->symbols); free(ctx->static_closures); free(ctx->gc_nodes); free(ctx->walk_nodes);
    for (RcPage *page = ctx->pages, *next; page; page = next) {
        next = page->next;
        RC_UNPOISON(page, RC_PAGE_BYTES);
        size_t first = (sizeof(RcPage) + 63) & ~(size_t)63;
        for (size_t at = first; at < page->used; at += page->cell_bytes) {
            RcValue *v = (RcValue *)((char *)page + at);
            if (v->type != RC_FREE_CELL) raw_free(v);
        }
        free(page);
    }
    free(ctx->plans); free(ctx->error); free(ctx);
}
RcType rc_type(const RcValue *v) { return is_immediate(v) ? RC_NUMBER : v ? v->type : RC_NIL; }
size_t rc_refcount(const RcValue *v) {
    return is_immediate(v) ? SIZE_MAX : !v ? 0 : v->refs == RC_IMMORTAL ? SIZE_MAX : v->refs;
}
RcValue *rc_nil(void) { return &nil_value; }
bool rc_is_nil(const RcValue *v) { return v == &nil_value; }
static bool belongs(RcContext *ctx, const RcValue *v) { return v && (v == &nil_value || is_immediate(v) || ctx_of(v) == ctx); }
void rc_builtin_cache_set(RcContext *ctx, RcBuiltinSlot slot, RcValue *fn) {
    /* The private installer contract guarantees the original exact builtin. */
    if (!ctx || (unsigned)slot >= RC_BUILTIN_COUNT || !belongs(ctx,fn) ||
        rc_type(fn) != RC_CLOSURE) abort();
    RcValue *old = ctx->builtin_roots[slot];
    ctx->builtin_roots[slot] = rc_retain(fn);
    fn->builtin_slot = (uint8_t)((unsigned)slot + 1);
    rc_release(old);
}
bool rc_builtin_identity_matches(RcContext *ctx, RcBuiltinSlot slot, RcValue *fn) {
    return ctx && (unsigned)slot < RC_BUILTIN_COUNT && fn && ctx->builtin_roots[slot] == fn;
}
bool rc_builtin_call_matches(RcContext *ctx, RcBuiltinSlot slot, RcValue *fn,
                              size_t argc, RcValue *const *argv) {
    if (!ctx || (unsigned)slot >= RC_BUILTIN_COUNT || !fn ||
        ctx->builtin_roots[slot] != fn || argc != (slot >= RC_BUILTIN_EQ ? 2u : 1u) ||
        !argv) return false;
    for (size_t i = 0; i < argc; ++i) if (!belongs(ctx,argv[i])) return false;
    return true;
}
int rc_builtin_call_slot(RcContext *ctx, RcValue *fn, size_t argc,
                         RcValue *const *argv) {
    /* Most user closures and unselected primitives have no tag. Check this
     * initialized common-header byte before touching closure-specific fields. */
    if (!ctx || !is_heap(fn) || !fn->builtin_slot) return -1;
    if (fn->type != RC_CLOSURE || ctx_of(fn) != ctx) return -1;
    unsigned slot = (unsigned)fn->builtin_slot - 1;
    if (slot >= RC_BUILTIN_COUNT || ctx->builtin_roots[slot] != fn ||
        argc != (slot >= RC_BUILTIN_EQ ? 2u : 1u) || !argv) return -1;
    for (size_t i = 0; i < argc; ++i) if (!belongs(ctx,argv[i])) return -1;
    return (int)slot;
}
/* Exact coarse classes only reject impossible lexical matches. All symbols
 * outside ASCII letters, including empty and non-ASCII names, share one class.
 * Caller has established RC_SYMBOL before accessing the interned name bytes. */
static uint8_t env_key_class(const RcValue *key) {
    unsigned char first=(unsigned char)key->as.text.data[0];
    return first>='A'&&first<='Z' ? 1u : first>='a'&&first<='z' ? 2u : 4u;
}
static bool env_storage_bytes(RcContext *ctx,size_t count,size_t *bytes) {
    if (count>SIZE_MAX/2/sizeof(RcValue *)) { rc_error(ctx,"evaluator frame size overflow");return false; }
    *bytes=count>1 ? 2*count*sizeof(RcValue *) : 0;return true;
}
/* Inputs and storage width have been validated by the public API or proven by
 * the private interpreted-call contract. Both paths construct identical graph
 * nodes, initialize pooled storage, and retain the same parent/value edges. */
static RcValue *env_frame_construct(RcContext *ctx,RcValue *parent,size_t count,
                                    RcValue *const *keys,RcValue *const *values,size_t bytes) {
    if (!count) return rc_retain(parent);
    RcValue **storage=bytes ? malloc(bytes) : NULL;
    if (bytes && !storage) return rc_error(ctx,"out of memory constructing evaluator frame");
    RcValue *frame=allocate(ctx,RC_ENV,bytes);
    if (!frame) { free(storage); return NULL; }
    /* Pooled objects contain stale union bytes. Initialize every field before
     * publishing strong edges; no constructor safepoint exposes a partial node. */
    frame->as.env.count=count;
    frame->as.env.key_classes=rc_is_nil(parent) ? 0 : parent->as.env.key_classes;
    frame->as.env.keys=count>1 ? storage : frame->as.env.inline_binding;
    frame->as.env.values=count>1 ? storage+count : frame->as.env.inline_binding+1;
    frame->as.env.parent=rc_retain(parent);
    frame->reaches_vector=may_reach_vector(parent);
    for (size_t i=0;i<count;i++) {
        frame->as.env.keys[i]=keys[i];
        frame->as.env.key_classes |= env_key_class(keys[i]);
        frame->as.env.values[i]=rc_retain(values[i]);
        frame->reaches_vector |= may_reach_vector(values[i]);
    }
    ctx->graph_slots += 1 + count;
    return frame;
}
RcValue *rc_env_frame(RcContext *ctx,RcValue *parent,size_t count,
                      RcValue *const *keys,RcValue *const *values) {
    if (!ctx || !belongs(ctx,parent) || (!rc_is_nil(parent) && rc_type(parent)!=RC_ENV) ||
        (count && (!keys || !values))) return rc_error(ctx,"invalid evaluator frame");
    size_t bytes;if(!env_storage_bytes(ctx,count,&bytes))return NULL;
    for (size_t i=0;i<count;i++) {
        if (!belongs(ctx,keys[i]) || rc_type(keys[i])!=RC_SYMBOL || !belongs(ctx,values[i]))
            return rc_error(ctx,"invalid evaluator frame binding/context");
    }
    return env_frame_construct(ctx,parent,count,keys,values,bytes);
}
RcValue *rc_env_lookup(const RcValue *environment,const RcValue *key) {
    if (rc_type(key)!=RC_SYMBOL || rc_type(environment)!=RC_ENV) return NULL;
    if (!(environment->as.env.key_classes & env_key_class(key))) return NULL;
    for (const RcValue *frame=environment;rc_type(frame)==RC_ENV;frame=frame->as.env.parent)
        for (size_t i=frame->as.env.count;i>0;i--)
            if (frame->as.env.keys[i-1]==key) return frame->as.env.values[i-1];
    return NULL;
}
bool rc_prolog_guard_get(RcContext *ctx,RcValue *const *keys,unsigned *mask) {
    if(ctx->function_generation==SIZE_MAX||ctx->function_generation!=ctx->prolog_guard_generation||
       ctx->prolog_guard_keys[0]!=keys[0]||ctx->prolog_guard_keys[1]!=keys[1]||ctx->prolog_guard_keys[2]!=keys[2])return false;
    *mask=ctx->prolog_guard_mask;return true;
}
void rc_prolog_guard_set(RcContext *ctx,RcValue *const *keys,unsigned mask) {
    ctx->prolog_guard_generation=ctx->function_generation;ctx->prolog_guard_mask=mask;
    for(size_t i=0;i<3;i++)ctx->prolog_guard_keys[i]=keys[i];
}
bool rc_native_hash_certified(RcContext *ctx) {
    return ctx->function_generation<SIZE_MAX&&ctx->native_hash_generation==ctx->function_generation+1;
}
void rc_native_hash_certify(RcContext *ctx) {
    ctx->native_hash_generation=ctx->function_generation<SIZE_MAX?ctx->function_generation+1:0;
}
RcValue *rc_function_slot_borrowed(RcContext *ctx,RcValue *symbol) {
    return belongs(ctx,symbol)&&rc_type(symbol)==RC_SYMBOL ? symbol->as.text.function : NULL;
}
RcValue *rc_vector_slot_borrowed(RcValue *vector,size_t index) {
    return rc_type(vector)==RC_VECTOR&&index<vector->as.vector.length ? vector->as.vector.items[index] : NULL;
}
void rc_tier_attach(RcContext *ctx,const RcTierHooks *hooks,void *state) {
    ctx->tier_hooks=hooks; ctx->tier_state=hooks ? state : NULL;
}
const RcTierHooks *rc_tier_hooks(const RcContext *ctx,void **state) {
    if(!ctx->tier_hooks)return NULL;
    *state=ctx->tier_state; return ctx->tier_hooks;
}
bool rc_eval_plan_environment(const RcValue *environment) {
    return rc_is_nil(environment) || (rc_type(environment)==RC_ENV && !(environment->as.env.key_classes&2));
}
RcValue *rc_eval_plan_get(RcValue *expression) {
    return rc_type(expression)==RC_PAIR ? plan_of(expression) : NULL;
}
bool rc_eval_plan_heat(RcValue *expression) {
    struct RcPlanEntry *entry=plan_insert(expression);
    if(!entry)return false;
    size_t *calls=&entry->calls;
    if(*calls==SIZE_MAX)return false;
    ++*calls;
    /* Unsupported/mismatched shapes retry only at exponentially spaced
     * counts. Optional preparation therefore stays bounded on cold and
     * adversarial code without retaining a negative cache of parameter lists. */
    return *calls>=32 && !(*calls&(*calls-1));
}
void rc_eval_plan_promote(RcValue *self,RcNative prepared,RcValue *plan) {
    /* Only dynamic_call uses this after validating its exact private layout.
     * The replacement reaches a subset of the original body's descendants;
     * no new vector path or backedge is introduced. Publish both fields before
     * dropping syntax. Aliases/partial copies retain identical semantics. */
    RcValue *old=self->as.closure.captures[0];
    self->as.closure.captures[0]=rc_retain(plan);self->as.closure.native=prepared;
    rc_release(old);
}
void rc_eval_plan_set(RcValue *expression,RcValue *plan) {
    struct RcPlanEntry *entry=plan_insert(expression);
    if(!entry)return; /* Plans are optional; the expression stays unprepared. */
    RcValue *old=entry->plan;
    entry->plan=rc_retain(plan);
    ctx_of(expression)->graph_slots+=(plan!=NULL);
    ctx_of(expression)->graph_slots-=(old!=NULL);
    rc_release(old);
}
RcValue *rc_number(RcContext *ctx, double n) {
    RcValue *v = allocate(ctx, RC_NUMBER, 0);
    if (v) { v->as.number.real = n; v->as.number.exact = false; }
    return v;
}
RcValue *rc_integer(RcContext *ctx, int64_t n) {
    if (!ctx) return NULL;
#if SHEN_RC_IMMEDIATE_INTEGERS
    if (n >= -INT64_C(4611686018427387904) && n <= INT64_C(4611686018427387903))
        return (RcValue *)(uintptr_t)(((uint64_t)n << 1) | UINT64_C(1));
#endif
    RcValue *v = allocate(ctx, RC_NUMBER, 0);
    if (v) { v->as.number.integer = n; v->as.number.real = (double)n; v->as.number.exact = true; }
    return v;
}
double rc_number_value(const RcValue *v) { return is_immediate(v) ? (double)immediate_integer(v) : v && rc_type(v) == RC_NUMBER ? v->as.number.real : NAN; }
bool rc_number_is_integer(const RcValue *v) {
    return is_immediate(v) || (v && rc_type(v) == RC_NUMBER && (v->as.number.exact || (isfinite(v->as.number.real) && trunc(v->as.number.real) == v->as.number.real)));
}
bool rc_number_is_exact_integer(const RcValue *v) {
    return is_immediate(v) || (v && rc_type(v) == RC_NUMBER && v->as.number.exact);
}
bool rc_number_is_int64(const RcValue *v) {
    if (!rc_number_is_integer(v)) return false;
    return is_immediate(v) || v->as.number.exact || (v->as.number.real >= -9223372036854775808.0 && v->as.number.real < 9223372036854775808.0);
}
int64_t rc_integer_value(const RcValue *v) {
    if (is_immediate(v)) return immediate_integer(v);
    if (!v || rc_type(v) != RC_NUMBER) return 0;
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
            if (v->as.text.hash == hash && !strcmp(v->as.text.data, name)) return v;
    }
    if (!ctx->symbol_capacity || ctx->symbol_count >= ctx->symbol_capacity - ctx->symbol_capacity / 4) {
        size_t capacity = ctx->symbol_capacity ? ctx->symbol_capacity * 2 : 64;
        if (capacity < ctx->symbol_capacity) return rc_error(ctx, "symbol table size overflow");
        if (!resize_symbols(ctx, capacity)) return NULL;
    }
    RcValue *v = text_value(ctx, RC_SYMBOL, name, strlen(name));
    if (!v) return NULL;
    size_t bucket = (size_t)hash & (ctx->symbol_capacity - 1);
    v->as.text.global = NULL; v->as.text.function = NULL;
    v->as.text.hash = hash;
    /* The intern table roots every symbol until teardown, so its count can
     * never decide a lifetime. The returned owner is the immortal itself. */
    v->refs = RC_IMMORTAL;
    v->as.text.intern_next = ctx->symbols[bucket];
    ctx->symbols[bucket] = v; ctx->symbol_count++;
    /* Only a new interned identity needs name comparisons. These borrowed
     * pointers live exactly as long as the context's intern-table roots. */
    if (!strcmp(name, "true")) ctx->true_symbol = v;
    else if (!strcmp(name, "false")) ctx->false_symbol = v;
    return v;
}
RcValue *const *rc_symbol_table(RcContext *ctx, const RcSymbolTable *table) {
    if (!ctx || !table) { rc_error(ctx, "invalid compiled symbol table"); return NULL; }
    RcResolvedSymbols **cached = &ctx->compiled_symbol_cache[((uintptr_t)table >> 4) & 63];
    if (*cached && (*cached)->table == table) return (*cached)->values;
    for (RcResolvedSymbols *entry = ctx->compiled_symbols; entry; entry = entry->next)
        if (entry->table == table) { *cached = entry; return entry->values; }
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
    *cached = entry;
    return entry->values;
}
bool rc_lean_install(RcContext *ctx, const RcSymbolTable *table) {
    RcValue *const *symbols = rc_symbol_table(ctx, table);
    if (!symbols) return false;
    RcValue **canon = calloc(table->count ? table->count : 1, sizeof(*canon));
    if (!canon) { rc_error(ctx, "out of memory"); return false; }
    /* Borrowed: only an immortal binding can never be freed and its address
     * reused by another closure, so only it may stand for "unchanged". */
    for (size_t i = 0; i < table->count; ++i) {
        RcValue *fn = symbols[i]->as.text.function;
        canon[i] = fn && fn->refs == RC_IMMORTAL ? fn : NULL;
    }
    free((void *)ctx->hot.lean_canon);
    ctx->hot.lean_syms = symbols; ctx->hot.lean_canon = canon;
    return true;
}
const char *rc_text(const RcValue *v) { return v && (rc_type(v) == RC_STRING || rc_type(v) == RC_SYMBOL) ? v->as.text.data : NULL; }
size_t rc_text_length(const RcValue *v) { return v && (rc_type(v) == RC_STRING || rc_type(v) == RC_SYMBOL) ? v->as.text.length : 0; }
RcValue *rc_true(RcContext *ctx) {
    return ctx && ctx->true_symbol ? ctx->true_symbol : rc_symbol(ctx, "true");
}
RcValue *rc_false(RcContext *ctx) {
    return ctx && ctx->false_symbol ? ctx->false_symbol : rc_symbol(ctx, "false");
}
bool rc_is_true(const RcValue *v) { return rc_type(v) == RC_SYMBOL && v == ctx_of(v)->true_symbol; }
bool rc_is_false(const RcValue *v) { return rc_type(v) == RC_SYMBOL && v == ctx_of(v)->false_symbol; }
bool rc_is_boolean(const RcValue *v) {
    return rc_type(v) == RC_SYMBOL && (v == ctx_of(v)->true_symbol || v == ctx_of(v)->false_symbol);
}
RcValue *rc_cons(RcContext *ctx, RcValue *head, RcValue *tail) {
    if (!belongs(ctx, head) || !belongs(ctx, tail)) return rc_error(ctx, "invalid cons value/context");
    RcValue *v = allocate(ctx, RC_PAIR, 0);
    if (v) {
        v->as.pair.head = rc_retain(head); v->as.pair.tail = rc_retain(tail);
        v->reaches_vector = may_reach_vector(head) || may_reach_vector(tail);
    }
    return v;
}
RcValue *rc_cons_owned(RcContext *ctx, RcValue *head, RcValue *tail) {
    if (!belongs(ctx, head) || !belongs(ctx, tail)) {
        rc_release(head); rc_release(tail);
        return rc_error(ctx, "invalid owned cons value/context");
    }
    RcValue *v = allocate(ctx, RC_PAIR, 0);
    if (!v) { rc_release(head); rc_release(tail); return NULL; }
    /* Move the two owned handles into graph slots without count traffic.
     * Neither allocation nor release invokes a collection or user callback. */
    v->as.pair.head = head; v->as.pair.tail = tail;
    v->reaches_vector = may_reach_vector(head) || may_reach_vector(tail);
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
    if (candidate && rc_type(candidate) == RC_PAIR && candidate->refs == 1 &&
        candidate != head && candidate != tail) {
        /* Protect replacement children before releasing any old graph edges.
         * The candidate stays a rooted, valid pair throughout; release never
         * invokes collection or user code. Even borrowed old children are safe. */
        rc_retain(head); rc_retain(tail);
        RcValue *old_head = candidate->as.pair.head, *old_tail = candidate->as.pair.tail;
        RcValue *old_plan=candidate->has_plan?plan_remove(candidate):NULL;
        candidate->as.pair.head = head; candidate->as.pair.tail = tail;
        candidate->reaches_vector = may_reach_vector(head) || may_reach_vector(tail);
        rc_release(old_head); rc_release(old_tail);
        rc_release(old_plan);
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
    if (!out || !belongs(ctx, candidate) || rc_type(candidate) != RC_PAIR) {
        rc_release(candidate);
        return rc_error(ctx, "tail reuse projection expects a pair and output slot");
    }
    RcValue *tail = candidate->as.pair.tail;
    if (candidate->refs == 1) {
        RcValue *head = candidate->as.pair.head;
        RcValue *old_plan=candidate->has_plan?plan_remove(candidate):NULL;
        /* Ownership of the old edge moves to the C result, with no decrement.
         * Before releasing any other edge, publish a valid pair containing nil
         * in both slots. The collector can visit it at every later safepoint. */
        candidate->as.pair.head = rc_nil(); candidate->as.pair.tail = rc_nil();
        candidate->reaches_vector = false;
        rc_release(head);
        rc_release(old_plan);
        ctx->stats.pair_tail_transfers++;
    } else {
        rc_retain(tail);
        ctx->stats.pair_tail_shares++;
    }
    *out = candidate;
    return tail;
}
RcListLengthStatus rc_list_length_cached(RcValue *list,size_t *out) {
    /* Pair cells have no room for a cached length; the evaluator that calls
     * this is diagnostic. The walk is bounded by the list. */
    if(rc_is_nil(list)){*out=0;return RC_LIST_LENGTH_OK;}
    if(rc_type(list)!=RC_PAIR)return RC_LIST_LENGTH_IMPROPER;
    size_t n=0;
    RcValue *cursor=list;
    while(rc_type(cursor)==RC_PAIR) {
        if(n==SIZE_MAX-2)return RC_LIST_LENGTH_OVERFLOW;
        n++;cursor=cursor->as.pair.tail;
    }
    if(!rc_is_nil(cursor))return RC_LIST_LENGTH_IMPROPER;
    *out=n;return RC_LIST_LENGTH_OK;
}
RcValue *rc_head(RcContext *ctx, RcValue *v) { return v && rc_type(v) == RC_PAIR ? rc_retain(v->as.pair.head) : rc_error(ctx, "hd expects a nonempty list"); }
RcValue *rc_tail(RcContext *ctx, RcValue *v) { return v && rc_type(v) == RC_PAIR ? rc_retain(v->as.pair.tail) : rc_error(ctx, "tl expects a nonempty list"); }
RcValue *rc_head_borrowed(const RcValue *v) { return v && rc_type(v) == RC_PAIR ? v->as.pair.head : NULL; }
RcValue *rc_tail_borrowed(const RcValue *v) { return v && rc_type(v) == RC_PAIR ? v->as.pair.tail : NULL; }
RcValue *rc_vector(RcContext *ctx, size_t length) {
    if (length > SIZE_MAX / sizeof(RcValue *)) return rc_error(ctx, "vector size overflow");
    RcValue **items = length ? malloc(length * sizeof(*items)) : NULL;
    if (length && !items) return rc_error(ctx, "out of memory");
    for (size_t i = 0; i < length; ++i) items[i] = rc_nil();
    RcValue *v = allocate(ctx, RC_VECTOR, length * sizeof(*items));
    if (!v) { free(items); return NULL; }
    v->as.vector.items = items; v->as.vector.length = length; v->as.vector.cycle_source = false;
    v->reaches_vector = true;
    return v;
}
size_t rc_vector_length(const RcValue *v) { return v && rc_type(v) == RC_VECTOR ? v->as.vector.length : 0; }
RcValue *rc_vector_get(RcContext *ctx, RcValue *v, size_t i) {
    return v && rc_type(v) == RC_VECTOR && i < v->as.vector.length ? rc_retain(v->as.vector.items[i]) : rc_error(ctx, "absvector index out of bounds");
}
typedef struct {
    RcContext *ctx;
    RcValue *target;
    size_t n;
    bool found, failed;
} CycleSearch;
static void cycle_search_edge(RcValue *v, void *data) {
    CycleSearch *search = data;
    if (!may_reach_vector(v)) return;
    if (v == search->target) { search->found = true; return; }
    if (v->visited) return;
    v->visited = true;
    if (!walk_push(search->ctx, &search->n, v)) search->failed = true;
}
static bool reaches(RcContext *ctx, RcValue *target, RcValue *item, size_t budget, bool from_children);
static bool would_create_cycle(RcContext *ctx, RcValue *target, RcValue *item, size_t budget) {
    return reaches(ctx, target, item, budget, false);
}
/* Whether a path of one or more edges leads from source back to itself. */
static bool on_cycle(RcContext *ctx, RcValue *source) {
    return reaches(ctx, source, source, SIZE_MAX, true);
}
static bool reaches(RcContext *ctx, RcValue *target, RcValue *item, size_t budget, bool from_children) {
    /* A graph walk that changes only visited bits, all cleared before return.
     * Running out of scratch memory answers yes, the conservative answer. */
    CycleSearch search = {ctx, target, 0, false, false};
    if (from_children) edges(item, cycle_search_edge, &search);
    else cycle_search_edge(item, &search);
    bool result = false;
    for (size_t i = 0; i < search.n && !search.found && !search.failed; ++i) {
        RcValue *v = ctx->walk_nodes[i];
        size_t width = 0;
        switch (v->type) {
        case RC_PAIR: width = 2 + v->has_plan; break;
        case RC_VECTOR: width = v->as.vector.length; break;
        case RC_CLOSURE: width = v->as.closure.capture_count + v->as.closure.bound_count; break;
        case RC_ENV: width = 1 + v->as.env.count; break;
        case RC_TAIL: width = 1 + v->as.tail.argc; break;
        default: break;
        }
        /* The unrestricted policy may conservatively mark a source rather
         * than spending unbounded mutation time proving an edge harmless.
         * Count nodes as well as slots, so scalar-heavy graphs stay bounded. */
        if (!budget || width > budget - 1) { result = true; break; }
        budget -= width + 1;
        edges(v, cycle_search_edge, &search);
    }
    for (size_t i = 0; i < search.n; ++i) ctx->walk_nodes[i]->visited = false;
    return result || search.found || search.failed;
}
bool rc_vector_set(RcContext *ctx, RcValue *v, size_t i, RcValue *item) {
    if (!belongs(ctx, v) || rc_type(v) != RC_VECTOR || i >= v->as.vector.length || !belongs(ctx, item)) { rc_error(ctx, "invalid absvector assignment"); return false; }
    /* Immutable graph nodes conservatively record whether they reach a vector.
     * Their flags cannot gain paths while aliased: pair rewriting requires
     * unique ownership; pending tail rewrites remain private. Vectors always
     * have their flag set, including before the first mutation. Thus most
     * assignments need no walk. Unrestricted checks visit at most 256 nodes
     * plus slots, then conservatively mark a source. A marked source stays
     * marked until freed;
     * removing edges may leave a harmless false positive. */
    if (may_reach_vector(item) && (ctx->hot.acyclic || !v->as.vector.cycle_source) &&
        would_create_cycle(ctx, v, item, ctx->hot.acyclic ? SIZE_MAX : 256)) {
        if (ctx->hot.acyclic) {
            rc_error(ctx, "acyclic mode rejects vector assignment that would create a cycle");
            return false;
        }
        source_link(ctx, v);
#if SHEN_RC_STATS_COUNTERS
        ctx->perceus.cycle_source_marks++;
#endif
    }
    RcValue *old = v->as.vector.items[i];
    v->as.vector.items[i] = rc_retain(item);
    rc_release(old); return true;
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
    v->as.closure.bound = NULL; v->as.closure.bound_count = 0;
    for (size_t i = 0; i < capture_count; ++i) {
        copy[i] = rc_retain(captures[i]);
        v->reaches_vector |= may_reach_vector(captures[i]);
    }
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
RcValue *rc_closure_return_second(RcContext *ctx, RcNative native,
                                  size_t capture_count, RcValue *const *captures) {
    RcValue *v=closure_create(ctx,2,native,NULL,capture_count,captures);
    if(v)v->returns_second=true;
    return v;
}
RcValue *rc_closure_return_second_owned(RcContext *ctx, RcNativeOwned native,
                                        size_t capture_count, RcValue *const *captures) {
    RcValue *v=closure_create(ctx,2,NULL,native,capture_count,captures);
    if(v)v->returns_second=true;
    return v;
}
static size_t static_closure_hash(const RcValue *v) {
    uint64_t h = (uint64_t)(uintptr_t)v->as.closure.native * UINT64_C(0x9e3779b97f4a7c15);
    h ^= (uint64_t)(uintptr_t)v->as.closure.owned_native * UINT64_C(0xbf58476d1ce4e5b9);
    h ^= ((uint64_t)v->as.closure.arity << 1 | v->returns_second) * UINT64_C(0x94d049bb133111eb);
    for (size_t i = 0; i < v->as.closure.capture_count; ++i)
        h = (h ^ (uint64_t)(uintptr_t)v->as.closure.captures[i]) * UINT64_C(0x100000001b3);
    return (size_t)(h ^ h >> 29);
}
static bool static_closure_same(const RcValue *a, const RcValue *b) {
    if (a->as.closure.native != b->as.closure.native ||
        a->as.closure.owned_native != b->as.closure.owned_native ||
        a->as.closure.arity != b->as.closure.arity || a->returns_second != b->returns_second ||
        a->as.closure.capture_count != b->as.closure.capture_count) return false;
    /* Immortal captures are immediates, nil, symbols or shared closures, so
     * handle identity is value identity. */
    for (size_t i = 0; i < a->as.closure.capture_count; ++i)
        if (a->as.closure.captures[i] != b->as.closure.captures[i]) return false;
    return true;
}
static bool grow_static_closures(RcContext *ctx) {
    size_t capacity = ctx->static_capacity ? ctx->static_capacity * 2 : 256;
    if (capacity < ctx->static_capacity || capacity > SIZE_MAX / sizeof(RcValue *)) return false;
    RcValue **slots = calloc(capacity, sizeof(*slots));
    if (!slots) return false;
    for (size_t i = 0; i < ctx->static_capacity; ++i) {
        RcValue *v = ctx->static_closures[i];
        if (!v) continue;
        size_t j = static_closure_hash(v) & (capacity - 1);
        while (slots[j]) j = (j + 1) & (capacity - 1);
        slots[j] = v;
    }
    free(ctx->static_closures);
    ctx->static_closures = slots; ctx->static_capacity = capacity;
    return true;
}
RcValue *rc_closure_static(RcValue *v) {
    if (!is_counted(v) || v->type != RC_CLOSURE || v->as.closure.bound_count) return v;
    /* Immortal captures add no counted edges, so the closure cannot keep any
     * mortal value alive and cannot lie on a cycle. Its code and captures are
     * immutable: only interpreted closures promote their first capture. */
    for (size_t i = 0; i < v->as.closure.capture_count; ++i)
        if (is_counted(v->as.closure.captures[i])) return v;
    RcContext *ctx = ctx_of(v);
    /* Without set storage the closure stays ordinary: correct, just counted. */
    if (ctx->static_count >= ctx->static_capacity / 2 && !grow_static_closures(ctx)) return v;
    size_t j = static_closure_hash(v) & (ctx->static_capacity - 1);
    for (RcValue *found; (found = ctx->static_closures[j]); j = (j + 1) & (ctx->static_capacity - 1))
        if (static_closure_same(found, v)) { rc_release(v); return found; }
    ctx->static_closures[j] = v; ctx->static_count++;
    v->refs = RC_IMMORTAL;
    return v;
}
bool rc_closure_returns_second(RcContext *ctx, const RcValue *v) {
    return belongs(ctx,v) && rc_type(v)==RC_CLOSURE && v->returns_second &&
           v->as.closure.arity==2 && !v->as.closure.bound_count;
}
bool rc_call_values_belong(RcContext *ctx,RcValue *function,size_t argc,
                           RcValue *const *argv) {
    if(!ctx||!belongs(ctx,function)||(argc&&!argv))return false;
    for(size_t i=0;i<argc;i++)if(!belongs(ctx,argv[i]))return false;
    return true;
}
bool rc_closure_is_native(const RcValue *v, RcNative native) {
    return v && rc_type(v) == RC_CLOSURE && v->as.closure.native == native &&
           !v->as.closure.owned_native;
}
RcValue *rc_native_call_capture(RcContext *ctx, RcValue *fn, size_t argc,
                                 RcValue *const *argv, RcNative native) {
    if (!ctx || !belongs(ctx, fn) || rc_type(fn) != RC_CLOSURE ||
        fn->as.closure.native != native || fn->as.closure.owned_native ||
        fn->as.closure.bound_count || fn->as.closure.arity != argc ||
        !fn->as.closure.capture_count || (argc && !argv)) return NULL;
    for (size_t i = 0; i < argc; ++i) if (!belongs(ctx, argv[i])) return NULL;
    return fn->as.closure.captures[0];
}
bool rc_closure_is_owned_native(const RcValue *v, RcNativeOwned native) {
    return v && rc_type(v) == RC_CLOSURE && v->as.closure.owned_native == native;
}
bool rc_compiled_call_matches(const RcValue *v, RcNativeOwned native,size_t arity) {
    return v && rc_type(v)==RC_CLOSURE && v->as.closure.owned_native==native &&
           !v->as.closure.native && v->as.closure.arity==arity &&
           !v->as.closure.bound_count && !v->as.closure.capture_count;
}
RcValue *rc_capture(const RcValue *v, size_t i) {
    return v && rc_type(v) == RC_CLOSURE && i < v->as.closure.capture_count ? v->as.closure.captures[i] : NULL;
}
RcValue *const *rc_eval_captures(RcContext *ctx,RcValue *self,RcNative callback,size_t arity) {
    if (!ctx || !belongs(ctx,self) || rc_type(self)!=RC_CLOSURE ||
        self->as.closure.native!=callback || self->as.closure.owned_native ||
        self->as.closure.arity!=arity || arity>SIZE_MAX-2 ||
        self->as.closure.capture_count!=arity+2) {
        rc_error(ctx,"eval-kl: invalid interpreted closure captures");return NULL;
    }
    return self->as.closure.captures;
}
RcValue *rc_eval_env_frame(RcContext *ctx,RcValue *self,RcNative callback,size_t arity,
                           RcValue *const *argv,RcValue **body) {
    if(body)*body=NULL;
    if(!body||(arity&&!argv))return rc_error(ctx,"eval-kl: invalid interpreted frame inputs");
    RcValue *const *captures=rc_eval_captures(ctx,self,callback,arity);
    if(!captures)return NULL;
    size_t bytes;if(!env_storage_bytes(ctx,arity,&bytes))return NULL;
    RcValue *env=env_frame_construct(ctx,captures[1],arity,captures+2,argv,bytes);
    if(env)*body=captures[0];
    return env;
}
RcValue *const *rc_compiled_literal_slots(const RcValue *self,size_t capture,size_t first){
 RcValue *v=rc_capture(self,capture);
 if(rc_type(v)!=RC_VECTOR||first>v->as.vector.length)return NULL;
 return v->as.vector.items+first;
}
size_t rc_arity(const RcValue *v) {
    return v && rc_type(v) == RC_CLOSURE ? v->as.closure.arity - v->as.closure.bound_count : 0;
}
static RcValue *partial(RcContext *ctx, RcValue *fn, size_t argc, RcValue *const *argv) {
    size_t n = fn->as.closure.bound_count + argc;
    if (n > SIZE_MAX / sizeof(RcValue *)) return rc_error(ctx, "partial application size overflow");
    RcValue **bound = n ? malloc(n * sizeof(*bound)) : NULL;
    if (n && !bound) return rc_error(ctx, "out of memory");
    /* Ordinary construction clears semantic certificates. A partial closure
     * therefore never advertises the unbound two-argument identity contract. */
    RcValue *v = closure_create(ctx, fn->as.closure.arity, fn->as.closure.native,
                                fn->as.closure.owned_native,
                                fn->as.closure.capture_count, fn->as.closure.captures);
    if (!v) { free(bound); return NULL; }
    for (size_t i = 0; i < fn->as.closure.bound_count; ++i) bound[i] = rc_retain(fn->as.closure.bound[i]);
    for (size_t i = 0; i < argc; ++i) bound[fn->as.closure.bound_count + i] = rc_retain(argv[i]);
    for (size_t i = 0; i < n; ++i) v->reaches_vector |= may_reach_vector(bound[i]);
    v->as.closure.bound = bound; v->as.closure.bound_count = n;
    ctx->stats.live_bytes += n * sizeof(*bound);
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
    v->as.tail.guards = 0;
    v->reaches_vector = may_reach_vector(fn);
    ctx->graph_slots += argc;
    for (size_t i = 0; i < argc; ++i) {
        copy[i] = rc_retain(argv[i]); v->reaches_vector |= may_reach_vector(argv[i]);
    }
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
    v->as.tail.guards = 0;
    v->reaches_vector = may_reach_vector(fn);
    ctx->graph_slots += argc;
    for (size_t i = 0; i < argc; ++i) {
        copy[i] = argv[i]; v->reaches_vector |= may_reach_vector(argv[i]);
        argv[i] = NULL;
    }
    return v;
}
static RcValue *require_guard(RcContext *ctx,RcValue *value,unsigned guard) {
    if (!value) {
        if (!rc_has_error(ctx)) rc_error(ctx,guard==GUARD_BOOLEAN ? "missing boolean result" : "invalid owned argument/context");
        return NULL;
    }
    if (belongs(ctx,value) && rc_type(value)==RC_TAIL) {
        unsigned before=value->as.tail.guards,after=compose_guards(before,guard);
        if (value->refs>1 && after!=before) {
            RcValue *copy=rc_tailcall(ctx,value->as.tail.function,value->as.tail.argc,value->as.tail.argv);
            rc_release(value);
            if(!copy)return NULL;
            value=copy;
        }
        value->as.tail.guards=(uint8_t)after;
        return value;
    }
    if (belongs(ctx,value) && (guard==GUARD_CONTEXT || rc_is_boolean(value))) return value;
    rc_release(value);
    return rc_error(ctx,guard==GUARD_BOOLEAN ? "boolean expected" : "invalid owned argument/context");
}
RcValue *rc_require_boolean(RcContext *ctx,RcValue *value) {
    return require_guard(ctx,value,GUARD_BOOLEAN);
}
RcValue *rc_eval_require_argument_context(RcContext *ctx,RcValue *value) {
    return require_guard(ctx,value,GUARD_CONTEXT);
}
/* args == NULL denotes a normalized completion guard. Adjacent guards merge
 * without crossing an overapplication boundary, keeping long tail do/boolean
 * chains bounded and preserving error order. */
typedef struct ApplyPending {
    RcValue *args; struct ApplyPending *next; unsigned guards;
} ApplyPending;
static RcValue *apply_request(RcContext *ctx, RcValue *request) {
    RcValue *result = NULL;
    ApplyPending *pending = NULL;
    while (request) {
        if (request->as.tail.guards) {
            if (pending && !pending->args) {
                pending->guards=compose_guards(request->as.tail.guards,pending->guards);
            } else {
                ApplyPending *p=malloc(sizeof(*p));
                if(!p){rc_error(ctx,"out of memory");goto fail;}
                p->args=NULL; p->guards=request->as.tail.guards; p->next=pending; pending=p;
            }
        }
        RcValue *target = request->as.tail.function;
        RcValue *resolved = NULL;
        if (rc_type(target) == RC_SYMBOL) {
            resolved = rc_function_get(ctx, target);
            if (!resolved) goto fail;
            target = resolved;
        }
        if (rc_type(target) != RC_CLOSURE) { rc_error(ctx, "attempt to apply a non-function"); rc_release(resolved); goto fail; }
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
                p->guards=0; p->next = pending; pending = p;
            }
            size_t n = target->as.closure.arity;
            if (n > SIZE_MAX / sizeof(RcValue *)) { rc_error(ctx, "call size overflow"); rc_release(resolved); goto fail; }
            if (!target->as.closure.owned_native && !target->as.closure.bound_count) {
                /* Borrowed callbacks cannot change the slots. The request
                 * stays rooted through callback return, including collection
                 * and repeated execution of a shared request. Excess args
                 * already have their own pending root; only n are exposed. */
                result = target->as.closure.native(ctx, target, n, request->as.tail.argv);
            } else {
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
        }
        rc_release(resolved);
        rc_release(request); request = NULL;
        if (!result) goto fail;
        if (rc_has_error(ctx)) { rc_release(result); result = NULL; goto fail; }
        rc_safepoint(ctx);
        if (rc_type(result) == RC_TAIL) { request = result; result = NULL; continue; }
        while (pending && !pending->args) {
            ApplyPending *p=pending; pending=p->next; unsigned guards=p->guards; free(p);
            if(guards&GUARD_CONTEXT)result=rc_eval_require_argument_context(ctx,result);
            if(result&&(guards&GUARD_BOOLEAN))result=rc_require_boolean(ctx,result);
            if(!result)goto fail;
        }
        if (!pending) return result;
        ApplyPending *p = pending; pending = p->next;
        request = p->args;
        /* Replace the dummy function edge with this owned result. */
        request->as.tail.function = result;
        request->reaches_vector |= may_reach_vector(result);
        result = NULL;
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
    if (rc_type(target) == RC_SYMBOL) target = target->as.text.function;
    if (!target || rc_type(target) != RC_CLOSURE || target->as.closure.bound_count ||
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
    return rc_type(result) == RC_TAIL ? apply_request(ctx, result) : result;
}
RcValue *rc_compiled_call_complete(RcContext *ctx,RcValue *result,
                                    size_t argc,RcValue **argv) {
    consume_arguments(argc,argv);
    if (!result) return NULL;
    if (rc_has_error(ctx)) { rc_release(result); return NULL; }
    rc_safepoint(ctx);
    return rc_type(result)==RC_TAIL ? apply_request(ctx,result) : result;
}
/* Private generated/evaluator callers already root fn through completion.
 * Their scratch slots are the actual owned callback array, avoiding a second
 * callee owner and copy. Partial/overapplication and uncertain contexts retain
 * the public checked path. Borrowed callbacks keep all argument roots here. */
RcValue *rc_apply_rooted_owned_args(RcContext *ctx,RcValue *fn,size_t argc,RcValue **argv) {
    if(!rc_call_values_belong(ctx,fn,argc,argv)||rc_type(fn)!=RC_CLOSURE||
       fn->as.closure.bound_count||fn->as.closure.arity!=argc)
        return rc_apply_owned_args(ctx,fn,argc,argv);
    RcValue *result=fn->as.closure.owned_native
        ? fn->as.closure.owned_native(ctx,fn,argc,argv)
        : fn->as.closure.native(ctx,fn,argc,argv);
    return rc_compiled_call_complete(ctx,result,argc,argv);
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
FILE *rc_stream_file(const RcValue *v) { return v && rc_type(v) == RC_STREAM ? v->as.stream.file : NULL; }
bool rc_stream_is_input(const RcValue *v) { return v && rc_type(v) == RC_STREAM && v->as.stream.input; }
bool rc_stream_close(RcContext *ctx, RcValue *v) {
    if (!belongs(ctx, v) || rc_type(v) != RC_STREAM || !v->as.stream.file) { rc_error(ctx, "close expects an open stream"); return false; }
    FILE *file = v->as.stream.file;
    v->as.stream.file = NULL;
    if (v->as.stream.close && fclose(file)) { rc_error(ctx, "stream close failed"); return false; }
    return true;
}
static bool set_symbol_slot(RcContext *ctx, RcValue *symbol, RcValue *value, bool function) {
    if (!belongs(ctx, symbol) || rc_type(symbol) != RC_SYMBOL || !belongs(ctx, value)) { rc_error(ctx, "invalid binding/context"); return false; }
    /* These are context roots, not graph edges: the context owns their counts.
     * Interned symbols survive until context teardown, so slots stay addressable. */
    RcValue **slot = function ? &symbol->as.text.function : &symbol->as.text.global;
    /* Every function publication invalidates dependency certificates. A
     * saturated generation disables cache hits, rather than wrapping. */
    if(function&&ctx->function_generation<SIZE_MAX)ctx->function_generation++;
    RcValue *old = *slot; *slot = rc_retain(value); rc_release(old); return true;
}
RcValue *rc_global_get(RcContext *ctx, RcValue *symbol) {
    if (!belongs(ctx, symbol) || rc_type(symbol) != RC_SYMBOL) return rc_error(ctx, "value expects a symbol");
    return symbol->as.text.global ? rc_retain(symbol->as.text.global) : rc_error(ctx, "variable %s has no value", rc_text(symbol));
}
bool rc_global_set(RcContext *ctx, RcValue *symbol, RcValue *value) { return ctx && set_symbol_slot(ctx, symbol, value, false); }
RcValue *rc_function_lookup(RcContext *ctx, RcValue *symbol) {
    if (!belongs(ctx, symbol) || rc_type(symbol) != RC_SYMBOL) return NULL;
    return rc_retain(symbol->as.text.function);
}
RcValue *rc_function_get(RcContext *ctx, RcValue *symbol) {
    RcValue *v = rc_function_lookup(ctx, symbol);
    return v ? v : rc_error(ctx, "undefined function %s", rc_text(symbol) ? rc_text(symbol) : "<non-symbol>");
}
RcValue *rc_function_get_borrowed(RcContext *ctx, RcValue *symbol) {
    RcValue *v = belongs(ctx, symbol) && rc_type(symbol) == RC_SYMBOL ? symbol->as.text.function : NULL;
    return v ? v : rc_error(ctx, "undefined function %s", rc_text(symbol) ? rc_text(symbol) : "<non-symbol>");
}
bool rc_function_set(RcContext *ctx, RcValue *symbol, RcValue *fn) {
    if (!fn || rc_type(fn) != RC_CLOSURE) { rc_error(ctx, "function binding expects a closure"); return false; }
    return ctx && set_symbol_slot(ctx, symbol, fn, true);
}

typedef struct EqualPair { const RcValue *left, *right; } EqualPair;
static bool scalar_equal(const RcValue *a, const RcValue *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    /* Exact immediate handles encode the complete integer identity. */
    if (is_immediate(a) && is_immediate(b)) return false;
    RcType type = rc_type(a);
    if (type != rc_type(b)) return false;
    if (type == RC_NUMBER) {
        bool exact_a = rc_number_is_exact_integer(a), exact_b = rc_number_is_exact_integer(b);
        if (exact_a && exact_b) return rc_integer_value(a) == rc_integer_value(b);
        if (exact_a || exact_b) {
            const RcValue *i = exact_a ? a : b, *d = exact_a ? b : a;
            double n = d->as.number.real;
            return isfinite(n) && n >= -9223372036854775808.0 && n < 9223372036854775808.0 && trunc(n) == n && (int64_t)n == rc_integer_value(i);
        }
        return a->as.number.real == b->as.number.real;
    }
    if (type == RC_SYMBOL) {
        /* Within one intern table different identities imply different names.
         * Preserve the public API's textual equality across contexts. */
        if (ctx_of(a) == ctx_of(b) || a->as.text.hash != b->as.text.hash) return false;
    } else if (type != RC_STRING) return false;
    return a->as.text.length == b->as.text.length &&
           !memcmp(a->as.text.data, b->as.text.data, a->as.text.length);
}
static bool structural(const RcValue *a) {
    return is_heap(a) && (a->type == RC_PAIR || a->type == RC_VECTOR);
}
static size_t equal_hash(EqualPair pair) {
    uintptr_t a = (uintptr_t)pair.left >> 3, b = (uintptr_t)pair.right >> 3;
    a ^= a >> 17; b ^= b >> 13;
    return (size_t)((a * UINT64_C(0x9e3779b97f4a7c15)) ^ (b * UINT64_C(0xbf58476d1ce4e5b9)));
}
/* Only structural comparisons enter the worklist/hash set. Scalar children
 * compare immediately; repeated graph edges never cause repeated expansion.
 * The first 32 pairs are deduplicated by a linear scan, so a small
 * comparison neither zeroes nor allocates a hash table. */
static bool graph_equal(const RcValue *left, const RcValue *right) {
    RcContext *ctx = ctx_of(left);
    EqualPair local_pairs[32], *pairs = local_pairs;
    size_t *seen = NULL;
    size_t count = 1, capacity = 32, cursor = 0;
    pairs[0] = (EqualPair){left, right};
    bool equal = true;
    while (cursor < count && equal) {
        EqualPair p = pairs[cursor++];
        const RcValue *a = p.left, *b = p.right;
        size_t n = a->type == RC_PAIR ? 2 : a->as.vector.length;
        if (a->type == RC_VECTOR && n != b->as.vector.length) { equal = false; break; }
        for (size_t i = 0; i < n; ++i) {
            EqualPair child = a->type == RC_PAIR ?
                (i ? (EqualPair){a->as.pair.tail,b->as.pair.tail} : (EqualPair){a->as.pair.head,b->as.pair.head}) :
                (EqualPair){a->as.vector.items[i],b->as.vector.items[i]};
            if (child.left == child.right) continue;
            if (!structural(child.left) || !structural(child.right) || child.left->type != child.right->type) {
                if (!scalar_equal(child.left, child.right)) { equal = false; break; }
                continue;
            }
            size_t slot = 0;
            if (!seen) {
                bool known = false;
                for (size_t k = 0; k < count && !known; ++k)
                    known = pairs[k].left == child.left && pairs[k].right == child.right;
                if (known) continue;
            } else {
                slot = equal_hash(child) & (capacity * 2 - 1);
                while (seen[slot] && (pairs[seen[slot]-1].left != child.left || pairs[seen[slot]-1].right != child.right))
                    slot = (slot + 1) & (capacity * 2 - 1);
                if (seen[slot]) continue;
            }
            if (count == capacity) {
                if (capacity > SIZE_MAX / 4 / sizeof(*seen) || capacity > SIZE_MAX / 2 / sizeof(*pairs)) {
                    rc_error(ctx, "comparison size overflow"); equal = false; break;
                }
                size_t next = capacity * 2;
                EqualPair *grown = malloc(next * sizeof(*pairs));
                size_t *buckets = calloc(next * 2, sizeof(*seen));
                if (!grown || !buckets) {
                    free(grown); free(buckets); rc_error(ctx, "out of memory comparing values"); equal = false; break;
                }
                memcpy(grown, pairs, count * sizeof(*pairs));
                if (pairs != local_pairs) free(pairs);
                free(seen);
                pairs = grown; seen = buckets; capacity = next;
                for (size_t k = 0; k < count; ++k) {
                    size_t bucket = equal_hash(pairs[k]) & (capacity * 2 - 1);
                    while (seen[bucket]) bucket = (bucket + 1) & (capacity * 2 - 1);
                    seen[bucket] = k + 1;
                }
                slot = equal_hash(child) & (capacity * 2 - 1);
                while (seen[slot]) slot = (slot + 1) & (capacity * 2 - 1);
            }
            pairs[count] = child; ++count;
            if (seen) seen[slot] = count;
        }
    }
    if (pairs != local_pairs) free(pairs);
    free(seen);
    return equal;
}
bool rc_equal(const RcValue *left, const RcValue *right) {
    if (left == right) return true;
    if (!left || !right) return false;
    RcType type = rc_type(left);
    if (type != RC_PAIR && type != RC_VECTOR) return scalar_equal(left, right);
    if (type != rc_type(right)) return false;
    /* Immutable pairs cannot form a cycle without a vector. Walk flat lists
     * without allocation; a small explicit stack also covers ordinary trees.
     * Bound branching work to avoid exponential traversal of shared DAGs. */
    EqualPair pending[32];
    size_t depth = 0, branches = 0;
    const RcValue *a = left, *b = right;
    for (;;) {
        if (a != b) {
            if (!structural(a) || !structural(b) || a->type != b->type) {
                if (!scalar_equal(a, b)) return false;
            } else {
                if (a->type != RC_PAIR || may_reach_vector(a) || may_reach_vector(b)) return graph_equal(left, right);
                const RcValue *h = a->as.pair.head, *k = b->as.pair.head;
                if (h != k) {
                    if (structural(h) && structural(k) && h->type == k->type) {
                        /* Identical head/tail subgraphs impose one relation,
                         * even when the two compared DAGs have distinct nodes. */
                        if (h == a->as.pair.tail && k == b->as.pair.tail) { a = h; b = k; continue; }
                        if (depth == 32 || ++branches == 256) return graph_equal(left, right);
                        pending[depth++] = (EqualPair){a->as.pair.tail,b->as.pair.tail};
                        a = h; b = k; continue;
                    }
                    if (!scalar_equal(h, k)) return false;
                }
                a = a->as.pair.tail; b = b->as.pair.tail; continue;
            }
        }
        if (!depth) return true;
        EqualPair next = pending[--depth]; a = next.left; b = next.right;
    }
}
