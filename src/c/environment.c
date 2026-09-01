#include "environment.h"
#include "gc.h"

#include <stdlib.h>
#include <string.h>

Environment* global_function_environment = NULL;
Environment* global_variable_environment = NULL;

typedef struct LexUndo {
  uint32_t id;
  uint32_t gen;
  KLObject* prev;
} LexUndo;

static __thread uint32_t lex_epoch = 0;
static __thread uint32_t lex_cap = 0;
static __thread uint32_t* lex_gen = NULL;
static __thread KLObject** lex_slots = NULL;
static __thread uint32_t lex_undo_sp = 0;
static __thread uint32_t lex_undo_cap = 0;
static __thread LexUndo* lex_undo = NULL;

static void lex_ensure (uint32_t id)
{
  uint32_t cap;
  uint32_t* gens;
  KLObject** slots;

  if (id < lex_cap)
    return;

  cap = lex_cap == 0 ? 256 : lex_cap;

  while (cap <= id) {
    if (cap > (UINT32_MAX / 2)) {
      cap = id + 1;
      break;
    }

    cap *= 2;
  }

  if (lex_slots != NULL)
    GC_remove_roots((char*)lex_slots, (char*)(lex_slots + lex_cap));

#pragma push_macro("realloc")
#undef realloc
  gens = realloc(lex_gen, (size_t)cap * sizeof(uint32_t));
  slots = realloc(lex_slots, (size_t)cap * sizeof(KLObject*));
#pragma pop_macro("realloc")

  if (gens == NULL || slots == NULL)
    throw_kl_exception("Failed to grow lexical slots");

  if (cap > lex_cap) {
    memset(gens + lex_cap, 0, (size_t)(cap - lex_cap) * sizeof(uint32_t));
    memset(slots + lex_cap, 0, (size_t)(cap - lex_cap) * sizeof(KLObject*));
  }

  lex_gen = gens;
  lex_slots = slots;
  GC_add_roots((char*)lex_slots, (char*)(lex_slots + cap));
  lex_cap = cap;
}

static void lex_ensure_undo (void)
{
  uint32_t cap;
  LexUndo* undo;

  if (lex_undo_sp < lex_undo_cap)
    return;

  cap = lex_undo_cap == 0 ? 64 : lex_undo_cap * 2;

  if (lex_undo != NULL)
    GC_remove_roots((char*)lex_undo, (char*)(lex_undo + lex_undo_cap));

#pragma push_macro("realloc")
#undef realloc
  undo = realloc(lex_undo, (size_t)cap * sizeof(LexUndo));
#pragma pop_macro("realloc")

  if (undo == NULL)
    throw_kl_exception("Failed to grow lexical undo");

  if (cap > lex_undo_cap)
    memset(undo + lex_undo_cap, 0, (size_t)(cap - lex_undo_cap) * sizeof(LexUndo));

  lex_undo = undo;
  GC_add_roots((char*)lex_undo, (char*)(lex_undo + cap));
  lex_undo_cap = cap;
}

ShenLexMark shen_lex_mark (void)
{
  ShenLexMark mark;

  mark.epoch = lex_epoch;
  mark.undo_sp = lex_undo_sp;

  return mark;
}

void shen_lex_rewind (ShenLexMark mark)
{
  while (lex_undo_sp > mark.undo_sp) {
    LexUndo u;

    lex_undo_sp--;
    u = lex_undo[lex_undo_sp];

    if (u.id < lex_cap) {
      lex_slots[u.id] = u.prev;
      lex_gen[u.id] = u.gen;
    }
  }

  lex_epoch = mark.epoch;
}

void shen_lex_enter_frame (void)
{
  lex_epoch++;

  if (lex_epoch == 0)
    lex_epoch = 1;
}

void shen_lex_bind (KLObject* symbol_object, KLObject* value)
{
  uint32_t id;
  LexUndo* u;

  if (is_null(symbol_object) || !is_kl_symbol(symbol_object))
    throw_kl_exception("lexical bind expects a symbol");

  id = get_kl_symbol_id(symbol_object);
  lex_ensure(id);
  lex_ensure_undo();
  u = &lex_undo[lex_undo_sp++];
  u->id = id;
  u->gen = lex_gen[id];
  u->prev = lex_slots[id];
  lex_slots[id] = value;
  lex_gen[id] = lex_epoch == 0 ? 1 : lex_epoch;

  if (lex_epoch == 0)
    lex_epoch = 1;
}

void shen_lex_assign (KLObject* symbol_object, KLObject* value)
{
  uint32_t id;

  if (is_null(symbol_object) || !is_kl_symbol(symbol_object))
    throw_kl_exception("lexical assign expects a symbol");

  id = get_kl_symbol_id(symbol_object);
  lex_ensure(id);
  lex_slots[id] = value;
  lex_gen[id] = lex_epoch == 0 ? 1 : lex_epoch;

  if (lex_epoch == 0)
    lex_epoch = 1;
}

static KLObject* lookup_environment_walk (uint32_t id, Environment* environment)
{
  while (environment != NULL) {
    uint32_t n = environment->size;

    if (n == 1) {
      if (environment->id == id)
        return environment->value;
    } else if (n > 1) {
      EnvBinding* binds = environment->binds;
      uint32_t i;

      for (i = n; i-- > 0; ) {
        if (binds[i].id == id)
          return binds[i].value;
      }
    }

    environment = environment->parent;
  }

  return NULL;
}

KLObject* lookup_environment (KLObject* symbol_object, Environment* environment)
{
  uint32_t id;

  if (is_null(symbol_object) || !is_kl_symbol(symbol_object))
    return NULL;

  id = get_kl_symbol_id(symbol_object);

  if (lex_epoch != 0 && id < lex_cap && lex_gen[id] == lex_epoch)
    return lex_slots[id];

  return lookup_environment_walk(id, environment);
}

extern Environment* get_parent_environment (Environment* environment);
extern void set_parent_environment (Environment* environment,
                                    Environment* parent_environment);
extern Environment* create_environment (void);
extern void initialize_global_environments (void);
extern Environment* get_global_function_environment (void);
extern Environment* get_global_variable_environment (void);
extern Environment* extend_environment (KLObject* symbol_object, KLObject* object,
                                        Environment* parent);
extern Environment* extend_environment_n (KLObject** symbols, KLObject** values,
                                          long n, Environment* parent);
extern void update_environment (KLObject* symbol_object, KLObject* object,
                                Environment* environment);
