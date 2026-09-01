#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi.h"
#include "environment.h"
#include "init.h"
#include "number.h"
#include "object.h"
#include "reader.h"
#include "repl.h"
#include "symbol.h"
#include "symbol_pool.h"
#include "variable.h"
#include "version.h"

static int failures = 0;

static void expect (int condition, const char* message)
{
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

static void test_gc_heap (void)
{
  KLObject* object = create_kl_number_l(7);
  void* block = shen_gc_malloc(&shen_root_context, 64);

  expect(shen_root_context.gc_ready == 1, "shen_context GC is initialized");
  expect(object != NULL, "KLObject allocation");
  expect(GC_base(object) != NULL, "KLObject lives on Boehm GC heap");
  expect(block != NULL, "shen_gc_malloc");
  expect(GC_base(block) != NULL, "shen_gc_malloc lives on Boehm GC heap");
}

static void test_version (void)
{
  expect(strcmp(SHEN_C_VERSION, "0.2.3") == 0, "SHEN_C_VERSION");
}

static void test_numbers_and_lists (void)
{
  KLObject* one = create_kl_number_l(1);
  KLObject* two = create_kl_number_l(2);
  KLObject* sum = add_kl_number(one, two);
  KLObject* list = CONS(one, CONS(two, EL));

  expect(is_kl_number_l(sum), "sum is a long");
  expect(get_kl_number_number_l(sum) == 3, "1 + 2 == 3");
  expect(is_non_empty_kl_list(list), "list is non-empty");
  expect(get_kl_list_size(list) == 2, "list size is 2");
  expect(CAR(list) == one, "list head");
  expect(CDR(list) != EL && CAR(CDR(list)) == two, "list tail head");
  expect(GC_base(list) == list, "cons cell is a GC object");
  expect(GC_base(get_pair(list)) == list,
         "cons pair is interior to the cons cell");
  expect(GC_base(one) == one, "number is a GC object");
  expect(GC_base(get_number(one)) == one,
         "number payload is interior to the number object");
}

static void test_type_special_form (void)
{
  KLObject* list_ctor = create_kl_symbol_by_name("list");
  KLObject* ascription =
    CONS(list_ctor, CONS(create_kl_symbol_by_name("symbol"), EL));
  KLObject* expr =
    CONS(get_type_symbol_object(),
         CONS(create_kl_number_l(42), CONS(ascription, EL)));
  KLObject* result = eval_kl_object(expr,
                                    get_global_function_environment(),
                                    get_global_variable_environment());

  expect(is_kl_number_l(result), "type returns the value");
  expect(get_kl_number_number_l(result) == 42,
         "(type 42 (list symbol)) == 42 without applying list");
}

static void test_primitive_add (void)
{
  KLObject* expr = CONS(get_add_symbol_object(),
                        CONS(create_kl_number_l(40),
                             CONS(create_kl_number_l(2), EL)));
  KLObject* result = eval_kl_object(expr,
                                    get_global_function_environment(),
                                    get_global_variable_environment());

  expect(is_kl_number_l(result), "eval + returns a long");
  expect(get_kl_number_number_l(result) == 42, "eval (+ 40 2) == 42");
}

static void test_reader_eof (void)
{
  FILE* file = tmpfile();
  KLObject* stream;
  KLObject* object;

  expect(file != NULL, "tmpfile for reader EOF");
  if (file == NULL)
    return;

  stream = create_std_kl_stream(file, get_in_symbol_object());
  object = read_string(stream);
  expect(is_null(object), "reader returns NULL on EOF");
  fclose(file);
}

static void test_argv (void)
{
  char* argv[] = { "shen-c", "script", "demo.kl" };
  KLObject* list;
  KLObject* first;

  set_command_line_arguments(3, argv);
  list = get_kl_symbol_variable_value(get_earmuff_argv_symbol_object());
  expect(get_kl_list_size(list) == 3, "*argv* size");
  first = CAR(list);
  expect(is_kl_string(first), "*argv* head is a string");
  expect(strcmp(get_string(first), "shen-c") == 0, "*argv* program name");
}

static void test_dense_env (void)
{
  KLObject* a = shen_intern(&shen_root_context, "dense-a");
  KLObject* b = shen_intern(&shen_root_context, "dense-b");
  KLObject* c = shen_intern(&shen_root_context, "dense-c");
  KLObject* a2 = shen_intern(&shen_root_context, "dense-a");
  KLObject* one = create_kl_number_l(1);
  KLObject* two = create_kl_number_l(2);
  KLObject* three = create_kl_number_l(3);
  KLObject* symbols[3];
  KLObject* values[3];
  Environment* env1;
  Environment* envn;
  Environment* nested;

  expect(a == a2, "intern same name is identical");
  expect(get_kl_symbol_id(a) == get_kl_symbol_id(a2), "intern same name same id");
  expect(get_kl_symbol_id(a) != get_kl_symbol_id(b),
         "distinct interned names have distinct ids");
  expect(get_kl_symbol_id(a) != 0, "symbol ids are dense and non-zero");
  expect(get_kl_symbol_id(a) < global_symbol_capacity, "id fits function table");

  env1 = extend_environment(a, one, get_global_variable_environment());
  expect(lookup_environment(a, env1) == one, "size-1 lookup");
  expect(lookup_environment(b, env1) == NULL, "size-1 miss walks parent");

  symbols[0] = a;
  symbols[1] = b;
  symbols[2] = c;
  values[0] = one;
  values[1] = two;
  values[2] = three;
  envn = extend_environment_n(symbols, values, 3, env1);
  expect(lookup_environment(c, envn) == three, "n-ary lookup");
  expect(lookup_environment(a, envn) == one, "n-ary inner binding");
  update_environment(b, three, envn);
  expect(lookup_environment(b, envn) == three, "n-ary update");

  nested = extend_environment(b, two, envn);
  expect(lookup_environment(b, nested) == two, "child shadows parent");
  expect(lookup_environment(c, nested) == three, "lookup walks lexical parent");
  expect(get_parent_environment(nested) == envn, "lexical parent retained");

  {
    ShenLexMark mark = shen_lex_mark();

    shen_lex_enter_frame();
    shen_lex_bind(a, one);
    expect(lookup_environment(a, envn) == one, "lex slot hits bound id");
    shen_lex_bind(a, two);
    expect(lookup_environment(a, envn) == two, "lex slot shadows");
    shen_lex_rewind(mark);
    expect(lookup_environment(a, env1) == one,
           "rewind restores environment walk");
  }

  set_kl_symbol_function(a, get_kl_symbol_function(get_add_symbol_object()));
  expect(get_kl_symbol_function(a) != NULL, "function table slot");
  expect(get_kl_symbol_variable_value(get_earmuff_language_symbol_object()) != NULL,
         "variable table slot");
  set_kl_symbol_function(a, NULL);
}

int main (void)
{
  shen_context_init(&shen_root_context);
  initialize_runtime(".");

  test_gc_heap();
  test_version();
  test_numbers_and_lists();
  test_type_special_form();
  test_primitive_add();
  test_reader_eof();
  test_argv();
  test_dense_env();

  if (failures != 0) {
    fprintf(stderr, "%d foundation test(s) failed\n", failures);
    return 1;
  }

  printf("foundation tests ok\n");
  return 0;
}
