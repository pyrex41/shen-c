#ifndef SHEN_C_ENVIRONMENT_H
#define SHEN_C_ENVIRONMENT_H

#include "exception.h"
#include "kl.h"
#include "symbol.h"

extern Environment* global_function_environment;
extern Environment* global_variable_environment;

typedef struct ShenLexMark {
  uint32_t epoch;
  uint32_t undo_sp;
} ShenLexMark;

ShenLexMark shen_lex_mark (void);
void shen_lex_rewind (ShenLexMark mark);
void shen_lex_enter_frame (void);
void shen_lex_bind (KLObject* symbol_object, KLObject* value);
void shen_lex_assign (KLObject* symbol_object, KLObject* value);

inline Environment* get_parent_environment (Environment* environment)
{
  return environment->parent;
}

inline void set_parent_environment (Environment* environment,
                                    Environment* parent_environment)
{
  environment->parent = parent_environment;
}

inline Environment* create_environment (void)
{
  Environment* environment = malloc(sizeof(Environment));

  environment->parent = NULL;
  environment->size = 0;
  environment->id = 0;
  environment->value = NULL;
  environment->binds = NULL;

  return environment;
}

inline void initialize_global_environments (void)
{
  global_function_environment = create_environment();
  global_variable_environment = create_environment();
}

inline Environment* get_global_function_environment (void)
{
  return global_function_environment;
}

inline Environment* get_global_variable_environment (void)
{
  return global_variable_environment;
}

inline Environment* extend_environment (KLObject* symbol_object, KLObject* object,
                                        Environment* parent)
{
  Environment* environment = malloc(sizeof(Environment));

  environment->parent = parent;
  environment->size = 1;
  environment->id = get_kl_symbol_id(symbol_object);
  environment->value = object;
  environment->binds = NULL;

  return environment;
}

inline Environment* extend_environment_n (KLObject** symbols, KLObject** values,
                                          long n, Environment* parent)
{
  Environment* environment;
  EnvBinding* binds;
  long i;

  if (n <= 0)
    return parent;

  if (n == 1)
    return extend_environment(symbols[0], values[0], parent);

  binds = malloc((size_t)n * sizeof(EnvBinding));
  environment = malloc(sizeof(Environment));
  environment->parent = parent;
  environment->size = (uint32_t)n;
  environment->id = 0;
  environment->value = NULL;
  environment->binds = binds;

  for (i = 0; i < n; ++i) {
    binds[i].id = get_kl_symbol_id(symbols[i]);
    binds[i].value = values[i];
  }

  return environment;
}

inline void update_environment (KLObject* symbol_object, KLObject* object,
                                Environment* environment)
{
  uint32_t id = get_kl_symbol_id(symbol_object);

  while (environment != NULL) {
    uint32_t n = environment->size;

    if (n == 1) {
      if (environment->id == id) {
        environment->value = object;
        return;
      }
    } else if (n > 1) {
      EnvBinding* binds = environment->binds;
      uint32_t i;

      for (i = n; i-- > 0; ) {
        if (binds[i].id == id) {
          binds[i].value = object;
          return;
        }
      }
    }

    environment = environment->parent;
  }

  throw_kl_exception("Failed to extend environment");
}

KLObject* lookup_environment (KLObject* symbol_object,
                              Environment* environment);

#endif
