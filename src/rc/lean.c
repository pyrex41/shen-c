/* Lean C compiler front end: KLambda -> Core -> LIR, lir_check, printers.
 * See lean.h and DESIGN B1 §1-§2. Host-only (rc-build).
 *
 * Core is the resolved, alpha-renamed KL tree. Every call is classified:
 *   prim    a runtime primitive (always through a binding guard; DESIGN
 *           seal-list (f)), including shen-c's native overwrites (d);
 *   known   a lean kernel defun called directly (sealed, kernel mode);
 *   guarded a lean kernel defun called directly only while its binding is
 *           canonical (shen.pvar?, seal-list (f));
 *   global  through the symbol's current binding (everything else,
 *           including every never-seal name);
 *   apply   a computed callee.
 * do: the kernel defines (defun do (A B) B); a sealed call to it is
 * sequencing (Core do), so B stays in tail position, as rc_transparent_do
 * gives the generic emitter.
 * Resolution uses post-bootstrap bindings: kernel defuns replace core
 * primitives of the same name (shen.gc), and the overwrites replace kernel
 * defuns (rc_bootstrap_kernel's install order).
 *
 * LIR is direct-style ANF. Statements form a tree: let, if, join points
 * (jp j(params) = join; body ... jmp j(args)), trap regions, ret and fail.
 * A let whose right-hand side may fail carries !fail: the backend checks it
 * (NULL or negative) and leaves through the error path. Values are typed
 * obj, bool or kref, with a mode own/bor (L0 lowers everything as owned;
 * borrow inference is L1b). */
#define _POSIX_C_SOURCE 200809L
#include "lean.h"
#include <inttypes.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Seal list                                                           */

/* (d) runtime replacements and (e) compile-kl reader hooks never become
 * Known targets. Primitives among them stay primitives (guarded). */
static const char *const never_sealed[] = {
    /* (d) primitives.c overwrites and the numeric hashkey wrapper */
    "pr", "symbol?", "variable?", "integer?", "boolean?", "not", "value/or",
    "<-address/or", "read-file-as-charlist", "shen.hashkey",
    /* (e) src/rc/main.c reader_hooks */
    "shen.store-arity", "shen.record-kl", "shen.record-external",
    "shen.record-internal", "eval-kl", "shen.record-macro",
    "shen.remember-datatype", "set", "put", "unput", "address->",
    "shen.synonyms-h",
};
/* (f) test_prolog_fast.sh redefines shen.pvar? and expects lazyderef's
 * callers to see it: a direct call is valid only under a binding guard. */
static const char *const guarded_defuns[] = {"shen.pvar?"};

static bool in_list(const char *const *list, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++) if (!strcmp(list[i], name)) return true;
    return false;
}
bool rc_lean_never_sealed(const char *name) {
    return in_list(never_sealed, sizeof never_sealed / sizeof *never_sealed, name);
}
bool rc_lean_guarded_defun(const char *name) {
    return in_list(guarded_defuns, sizeof guarded_defuns / sizeof *guarded_defuns, name);
}

/* ------------------------------------------------------------------ */
/* Primitives                                                          */

/* Runtime primitives (primitives.c core and overwrites, eval.c eval-kl).
 * test: returns a boolean, so a condition lowers to a C test. */
typedef struct { const char *name; int arity; bool test, overwrite; } Prim;
static const Prim prims[] = {
    {"intern",1,0,0},{"pos",2,0,0},{"tlstr",1,0,0},{"cn",2,0,0},{"str",1,0,0},
    {"string?",1,1,0},{"n->string",1,0,0},{"string->n",1,0,0},{"set",2,0,0},
    {"value",1,0,0},{"simple-error",1,0,0},{"error-to-string",1,0,0},
    {"cons",2,0,0},{"hd",1,0,0},{"tl",1,0,0},{"cons?",1,1,0},{"=",2,1,0},
    {"type",2,0,0},{"absvector",1,0,0},{"address->",3,0,0},{"<-address",2,0,0},
    {"absvector?",1,1,0},{"write-byte",2,0,0},{"read-byte",1,0,0},{"open",2,0,0},
    {"close",1,0,0},{"get-time",1,0,0},{"+",2,0,0},{"-",2,0,0},{"*",2,0,0},
    {"/",2,0,0},{">",2,1,0},{"<",2,1,0},{">=",2,1,0},{"<=",2,1,0},
    {"number?",1,1,0},{"function",1,0,0},{"exit",1,0,0},{"shen.gc",0,0,0},
    {"do",2,0,0},{"char-stream?",1,1,0},{"read-unit-string",1,0,0},
    {"write-string",2,0,0},{"eval-kl",1,0,0},
    /* overwrites: installed after the kernel defuns */
    {"symbol?",1,1,1},{"boolean?",1,1,1},{"integer?",1,1,1},{"variable?",1,1,1},
    {"not",1,1,1},{"value/or",2,0,1},{"<-address/or",3,0,1},{"pr",2,0,1},
    {"read-file-as-charlist",1,0,1},
};
#define NPRIMS ((int)(sizeof prims / sizeof *prims))
static int prim_index(const char *name) {
    for (int i = 0; i < NPRIMS; i++) if (!strcmp(prims[i].name, name)) return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Program, arena, errors                                              */

typedef struct Chunk { struct Chunk *next; max_align_t align; unsigned char data[]; } Chunk;
typedef struct { const char *name; RcValue *params, *body; int arity; bool lean; } KDef;
typedef enum { CVAR, CLIT, CPRIM, CKNOWN, CGUARD, CGLOBAL, CAPPLY, CIF, CLET, CLAM,
               CFRZ, CTRAP, CAND, COR, CERR, CDO } CoreKind;
typedef struct Core Core;
struct Core {
    CoreKind k;
    int var;          /* CVAR: var; CLET/CLAM: bound var */
    RcValue *lit;     /* CLIT */
    const char *name; /* call targets, CERR message */
    int idx;          /* CPRIM: prim index; CKNOWN/CGUARD: defun index */
    Core **a; int n;  /* children: args (CAPPLY: a[0] callee), if c/t/e, let init/body, ... */
};
typedef enum { TY_OBJ, TY_BOOL, TY_KREF } LTy;
typedef enum { MD_OWN, MD_BOR } LMode;
typedef struct { const char *hint; LTy ty; LMode md; } LVar;
typedef enum { R_LIT, R_PRIM, R_TEST, R_TRUTH, R_BOOL, R_BOX, R_REQB, R_SNAP,
               R_CALL, R_GCALL, R_APPLY, R_CLOS, R_ERRMSG } RKind;
typedef struct {
    RKind k;
    RcValue *lit;     /* R_LIT */
    int idx;          /* R_PRIM/R_TEST: prim; R_CALL: defun; R_CLOS: fn; R_BOOL: 0/1 */
    const char *sym;  /* R_SNAP/R_GCALL/R_CALL: symbol name */
    int *args; int nargs;
    int snap;         /* binding snapshot var, or -1: resolve at the call */
    const char *msg;  /* R_TEST/R_TRUTH: non-boolean message */
} LRhs;
typedef enum { S_LET, S_IF, S_JP, S_JMP, S_RET, S_FAIL, S_TRAP } SKind;
typedef struct LStmt LStmt;
struct LStmt {
    SKind k;
    int x;              /* S_LET: var; S_IF: condition; S_RET: value */
    LRhs r; bool fails; /* S_LET */
    LStmt *next;        /* S_LET continuation */
    LStmt *a, *b;       /* S_IF then/else; S_JP body/join; S_TRAP body/handler */
    int label;          /* S_JP, S_JMP */
    int *args; int nargs; /* S_JP params; S_JMP args */
    const char *msg;    /* S_FAIL */
};
typedef enum { FN_DEFUN, FN_LAMBDA, FN_FREEZE } FnKind;
typedef struct {
    const char *name; FnKind kind; int parent;
    int *params; int nparams;
    int *caps; int ncaps;      /* this fn's capture vars */
    LVar *vars; int nvars, capvars;
    int nlabels;
    LStmt *body;
} LFn;
typedef struct {
    Core *core; int ncorevars; const char **corenames;
    LFn **fns; int nfns, capfns; /* stable addresses: holes point into them */
} Unit;

struct RcLean {
    RcContext *ctx;
    Chunk *mem;
    KDef *defs; size_t ndefs;
    int *table; size_t tcap;       /* name hash -> defs index + 1 */
    jmp_buf fail; char error[512]; bool rejected;
    Unit *unit;                    /* function being compiled */
    /* resolution statistics for check_all */
    long sites_prim, sites_known, sites_guard, sites_global, sites_apply, sites_do;
    bool do_identity; /* the kernel's do is exactly (defun do (A B) B) */
};

static void *A(RcLean *L, size_t n) {
    Chunk *c = calloc(1, sizeof(Chunk) + (n ? n : 1));
    if (!c) { snprintf(L->error, sizeof L->error, "lean: out of memory"); L->rejected = false; longjmp(L->fail, 1); }
    c->next = L->mem; L->mem = c; return c->data;
}
static void *grow(RcLean *L, void *old, size_t oldn, size_t n) {
    void *p = A(L, n); if (old && oldn) memcpy(p, old, oldn); return p;
}
static char *dup(RcLean *L, const char *s) { size_t n = strlen(s) + 1; char *p = A(L, n); memcpy(p, s, n); return p; }
static _Noreturn void fail(RcLean *L, bool reject, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(L->error, sizeof L->error, fmt, ap); va_end(ap);
    L->rejected = reject; longjmp(L->fail, 1);
}

/* ------------------------------------------------------------------ */
/* AST helpers                                                         */

static bool is_sym(const RcValue *x) { return rc_type(x) == RC_SYMBOL; }
static bool is_pair(const RcValue *x) { return rc_type(x) == RC_PAIR; }
static const char *sname(const RcValue *x) { return rc_text(x); }
static bool named(const RcValue *x, const char *s) { return is_sym(x) && !strcmp(sname(x), s); }
static RcValue *hd(RcValue *x) { return rc_head_borrowed(x); }
static RcValue *tl(RcValue *x) { return rc_tail_borrowed(x); }
static int length(RcLean *L, RcValue *x) {
    int n = 0;
    for (; is_pair(x); x = tl(x)) n++;
    if (!rc_is_nil(x)) fail(L, true, "improper list in KL form");
    return n;
}
static RcValue *nth(RcValue *x, int i) { while (i--) x = tl(x); return hd(x); }

static size_t hash(const char *s) { size_t h = 1469598103934665603u; while (*s) h = (h ^ (unsigned char)*s++) * 1099511628211u; return h; }
static int lookup_def(RcLean *L, const char *name) {
    if (!L->tcap) return -1;
    for (size_t i = hash(name) & (L->tcap - 1);; i = (i + 1) & (L->tcap - 1)) {
        int e = L->table[i];
        if (!e) return -1;
        if (!strcmp(L->defs[e - 1].name, name)) return e - 1;
    }
}

/* ------------------------------------------------------------------ */
/* Resolve: KL -> Core                                                 */

typedef struct Scope { const char *name; int var; const struct Scope *up; } Scope;
static int scope_find(const Scope *s, const char *name) {
    for (; s; s = s->up) if (!strcmp(s->name, name)) return s->var;
    return -1;
}
static Core *node(RcLean *L, CoreKind k, int n) {
    Core *c = A(L, sizeof *c); c->k = k; c->n = n; c->var = -1; c->idx = -1;
    if (n) c->a = A(L, (size_t)n * sizeof *c->a);
    return c;
}
static int new_corevar(RcLean *L, const char *name) {
    Unit *u = L->unit;
    if (!(u->ncorevars & 63)) u->corenames = grow(L, u->corenames, (size_t)u->ncorevars * sizeof *u->corenames, (size_t)(u->ncorevars + 64) * sizeof *u->corenames);
    /* Alpha-renaming: a name bound twice in one defun gets a ' suffix. */
    char buf[256]; snprintf(buf, sizeof buf, "%s", name);
    for (bool clash = true; clash;) {
        clash = false;
        for (int i = 0; i < u->ncorevars; i++) if (!strcmp(u->corenames[i], buf)) { clash = true; break; }
        if (clash && strlen(buf) + 2 < sizeof buf) strcat(buf, "'");
        else if (clash) fail(L, false, "lean: variable name too long");
    }
    u->corenames[u->ncorevars] = dup(L, buf);
    return u->ncorevars++;
}
static Core *resolve(RcLean *L, RcValue *x, const Scope *sc);
static Core *call_node(RcLean *L, RcValue *head, RcValue *args, const Scope *sc) {
    int n = length(L, args);
    const char *name = sname(head);
    int p = prim_index(name), d = lookup_def(L, name);
    CoreKind k; int idx = -1;
    if (p >= 0 && prims[p].overwrite) { k = prims[p].arity == n ? CPRIM : CGLOBAL; idx = p; }
    else if (d >= 0) {
        bool direct = L->defs[d].lean && L->defs[d].arity == n && !rc_lean_never_sealed(name);
        k = !direct ? CGLOBAL : rc_lean_guarded_defun(name) ? CGUARD : CKNOWN; idx = d;
    } else if (p >= 0 && prims[p].arity == n) { k = CPRIM; idx = p; }
    else k = CGLOBAL;
    if (k == CGLOBAL) idx = -1;
    if (n == 2 && d >= 0 && !strcmp(name, "do") && L->do_identity) {
        /* Intrinsic: independent of the lean set, like if. */
        if (k == CKNOWN) L->sites_known--; else L->sites_global--;
        L->sites_do++;
        Core *c = node(L, CDO, 2);
        for (int i = 0; i < 2; i++, args = tl(args)) c->a[i] = resolve(L, hd(args), sc);
        return c;
    }
    switch (k) {
    case CPRIM: L->sites_prim++; break; case CKNOWN: L->sites_known++; break;
    case CGUARD: L->sites_guard++; break; default: L->sites_global++; break;
    }
    Core *c = node(L, k, n); c->name = name; c->idx = idx;
    for (int i = 0; i < n; i++, args = tl(args)) c->a[i] = resolve(L, hd(args), sc);
    return c;
}
static Core *resolve(RcLean *L, RcValue *x, const Scope *sc) {
    if (is_sym(x)) {
        int v = scope_find(sc, sname(x));
        if (v >= 0) { Core *c = node(L, CVAR, 0); c->var = v; return c; }
        Core *c = node(L, CLIT, 0); c->lit = x; return c;
    }
    if (!is_pair(x)) {
        RcType t = rc_type(x);
        if (t != RC_NIL && t != RC_NUMBER && t != RC_STRING) fail(L, true, "unsupported literal");
        Core *c = node(L, CLIT, 0); c->lit = x; return c;
    }
    RcValue *h = hd(x), *args = tl(x);
    int n = length(L, args);
    bool special = is_sym(h) && scope_find(sc, sname(h)) < 0;
    if (special && named(h, "if")) {
        if (n != 3) fail(L, true, "if arity");
        Core *c = node(L, CIF, 3);
        for (int i = 0; i < 3; i++) c->a[i] = resolve(L, nth(args, i), sc);
        return c;
    }
    if (special && named(h, "cond")) {
        /* Nested ifs ending in the generic emitter's no-true-branch error. */
        Core *c = node(L, CERR, 0); c->name = "cond has no true branch";
        Core **tail = &c;
        Core *root = NULL; Core **link = &root;
        for (RcValue *q = args; is_pair(q); q = tl(q)) {
            RcValue *cl = hd(q);
            if (length(L, cl) != 2) fail(L, true, "cond clause");
            Core *i = node(L, CIF, 3);
            i->a[0] = resolve(L, hd(cl), sc); i->a[1] = resolve(L, nth(cl, 1), sc);
            *link = i; link = &i->a[2];
        }
        *link = *tail;
        return root ? root : c;
    }
    if (special && named(h, "let")) {
        if (n != 3 || !is_sym(hd(args))) fail(L, true, "let shape");
        Core *c = node(L, CLET, 2);
        c->a[0] = resolve(L, nth(args, 1), sc);
        c->var = new_corevar(L, sname(hd(args)));
        Scope s = {sname(hd(args)), c->var, sc};
        c->a[1] = resolve(L, nth(args, 2), &s);
        return c;
    }
    if (special && named(h, "lambda")) {
        if (n != 2 || !is_sym(hd(args))) fail(L, true, "lambda shape");
        Core *c = node(L, CLAM, 1);
        c->var = new_corevar(L, sname(hd(args)));
        Scope s = {sname(hd(args)), c->var, sc};
        c->a[0] = resolve(L, nth(args, 1), &s);
        return c;
    }
    if (special && named(h, "freeze")) {
        if (n != 1) fail(L, true, "freeze arity");
        Core *c = node(L, CFRZ, 1); c->a[0] = resolve(L, hd(args), sc); return c;
    }
    if (special && named(h, "trap-error")) {
        if (n != 2) fail(L, true, "trap-error arity");
        Core *c = node(L, CTRAP, 2);
        c->a[0] = resolve(L, hd(args), sc); c->a[1] = resolve(L, nth(args, 1), sc);
        return c;
    }
    if (special && (named(h, "and") || named(h, "or")) && n == 2) {
        Core *c = node(L, named(h, "and") ? CAND : COR, 2);
        c->a[0] = resolve(L, hd(args), sc); c->a[1] = resolve(L, nth(args, 1), sc);
        return c;
    }
    if (special && named(h, "type")) {
        if (n != 2) fail(L, true, "type arity");
        return resolve(L, hd(args), sc);
    }
    if (special && (named(h, "defun") || named(h, "quote") || named(h, "c-quote")))
        fail(L, true, "unsupported form %s", sname(h));
    if (special) return call_node(L, h, args, sc);
    Core *c = node(L, CAPPLY, n + 1);
    c->a[0] = resolve(L, h, sc);
    for (int i = 0; i < n; i++, args = tl(args)) c->a[i + 1] = resolve(L, hd(args), sc);
    L->sites_apply++;
    return c;
}

/* ------------------------------------------------------------------ */
/* Lower: Core -> LIR                                                  */

typedef struct { RcLean *L; int fn; LStmt **hole; int *map; } Lw;
static LFn *FN(Lw *w) { return w->L->unit->fns[w->fn]; }
static int new_fn(RcLean *L, const char *name, FnKind kind, int parent) {
    Unit *u = L->unit;
    if (u->nfns == u->capfns) {
        int cap = u->capfns ? u->capfns * 2 : 8;
        u->fns = grow(L, u->fns, (size_t)u->capfns * sizeof *u->fns, (size_t)cap * sizeof *u->fns);
        u->capfns = cap;
    }
    LFn *f = A(L, sizeof *f); u->fns[u->nfns] = f;
    f->name = name; f->kind = kind; f->parent = parent;
    return u->nfns++;
}
static int nv(Lw *w, LTy ty, const char *hint) {
    LFn *f = FN(w);
    if (f->nvars == f->capvars) {
        int cap = f->capvars ? f->capvars * 2 : 32;
        f->vars = grow(w->L, f->vars, (size_t)f->capvars * sizeof *f->vars, (size_t)cap * sizeof *f->vars);
        f->capvars = cap;
    }
    f->vars[f->nvars] = (LVar){hint, ty, MD_OWN};
    return f->nvars++;
}
static LStmt *stmt(Lw *w, SKind k) { LStmt *s = A(w->L, sizeof *s); s->k = k; s->x = -1; s->label = -1; return s; }
static bool live(const Lw *w) { return w->hole != NULL; }
static void close_block(Lw *w, LStmt *s) { if (live(w)) *w->hole = s; w->hole = NULL; }
static bool rhs_fails(const LRhs *r) {
    switch (r->k) {
    case R_LIT: return rc_type(r->lit) == RC_NUMBER || rc_type(r->lit) == RC_STRING; /* allocation */
    case R_BOOL: case R_BOX: return false;
    default: return true;
    }
}
static LTy rhs_type(const LRhs *r) { return r->k == R_TEST || r->k == R_TRUTH || r->k == R_BOOL ? TY_BOOL : TY_OBJ; }
static int let(Lw *w, LRhs r, const char *hint) {
    if (!live(w)) return -1;
    LStmt *s = stmt(w, S_LET);
    s->x = nv(w, rhs_type(&r), hint); s->r = r; s->fails = rhs_fails(&r);
    *w->hole = s; w->hole = &s->next;
    return s->x;
}
static LRhs rhs(RKind k) { LRhs r; memset(&r, 0, sizeof r); r.k = k; r.snap = -1; r.idx = -1; return r; }
static int lv(Lw *w, Core *e);
static void lt(Lw *w, Core *e);
static int lb(Lw *w, Core *e, bool rhs_boolean);
static const char *cond_msg(bool rhs_boolean) { return rhs_boolean ? "boolean expected" : "condition is not a boolean"; }
/* Evaluate call arguments left to right. A binding snapshot precedes them
 * when an argument could run code (anything but a variable or literal),
 * since the generic emitter resolves the callee before its arguments. */
static int *call_args(Lw *w, Core **a, int n, const char *snapsym, int *snap) {
    *snap = -1;
    if (snapsym) for (int i = 0; i < n; i++) if (a[i]->k != CVAR && a[i]->k != CLIT) {
        LRhs r = rhs(R_SNAP); r.sym = snapsym; *snap = let(w, r, NULL); break;
    }
    int *v = n ? A(w->L, (size_t)n * sizeof *v) : NULL;
    for (int i = 0; i < n; i++) v[i] = lv(w, a[i]);
    return v;
}
static int lower_closure(Lw *w, Core *e);
static int lower_call(Lw *w, Core *e, bool test, bool rhs_boolean) {
    int snap; LRhs r;
    switch (e->k) {
    case CPRIM:
        r = rhs(test ? R_TEST : R_PRIM); r.idx = e->idx; r.sym = e->name;
        r.args = call_args(w, e->a, e->n, e->name, &snap); r.nargs = e->n; r.snap = snap;
        if (test) r.msg = cond_msg(rhs_boolean);
        return let(w, r, NULL);
    case CKNOWN: case CGUARD:
        r = rhs(R_CALL); r.idx = e->idx; r.sym = e->name;
        r.args = call_args(w, e->a, e->n, e->k == CGUARD ? e->name : NULL, &snap); r.nargs = e->n; r.snap = snap;
        return let(w, r, NULL);
    case CGLOBAL:
        r = rhs(R_GCALL); r.sym = e->name;
        r.args = call_args(w, e->a, e->n, e->name, &snap); r.nargs = e->n; r.snap = snap;
        return let(w, r, NULL);
    default: {
        r = rhs(R_APPLY);
        int f = lv(w, e->a[0]);
        r.args = A(w->L, (size_t)e->n * sizeof *r.args); r.args[0] = f; r.nargs = e->n;
        for (int i = 1; i < e->n; i++) r.args[i] = lv(w, e->a[i]);
        return let(w, r, NULL);
    }
    }
}
/* jp j(y:ty) = <continuation>; <body>: returns the label; body goes to *body. */
static LStmt *join(Lw *w, LTy ty, int *y, LStmt ***body) {
    LStmt *s = stmt(w, S_JP);
    s->label = FN(w)->nlabels++;
    *y = nv(w, ty, NULL);
    s->args = A(w->L, sizeof *s->args); s->args[0] = *y; s->nargs = 1;
    if (live(w)) *w->hole = s; /* else unreachable: lowered into a detached tree */
    *body = &s->a;
    return s;
}
static void jmp(Lw *w, LStmt *jp, int v) {
    if (!live(w)) return;
    LStmt *s = stmt(w, S_JMP); s->label = jp->label;
    s->args = A(w->L, sizeof *s->args); s->args[0] = v; s->nargs = 1;
    close_block(w, s);
}
static int lower_trap(Lw *w, Core *e, bool tail) {
    LStmt *jp = NULL; int y = -1; LStmt **after = NULL;
    if (!tail) { LStmt **body; jp = join(w, TY_OBJ, &y, &body); after = &jp->b; w->hole = body; }
    if (!live(w)) return -1;
    LStmt *t = stmt(w, S_TRAP);
    close_block(w, t);
    w->hole = &t->a;
    if (tail) lt(w, e->a[0]); else jmp(w, jp, lv(w, e->a[0]));
    w->hole = &t->b;
    int m = let(w, rhs(R_ERRMSG), "E");
    Core *h = e->a[1];
    if (h->k == CLAM) { /* (lambda Z H): bind Z to the message */
        w->map[h->var] = m;
        if (tail) lt(w, h->a[0]); else jmp(w, jp, lv(w, h->a[0]));
    } else {
        LRhs r = rhs(R_APPLY);
        int f = lv(w, h);
        r.args = A(w->L, 2 * sizeof *r.args); r.args[0] = f; r.args[1] = m; r.nargs = 2;
        int v = let(w, r, NULL);
        if (tail) { LStmt *s = stmt(w, S_RET); s->x = v; close_block(w, s); } else jmp(w, jp, v);
    }
    if (tail) { w->hole = NULL; return -1; }
    w->hole = after; return y;
}
static int lv(Lw *w, Core *e) {
    if (!live(w)) return -1;
    switch (e->k) {
    case CVAR: return w->map[e->var];
    case CLIT: { LRhs r = rhs(R_LIT); r.lit = e->lit; return let(w, r, NULL); }
    case CPRIM: case CKNOWN: case CGUARD: case CGLOBAL: case CAPPLY: return lower_call(w, e, false, false);
    case CIF: {
        int y; LStmt **body; LStmt *jp = join(w, TY_OBJ, &y, &body);
        w->hole = body;
        int c = lb(w, e->a[0], false);
        LStmt *s = stmt(w, S_IF); s->x = c;
        if (live(w)) { *w->hole = s;
            w->hole = &s->a; jmp(w, jp, lv(w, e->a[1]));
            w->hole = &s->b; jmp(w, jp, lv(w, e->a[2]));
        }
        w->hole = &jp->b; return y;
    }
    case CLET: {
        int v = lv(w, e->a[0]);
        w->map[e->var] = v;
        return lv(w, e->a[1]);
    }
    case CLAM: case CFRZ: return lower_closure(w, e);
    case CTRAP: return lower_trap(w, e, false);
    case CAND: case COR: {
        /* emit.c: left checked with "condition is not a boolean", right
         * through rc_require_boolean ("boolean expected"). */
        int a = lv(w, e->a[0]);
        if (!live(w)) return -1;
        LRhs t = rhs(R_TRUTH); t.args = A(w->L, sizeof *t.args); t.args[0] = a; t.nargs = 1; t.msg = cond_msg(false);
        int ta = let(w, t, NULL);
        int y; LStmt **body; LStmt *jp = join(w, TY_OBJ, &y, &body);
        w->hole = body;
        LStmt *s = stmt(w, S_IF); s->x = ta; *w->hole = s;
        if (ta < 0) { w->hole = NULL; return -1; }
        LStmt **more = e->k == CAND ? &s->a : &s->b, **done = e->k == CAND ? &s->b : &s->a;
        w->hole = more;
        int b = lv(w, e->a[1]);
        LRhs q = rhs(R_REQB); q.args = A(w->L, sizeof *q.args); q.args[0] = b; q.nargs = 1;
        jmp(w, jp, let(w, q, NULL));
        w->hole = done; jmp(w, jp, a);
        w->hole = &jp->b; return y;
    }
    case CERR: { LStmt *s = stmt(w, S_FAIL); s->msg = e->name; close_block(w, s); return -1; }
    case CDO: lv(w, e->a[0]); return lv(w, e->a[1]);
    }
    return -1;
}
static int lb(Lw *w, Core *e, bool rhs_boolean) {
    if (!live(w)) return -1;
    if (e->k == CLIT && (named(e->lit, "true") || named(e->lit, "false"))) {
        LRhs r = rhs(R_BOOL); r.idx = named(e->lit, "true"); return let(w, r, NULL);
    }
    if (e->k == CPRIM && prims[e->idx].test) return lower_call(w, e, true, rhs_boolean);
    if (e->k == CAND || e->k == COR) {
        int l = lb(w, e->a[0], false);
        if (!live(w)) return -1;
        int y; LStmt **body; LStmt *jp = join(w, TY_BOOL, &y, &body);
        w->hole = body;
        LStmt *s = stmt(w, S_IF); s->x = l;
        if (live(w)) {
            *w->hole = s;
            LStmt **more = e->k == CAND ? &s->a : &s->b, **done = e->k == CAND ? &s->b : &s->a;
            w->hole = more; jmp(w, jp, lb(w, e->a[1], true));
            w->hole = done; LRhs r = rhs(R_BOOL); r.idx = e->k == COR; jmp(w, jp, let(w, r, NULL));
        }
        w->hole = &jp->b; return y;
    }
    int v = lv(w, e);
    LRhs t = rhs(R_TRUTH); t.args = A(w->L, sizeof *t.args); t.args[0] = v; t.nargs = 1; t.msg = cond_msg(rhs_boolean);
    return let(w, t, NULL);
}
static void lt(Lw *w, Core *e) {
    if (!live(w)) return;
    switch (e->k) {
    case CIF: {
        int c = lb(w, e->a[0], false);
        if (!live(w)) return;
        LStmt *s = stmt(w, S_IF); s->x = c; close_block(w, s);
        w->hole = &s->a; lt(w, e->a[1]);
        w->hole = &s->b; lt(w, e->a[2]);
        w->hole = NULL; return;
    }
    case CLET: { int v = lv(w, e->a[0]); w->map[e->var] = v; lt(w, e->a[1]); return; }
    case CTRAP: lower_trap(w, e, true); return;
    case CERR: lv(w, e); return;
    case CDO: lv(w, e->a[0]); lt(w, e->a[1]); return;
    default: {
        int v = lv(w, e);
        if (!live(w)) return;
        LStmt *s = stmt(w, S_RET); s->x = v; close_block(w, s); return;
    }
    }
}
/* Core variables used in e but bound outside it, ascending (binding order). */
static void used_vars(const Core *e, unsigned char *used, unsigned char *bound) {
    if (e->k == CVAR) used[e->var] = 1;
    if (e->k == CLET || e->k == CLAM) bound[e->var] = 1;
    for (int i = 0; i < e->n; i++) used_vars(e->a[i], used, bound);
}
static void lower_fn(RcLean *L, int fn, Core *body, int *map);
static int lower_closure(Lw *w, Core *e) {
    RcLean *L = w->L; Unit *u = L->unit;
    unsigned char *used = A(L, (size_t)u->ncorevars + 1), *bound = A(L, (size_t)u->ncorevars + 1);
    used_vars(e->a[0], used, bound);
    if (e->k == CLAM) bound[e->var] = 1;
    char name[64]; snprintf(name, sizeof name, "%s/%s%d", FN(w)->name, e->k == CLAM ? "lambda" : "freeze", u->nfns);
    int fn = new_fn(L, dup(L, name), e->k == CLAM ? FN_LAMBDA : FN_FREEZE, w->fn);
    int *map = A(L, (size_t)(u->ncorevars + 1) * sizeof *map);
    for (int i = 0; i < u->ncorevars; i++) map[i] = -1;
    int ncaps = 0; for (int i = 0; i < u->ncorevars; i++) if (used[i] && !bound[i]) ncaps++;
    LRhs r = rhs(R_CLOS); r.idx = fn; r.nargs = ncaps;
    r.args = ncaps ? A(L, (size_t)ncaps * sizeof *r.args) : NULL;
    Lw inner = {L, fn, NULL, map};
    LFn *f = u->fns[fn];
    f->ncaps = ncaps; f->caps = ncaps ? A(L, (size_t)ncaps * sizeof *f->caps) : NULL;
    for (int i = 0, k = 0; i < u->ncorevars; i++) if (used[i] && !bound[i]) {
        r.args[k] = w->map[i];
        if (r.args[k] < 0) fail(L, false, "lean: unbound capture %s", u->corenames[i]);
        f = u->fns[fn];
        int v = nv(&inner, FN(w)->vars[w->map[i]].ty, u->corenames[i]);
        f = u->fns[fn]; f->caps[k++] = v; map[i] = v;
    }
    if (e->k == CLAM) {
        int p = nv(&inner, TY_OBJ, u->corenames[e->var]);
        f = u->fns[fn]; f->params = A(L, sizeof *f->params); f->params[0] = p; f->nparams = 1; map[e->var] = p;
    }
    lower_fn(L, fn, e->a[0], map);
    return let(w, r, NULL);
}
static void lower_fn(RcLean *L, int fn, Core *body, int *map) {
    LFn *f = L->unit->fns[fn];
    Lw w = {L, fn, &f->body, map};
    lt(&w, body);
}

/* ------------------------------------------------------------------ */
/* lir_check                                                           */

typedef struct { RcLean *L; const LFn *f; unsigned char *defined, *ever; int *jp_params; int *jp_n; unsigned char *jp_live; } Ck;
static _Noreturn void bad(Ck *c, const char *fmt, ...) {
    char m[384]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    fail(c->L, false, "lir_check %s: %s", c->f->name, m);
}
static void use(Ck *c, int v, LTy ty, const char *what) {
    if (v < 0 || v >= c->f->nvars) bad(c, "%s: invalid variable %d", what, v);
    if (!c->defined[v]) bad(c, "%s: %s used before definition", what, c->f->vars[v].hint ? c->f->vars[v].hint : "temporary");
    /* Rule 1: a kref never flows into an obj position (and no other mismatch). */
    if (c->f->vars[v].ty != ty) bad(c, "%s: variable %d has type %d, expected %d", what, v, c->f->vars[v].ty, ty);
}
static void define(Ck *c, int v) {
    if (v < 0 || v >= c->f->nvars) bad(c, "invalid definition %d", v);
    if (c->ever[v]) bad(c, "variable %d defined twice", v);
    c->defined[v] = c->ever[v] = 1;
}
static void check_rhs(Ck *c, const LStmt *s) {
    const LRhs *r = &s->r;
    /* Rule 2: every result that may fail is checked (carries !fail). */
    if (s->fails != rhs_fails(r)) bad(c, "let %d: fail check %s", s->x, s->fails ? "on an infallible value" : "missing");
    if (r->snap >= 0) {
        use(c, r->snap, TY_OBJ, "snapshot");
        if (r->k != R_PRIM && r->k != R_TEST && r->k != R_GCALL && r->k != R_CALL) bad(c, "snapshot on rhs kind %d", r->k);
    }
    switch (r->k) {
    case R_LIT: if (!r->lit) bad(c, "literal missing"); break;
    case R_PRIM: case R_TEST:
        if (r->idx < 0 || r->idx >= NPRIMS || prims[r->idx].arity != r->nargs) bad(c, "primitive %s arity", r->sym);
        if (r->k == R_TEST && (!prims[r->idx].test || !r->msg)) bad(c, "test on non-test primitive %s", r->sym);
        for (int i = 0; i < r->nargs; i++) use(c, r->args[i], TY_OBJ, r->sym);
        break;
    case R_TRUTH: case R_REQB: case R_BOX:
        if (r->nargs != 1) bad(c, "unary rhs arity");
        use(c, r->args[0], r->k == R_BOX ? TY_BOOL : TY_OBJ, "boolean check");
        if (r->k == R_TRUTH && !r->msg) bad(c, "truth without message");
        break;
    case R_BOOL: if (r->idx != 0 && r->idx != 1) bad(c, "boolean constant"); break;
    case R_SNAP: if (!r->sym) bad(c, "snapshot symbol"); break;
    case R_CALL:
        if (r->idx < 0 || (size_t)r->idx >= c->L->ndefs || c->L->defs[r->idx].arity != r->nargs || !c->L->defs[r->idx].lean)
            bad(c, "known call %s: not a lean defun of arity %d", r->sym, r->nargs);
        if (rc_lean_never_sealed(r->sym)) bad(c, "known call to never-sealed %s", r->sym);
        for (int i = 0; i < r->nargs; i++) use(c, r->args[i], TY_OBJ, r->sym);
        break;
    case R_GCALL: for (int i = 0; i < r->nargs; i++) use(c, r->args[i], TY_OBJ, r->sym); break;
    case R_APPLY:
        if (r->nargs < 1) bad(c, "apply without callee");
        for (int i = 0; i < r->nargs; i++) use(c, r->args[i], TY_OBJ, "apply");
        break;
    case R_CLOS: {
        const Unit *u = c->L->unit;
        if (r->idx < 0 || r->idx >= u->nfns || u->fns[r->idx]->ncaps != r->nargs) bad(c, "closure captures");
        for (int i = 0; i < r->nargs; i++) use(c, r->args[i], u->fns[r->idx]->vars[u->fns[r->idx]->caps[i]].ty, "capture");
        break;
    }
    case R_ERRMSG: bad(c, "error message outside a trap handler"); break;
    }
}
static void check_block(Ck *c, const LStmt *s, bool handler_entry);
static void check_sub(Ck *c, const LStmt *s, bool handler_entry) {
    /* Tree scoping: a sub-block's definitions are not visible after it
     * (ever keeps them reserved, so SSA still holds across blocks). */
    unsigned char *saved = A(c->L, (size_t)c->f->nvars + 1);
    memcpy(saved, c->defined, (size_t)c->f->nvars);
    check_block(c, s, handler_entry);
    memcpy(c->defined, saved, (size_t)c->f->nvars);
}
static void check_block(Ck *c, const LStmt *s, bool handler_entry) {
    for (; ; s = s->next) {
        if (!s) bad(c, "block falls off its end");
        switch (s->k) {
        case S_LET:
            if (s->r.k == R_ERRMSG) {
                if (!handler_entry) bad(c, "error message outside a trap handler entry");
                if (s->fails != rhs_fails(&s->r)) bad(c, "errmsg fail check");
            } else check_rhs(c, s);
            handler_entry = false;
            define(c, s->x);
            if (c->f->vars[s->x].ty != rhs_type(&s->r)) bad(c, "let %d type", s->x);
            continue;
        case S_IF:
            use(c, s->x, TY_BOOL, "if");
            check_sub(c, s->a, false); check_sub(c, s->b, false);
            return;
        case S_JP:
            if (s->label < 0 || s->label >= c->f->nlabels || c->jp_live[s->label]) bad(c, "join point %d", s->label);
            c->jp_live[s->label] = 1; c->jp_n[s->label] = s->nargs; c->jp_params[s->label] = s->nargs ? s->args[0] : -1;
            check_sub(c, s->a, false);
            c->jp_live[s->label] = 0;
            for (int i = 0; i < s->nargs; i++) define(c, s->args[i]);
            check_block(c, s->b, false);
            return;
        case S_JMP:
            if (s->label < 0 || s->label >= c->f->nlabels || !c->jp_live[s->label]) bad(c, "jump to label %d out of scope", s->label);
            if (s->nargs != c->jp_n[s->label]) bad(c, "jump arity");
            for (int i = 0; i < s->nargs; i++) use(c, s->args[i], c->f->vars[c->jp_params[s->label]].ty, "jump");
            return;
        case S_RET: use(c, s->x, TY_OBJ, "return"); return;
        case S_FAIL: if (!s->msg) bad(c, "fail without message"); return;
        case S_TRAP:
            check_sub(c, s->a, false); check_sub(c, s->b, true);
            return;
        }
        bad(c, "unknown statement");
    }
}
static void check_fn(RcLean *L, const LFn *f) {
    Ck c = {L, f, A(L, (size_t)f->nvars + 1), A(L, (size_t)f->nvars + 1), A(L, (size_t)(f->nlabels + 1) * sizeof(int)),
            A(L, (size_t)(f->nlabels + 1) * sizeof(int)), A(L, (size_t)f->nlabels + 1)};
    for (int i = 0; i < f->ncaps; i++) define(&c, f->caps[i]);
    for (int i = 0; i < f->nparams; i++) define(&c, f->params[i]);
    check_block(&c, f->body, false);
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */

static void print_lit(FILE *o, RcValue *x) {
    switch (rc_type(x)) {
    case RC_NIL: fputs("()", o); break;
    case RC_SYMBOL: fputs(sname(x), o); break;
    case RC_STRING: {
        fputc('"', o);
        for (const char *p = rc_text(x); *p; p++) {
            unsigned char ch = (unsigned char)*p;
            if (ch == '"' || ch == '\\') fprintf(o, "\\%c", ch);
            else if (ch < 32 || ch >= 127) fprintf(o, "\\%03o", ch);
            else fputc(ch, o);
        }
        fputc('"', o); break;
    }
    case RC_NUMBER:
        if (rc_number_is_exact_integer(x)) fprintf(o, "%" PRId64, rc_integer_value(x));
        else fprintf(o, "%.17g", rc_number_value(x));
        break;
    default: fputs("#<?>", o);
    }
}
static void print_core(FILE *o, const Unit *u, const Core *e) {
    static const char *const kinds[] = {"", "", "prim", "known", "guarded", "global", "apply"};
    switch (e->k) {
    case CVAR: fputs(u->corenames[e->var], o); return;
    case CLIT: if (is_sym(e->lit)) fputc('\'', o); print_lit(o, e->lit); return;
    case CPRIM: case CKNOWN: case CGUARD: case CGLOBAL:
        fprintf(o, "(%s %s", kinds[e->k], e->name);
        for (int i = 0; i < e->n; i++) { fputc(' ', o); print_core(o, u, e->a[i]); }
        fputc(')', o); return;
    case CAPPLY:
        fputs("(apply", o);
        for (int i = 0; i < e->n; i++) { fputc(' ', o); print_core(o, u, e->a[i]); }
        fputc(')', o); return;
    case CIF: case CAND: case COR: case CTRAP: case CDO:
        fprintf(o, "(%s", e->k == CIF ? "if" : e->k == CAND ? "and" : e->k == COR ? "or" : e->k == CDO ? "do" : "trap");
        for (int i = 0; i < e->n; i++) { fputc(' ', o); print_core(o, u, e->a[i]); }
        fputc(')', o); return;
    case CLET: fprintf(o, "(let %s ", u->corenames[e->var]); print_core(o, u, e->a[0]); fputc(' ', o); print_core(o, u, e->a[1]); fputc(')', o); return;
    case CLAM: fprintf(o, "(lambda %s ", u->corenames[e->var]); print_core(o, u, e->a[0]); fputc(')', o); return;
    case CFRZ: fputs("(freeze ", o); print_core(o, u, e->a[0]); fputc(')', o); return;
    case CERR: fprintf(o, "(error \"%s\")", e->name); return;
    }
}
static void pvar(FILE *o, const LFn *f, int v) {
    if (v < 0) { fputs("?", o); return; }
    if (f->vars[v].hint) fputs(f->vars[v].hint, o); else fprintf(o, "t%d", v);
}
static const char *tyname(LTy t) { return t == TY_OBJ ? "obj" : t == TY_BOOL ? "bool" : "kref"; }
static void pargs(FILE *o, const LFn *f, const int *a, int n) {
    fputc('(', o);
    for (int i = 0; i < n; i++) { if (i) fputs(", ", o); pvar(o, f, a[i]); }
    fputc(')', o);
}
static void indent(FILE *o, int d) { for (int i = 0; i < d; i++) fputs("  ", o); }
static void print_block(FILE *o, const Unit *u, const LFn *f, const LStmt *s, int d) {
    for (; s; s = s->next) {
        indent(o, d);
        switch (s->k) {
        case S_LET: {
            const LRhs *r = &s->r;
            fputs("let ", o); pvar(o, f, s->x); fprintf(o, ":%s = ", tyname(f->vars[s->x].ty));
            switch (r->k) {
            case R_LIT: fputs("lit ", o); print_lit(o, r->lit); break;
            case R_PRIM: fprintf(o, "prim %s", r->sym); pargs(o, f, r->args, r->nargs); break;
            case R_TEST: fprintf(o, "test %s", r->sym); pargs(o, f, r->args, r->nargs); break;
            case R_TRUTH: fputs("truth ", o); pvar(o, f, r->args[0]); break;
            case R_BOOL: fputs(r->idx ? "bool true" : "bool false", o); break;
            case R_BOX: fputs("box ", o); pvar(o, f, r->args[0]); break;
            case R_REQB: fputs("reqbool ", o); pvar(o, f, r->args[0]); break;
            case R_SNAP: fprintf(o, "snap %s", r->sym); break;
            case R_CALL: fprintf(o, "call %s", r->sym); pargs(o, f, r->args, r->nargs); break;
            case R_GCALL: fprintf(o, "gcall %s", r->sym); pargs(o, f, r->args, r->nargs); break;
            case R_APPLY: fputs("apply ", o); pvar(o, f, r->args[0]); pargs(o, f, r->args + 1, r->nargs - 1); break;
            case R_CLOS: fprintf(o, "closure %s", u->fns[r->idx]->name); pargs(o, f, r->args, r->nargs); break;
            case R_ERRMSG: fputs("errmsg", o); break;
            }
            if (r->snap >= 0) { fputs(" @", o); pvar(o, f, r->snap); }
            if (s->fails) fputs(" !fail", o);
            if (r->msg) fprintf(o, " \"%s\"", r->msg);
            fputc('\n', o);
            continue;
        }
        case S_IF:
            fputs("if ", o); pvar(o, f, s->x); fputs(" then\n", o);
            print_block(o, u, f, s->a, d + 1);
            indent(o, d); fputs("else\n", o);
            print_block(o, u, f, s->b, d + 1);
            return;
        case S_JP:
            fprintf(o, "jp j%d", s->label); pargs(o, f, s->args, s->nargs);
            fprintf(o, ":%s =\n", s->nargs ? tyname(f->vars[s->args[0]].ty) : "");
            print_block(o, u, f, s->b, d + 1);
            indent(o, d); fputs("in\n", o);
            print_block(o, u, f, s->a, d + 1);
            return;
        case S_JMP: fprintf(o, "jmp j%d", s->label); pargs(o, f, s->args, s->nargs); fputc('\n', o); return;
        case S_RET: fputs("ret ", o); pvar(o, f, s->x); fputc('\n', o); return;
        case S_FAIL: fprintf(o, "fail \"%s\"\n", s->msg); return;
        case S_TRAP:
            fputs("trap\n", o); print_block(o, u, f, s->a, d + 1);
            indent(o, d); fputs("handler\n", o); print_block(o, u, f, s->b, d + 1);
            return;
        }
    }
}
static void print_fn(FILE *o, const Unit *u, const LFn *f) {
    static const char *const kinds[] = {"fn", "lambda", "freeze"};
    fprintf(o, "%s %s", kinds[f->kind], f->name);
    if (f->ncaps) { fputs(" {", o); for (int i = 0; i < f->ncaps; i++) { if (i) fputs(", ", o); pvar(o, f, f->caps[i]); fprintf(o, ":%s", tyname(f->vars[f->caps[i]].ty)); } fputc('}', o); }
    fputc('(', o);
    for (int i = 0; i < f->nparams; i++) { if (i) fputs(", ", o); pvar(o, f, f->params[i]); fprintf(o, ":%s@own", tyname(f->vars[f->params[i]].ty)); }
    fputs(") : obj@own\n", o);
    print_block(o, u, f, f->body, 1);
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */

/* Resolve, lower and check one defun into L->unit. */
static void compile_def(RcLean *L, int d) {
    const KDef *k = &L->defs[d];
    Unit *u = A(L, sizeof *u); L->unit = u;
    Scope *sc = NULL;
    int arity = k->arity, *pv = arity ? A(L, (size_t)arity * sizeof *pv) : NULL;
    RcValue *p = k->params;
    for (int i = 0; i < arity; i++, p = tl(p)) {
        if (!is_sym(hd(p))) fail(L, true, "parameter is not a symbol");
        for (Scope *s = sc; s; s = (Scope *)s->up) if (!strcmp(s->name, sname(hd(p)))) fail(L, true, "duplicate parameter");
        Scope *s = A(L, sizeof *s);
        pv[i] = new_corevar(L, sname(hd(p)));
        *s = (Scope){sname(hd(p)), pv[i], sc}; sc = s;
    }
    u->core = resolve(L, k->body, sc);
    int fn = new_fn(L, k->name, FN_DEFUN, -1);
    int *map = A(L, (size_t)(u->ncorevars + 1) * sizeof *map);
    for (int i = 0; i < u->ncorevars; i++) map[i] = -1;
    Lw w = {L, fn, NULL, map};
    LFn *f = u->fns[fn];
    f->nparams = arity; f->params = pv ? A(L, (size_t)arity * sizeof *f->params) : NULL;
    for (int i = 0; i < arity; i++) {
        int v = nv(&w, TY_OBJ, u->corenames[pv[i]]);
        f = u->fns[fn]; f->params[i] = v; map[pv[i]] = v;
    }
    lower_fn(L, fn, u->core, map);
    for (int i = 0; i < u->nfns; i++) check_fn(L, u->fns[i]);
}

RcLean *rc_lean_new(RcContext *ctx, RcValue *const *forms, size_t nforms,
                    const char *const *lean_set, size_t nset) {
    RcLean *L = calloc(1, sizeof *L);
    if (!L) { rc_error(ctx, "lean: out of memory"); return NULL; }
    L->ctx = ctx;
    if (setjmp(L->fail)) { rc_error(ctx, "%s", L->error); rc_lean_free(L); return NULL; }
    size_t n = 0;
    for (size_t i = 0; i < nforms; i++) if (is_pair(forms[i]) && named(hd(forms[i]), "defun")) n++;
    L->defs = A(L, (n ? n : 1) * sizeof *L->defs);
    for (L->tcap = 16; L->tcap < 2 * n + 2; L->tcap *= 2) {}
    L->table = A(L, L->tcap * sizeof *L->table);
    for (size_t i = 0; i < nforms; i++) {
        RcValue *x = forms[i];
        if (!is_pair(x) || !named(hd(x), "defun")) continue;
        if (length(L, tl(x)) != 3 || !is_sym(nth(x, 1))) fail(L, false, "lean: malformed kernel defun");
        const char *name = sname(nth(x, 1));
        int d = lookup_def(L, name);
        if (d < 0) { d = (int)L->ndefs++;
            for (size_t j = hash(name) & (L->tcap - 1);; j = (j + 1) & (L->tcap - 1)) if (!L->table[j]) { L->table[j] = d + 1; break; } }
        /* A later kernel definition of the same name wins, as at bootstrap. */
        L->defs[d] = (KDef){name, nth(x, 2), nth(x, 3), length(L, nth(x, 2)), false};
    }
    int dd = lookup_def(L, "do");
    if (dd >= 0) {
        RcValue *ps = L->defs[dd].params, *b = L->defs[dd].body;
        L->do_identity = L->defs[dd].arity == 2 && is_sym(hd(ps)) && is_sym(nth(ps, 1)) &&
                         strcmp(sname(hd(ps)), sname(nth(ps, 1))) && is_sym(b) && !strcmp(sname(b), sname(nth(ps, 1)));
    }
    for (size_t d = 0; d < L->ndefs; d++) {
        const char *name = L->defs[d].name;
        int p = prim_index(name);
        bool eligible = !rc_lean_never_sealed(name) && !(p >= 0 && prims[p].overwrite);
        L->defs[d].lean = eligible && (!lean_set || in_list(lean_set, nset, name));
    }
    if (lean_set) for (size_t i = 0; i < nset; i++) {
        int d = lookup_def(L, lean_set[i]), p = prim_index(lean_set[i]);
        if (rc_lean_never_sealed(lean_set[i]) || (p >= 0 && prims[p].overwrite))
            fail(L, false, "lean set: %s is on the never-seal list", lean_set[i]);
        if (d < 0) fail(L, false, "lean set: %s is not a kernel defun", lean_set[i]);
    }
    return L;
}
void rc_lean_free(RcLean *L) {
    if (!L) return;
    while (L->mem) { Chunk *n = L->mem->next; free(L->mem); L->mem = n; }
    free(L);
}
bool rc_lean_dump(RcLean *L, const char *name, FILE *out) {
    int d = lookup_def(L, name);
    if (d < 0) { rc_error(L->ctx, "lean: %s is not a kernel defun", name); return false; }
    if (setjmp(L->fail)) { rc_error(L->ctx, "lean: %s: %s", name, L->error); return false; }
    compile_def(L, d);
    const KDef *k = &L->defs[d];
    fprintf(out, "core %s%s", k->name, k->lean ? "" : " [not lean]");
    fputs(" (", out);
    for (int i = 0; i < k->arity; i++) fprintf(out, "%s%s", i ? " " : "", L->unit->corenames[i]);
    fputs(") =\n  ", out);
    print_core(out, L->unit, L->unit->core);
    fputs("\n", out);
    for (int i = 0; i < L->unit->nfns; i++) print_fn(out, L->unit, L->unit->fns[i]);
    return true;
}
/* lir_check must reject each of these malformed functions. */
static bool check_rejects(RcLean *L, FILE *out, int variant) {
    Unit *u = A(L, sizeof *u); L->unit = u;
    int fn = new_fn(L, "lean-selftest", FN_DEFUN, -1);
    Lw w = {L, fn, NULL, NULL};
    LFn *f = u->fns[fn];
    int p = nv(&w, variant == 0 ? TY_KREF : TY_OBJ, "P");
    f->params = A(L, sizeof *f->params); f->params[0] = p; f->nparams = 1;
    w.hole = &f->body;
    LStmt *s;
    switch (variant) {
    case 0: /* rule 1: a kref returned as obj */
        s = stmt(&w, S_RET); s->x = p; close_block(&w, s); break;
    case 1: { /* rule 2: a fallible value without its check */
        LRhs r = rhs(R_GCALL); r.sym = "f"; r.args = A(L, sizeof(int)); r.args[0] = p; r.nargs = 1;
        int v = let(&w, r, NULL); f->body->fails = false;
        s = stmt(&w, S_RET); s->x = v; close_block(&w, s); break; }
    case 2: /* use before definition */
        s = stmt(&w, S_RET); s->x = nv(&w, TY_OBJ, "Q"); close_block(&w, s); break;
    case 3: { /* jump to a label that is not in scope */
        f->nlabels = 1; s = stmt(&w, S_JMP); s->label = 0; s->args = A(L, sizeof(int)); s->args[0] = p; s->nargs = 1;
        close_block(&w, s); break; }
    case 4: { /* if on an obj */
        s = stmt(&w, S_IF); s->x = p; close_block(&w, s);
        LStmt *r1 = stmt(&w, S_RET); r1->x = p; s->a = r1;
        LStmt *r2 = stmt(&w, S_RET); r2->x = p; s->b = r2; break; }
    case 5: { /* a block that falls off its end */
        LRhs r = rhs(R_BOOL); r.idx = 1; let(&w, r, NULL); break; }
    case 6: { /* a join-point parameter used inside its own body */
        LStmt *jp = stmt(&w, S_JP); jp->label = f->nlabels++;
        int y = nv(&w, TY_OBJ, "Y"); jp->args = A(L, sizeof(int)); jp->args[0] = y; jp->nargs = 1;
        close_block(&w, jp);
        LStmt *r1 = stmt(&w, S_RET); r1->x = y; jp->b = r1;
        LStmt *r2 = stmt(&w, S_RET); r2->x = y; jp->a = r2; break; }
    default: return false;
    }
    if (setjmp(L->fail)) { fprintf(out, "selftest %d rejected: %s\n", variant, L->error); return true; }
    check_fn(L, u->fns[fn]);
    fprintf(out, "FAIL lir_check accepted malformed function %d\n", variant);
    return false;
}
bool rc_lean_check_all(RcLean *L, FILE *out) {
    size_t ok = 0, rejected = 0, failed = 0, fns = 0;
    for (int v = 0; v < 7; v++) if (!check_rejects(L, out, v)) failed++;
    L->sites_prim = L->sites_known = L->sites_guard = L->sites_global = L->sites_apply = L->sites_do = 0;
    for (size_t d = 0; d < L->ndefs; d++) {
        if (setjmp(L->fail)) {
            if (L->rejected) { rejected++; fprintf(out, "rejected %s: %s\n", L->defs[d].name, L->error); }
            else { failed++; fprintf(out, "FAIL %s: %s\n", L->defs[d].name, L->error); }
            continue;
        }
        compile_def(L, (int)d);
        ok++; fns += (size_t)L->unit->nfns;
    }
    /* Seal list: no never-sealed name is a lean (Known) target. */
    size_t sealed = 0;
    for (size_t d = 0; d < L->ndefs; d++) {
        if (L->defs[d].lean && rc_lean_never_sealed(L->defs[d].name)) { failed++; fprintf(out, "FAIL seal list: %s is lean\n", L->defs[d].name); }
        if (L->defs[d].lean) sealed++;
    }
    fprintf(out, "lean check: lir_check rejects 7 malformed functions; %zu kernel defuns, %zu lowered and checked (%zu LIR functions), %zu rejected, %zu failed; "
            "%zu lean targets; call sites: %ld prim, %ld known, %ld guarded, %ld global, %ld apply, %ld do\n",
            L->ndefs, ok, fns, rejected, failed, sealed, L->sites_prim, L->sites_known, L->sites_guard, L->sites_global, L->sites_apply, L->sites_do);
    return failed == 0;
}

/* ------------------------------------------------------------------ */
/* Generated kernel: prelude and bootstrap                             */

/* The symbols whose bindings guard lean code: every primitive (seal-list
 * (d), (e), (f)) and the guarded kernel defuns. Indices are stable within
 * one generated kernel. */
bool rc_lean_emit_prelude(RcLean *L, FILE *out) {
    fputs("/* Lean C (RC_LEAN=1): runtime support and the guard symbol table. */\n"
          "#include \"lean_rt.h\"\n"
          "static const char *const rc_lean_symbol_names[]={\n", out);
    int n = 0;
    for (int i = 0; i < NPRIMS; i++, n++) { if (n) fputs(",\n", out); fprintf(out, "  \"%s\"", prims[i].name); }
    for (size_t i = 0; i < sizeof guarded_defuns / sizeof *guarded_defuns; i++, n++) fprintf(out, ",\n  \"%s\"", guarded_defuns[i]);
    fprintf(out, "\n};\nstatic const RcSymbolTable rc_lean_symbols={rc_lean_symbol_names,%d};\n", n);
    (void)L;
    return !ferror(out);
}
bool rc_lean_emit_bootstrap(RcLean *L, FILE *out) {
    (void)L;
    fputs("  if(!rc_lean_install(ctx,&rc_lean_symbols)) return false;\n", out);
    return !ferror(out);
}
