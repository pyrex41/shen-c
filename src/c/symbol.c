#include "symbol.h"

unsigned long auto_increment_symbol_name_id = 0;
char* auto_increment_symbol_name_prefix = "G__";
khash_t(SymbolTable)* symbol_table;
uint32_t next_symbol_id = 1;
uint32_t global_symbol_capacity = 0;
KLObject** global_function_table = NULL;
KLObject** global_variable_table = NULL;

void grow_global_symbol_tables (uint32_t min_capacity)
{
  uint32_t old_cap = global_symbol_capacity;
  uint32_t cap = old_cap == 0 ? 256 : old_cap;
  KLObject** functions;
  KLObject** variables;

  while (cap < min_capacity) {
    if (cap > (UINT32_MAX / 2)) {
      cap = min_capacity;
      break;
    }

    cap *= 2;
  }

  functions = realloc(global_function_table, (size_t)cap * sizeof(KLObject*));
  if (functions == NULL)
    throw_kl_exception("Failed to grow function table");

  global_function_table = functions;

  variables = realloc(global_variable_table, (size_t)cap * sizeof(KLObject*));
  if (variables == NULL)
    throw_kl_exception("Failed to grow variable table");

  global_variable_table = variables;

  if (cap > old_cap) {
    memset(functions + old_cap, 0, (size_t)(cap - old_cap) * sizeof(KLObject*));
    memset(variables + old_cap, 0, (size_t)(cap - old_cap) * sizeof(KLObject*));
  }

  global_symbol_capacity = cap;
}

extern uint32_t mint_symbol_id (void);
extern uint32_t get_kl_symbol_id (KLObject* symbol_object);
extern KLObject* get_symbol_name (Symbol* symbol);
extern void set_symbol_name (Symbol* symbol, KLObject* string_object);
extern KLObject* get_symbol_function (Symbol* symbol);
extern void set_symbol_function (Symbol* symbol, KLObject* function_object);
extern KLObject* get_symbol_variable_value (Symbol* symbol);
extern void set_symbol_variable_value (Symbol* symbol,
                                       KLObject* variable_value_object);
extern Symbol* create_symbol (KLObject* string_object);

extern Symbol* get_symbol (KLObject* symbol_object);
extern void set_symbol (KLObject* symbol_object, Symbol* symbol);
extern KLObject* create_kl_symbol (KLObject* string_object);
extern KLObject* create_kl_symbol_by_name (char* symbol_name);
extern KLObject* get_kl_symbol_name (KLObject* symbol_object);
extern void set_kl_symbol_name (KLObject* symbol_object, KLObject* string_object);
extern KLObject* get_kl_symbol_function (KLObject* symbol_object);
extern void set_kl_symbol_function (KLObject* symbol_object,
                                    KLObject* function_object);
extern KLObject* get_kl_symbol_variable_value (KLObject* symbol_object);
extern void set_kl_symbol_variable_value (KLObject* symbol_object,
                                          KLObject* variable_value_object);
extern bool is_kl_symbol (KLObject* object);
extern bool is_symbol_equal (Symbol* left_symbol, Symbol* right_symbol);
extern bool is_kl_symbol_equal (KLObject* left_object, KLObject* right_object);

extern unsigned long get_auto_increment_symbol_name_id (void);
extern unsigned long pre_increment_auto_increment_symbol_name_id (void);
extern char* get_auto_increment_symbol_name_prefix (void);
extern char* create_auto_increment_symbol_name (void);
extern KLObject* create_auto_increment_kl_symbol (void);

extern khash_t(SymbolTable)* get_symbol_table (void);
extern void initialize_symbol_table (void);
extern KLObject* lookup_symbol_table (KLObject* string_object);
extern void extend_symbol_table (KLObject* string_object, KLObject* object);
