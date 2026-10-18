#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "repl.h"
#include "abi.h"
#include "boolean.h"
#include "tc_cache.h"
#include "variable.h"

static KLObject* eval_in_global_environments (KLObject* object)
{
  return eval_kl_object(object,
                        get_global_function_environment(),
                        get_global_variable_environment());
}

static void load_kl_from_stream (KLObject* stream)
{
  while (true) {
    KLObject* read_object = read_string(stream);

    if (is_null(read_object))
      break;

    eval_in_global_environments(read_object);
  }
}

void load_kl_file (char* file_path)
{
  KLObject* stream = create_kl_stream_from_home_path(file_path,
                                                     get_in_symbol_object());

  load_kl_from_stream(stream);
  close_kl_stream(stream);
}

void load_kl_path (char* file_path)
{
  FILE* file = fopen(file_path, "r");
  KLObject* stream;

  if (is_null(file))
    throw_kl_exception("Failed to open stream");

  stream = create_std_kl_stream(file, get_in_symbol_object());
  load_kl_from_stream(stream);
  fclose(file);
}

void load_shen_kl_files (void)
{
  /* S42 install.lsp order. backend.kl is not booted. */
  load_kl_file("shen/src/kl/sys.kl");
  register_overwrite_sys_primitive_kl_functions();

  load_kl_file("shen/src/kl/writer.kl");
  register_overwrite_writer_primitive_kl_functions();

  load_kl_file("shen/src/kl/core.kl");
  register_overwrite_core_primitive_kl_functions();

  load_kl_file("shen/src/kl/reader.kl");
  register_overwrite_reader_primitive_kl_functions();

  load_kl_file("shen/src/kl/declarations.kl");

  load_kl_file("shen/src/kl/toplevel.kl");
  register_overwrite_toplevel_primitive_kl_functions();

  load_kl_file("shen/src/kl/macros.kl");
  register_overwrite_macros_primitive_kl_functions();

  load_kl_file("shen/src/kl/load.kl");

  load_kl_file("shen/src/kl/prolog.kl");
  /* S42 prolog is interpreted; 22.4 C overwrites are not used. */

  load_kl_file("shen/src/kl/sequent.kl");
  load_kl_file("shen/src/kl/track.kl");
  load_kl_file("shen/src/kl/t-star.kl");

  load_kl_file("shen/src/kl/yacc.kl");
  register_overwrite_yacc_primitive_kl_functions();

  load_kl_file("shen/src/kl/types.kl");
  /* rust install_all: after boot, overwrite kernel defuns with natives.
   * Loaded tests stay on the tree-walker. interpreter.shen is not AOT. */
  if (getenv("SHEN_C_NO_AOT") == NULL)
    shen_kernel_aot_install_all();
  /* C map/pr/... win over AOT kernel cells; tc-cache wraps last. */
  shen_apply_port_overwrites();
}

void load_development_kl_file (void)
{
  load_kl_file("src/kl/development.kl");
}

void call_shen_initialise (void)
{
  /* S42 has no shen.initialise; declarations.kl top-level forms initialise. */
}

static KLObject* lookup_named_symbol (char* name)
{
  return lookup_symbol_table(create_kl_string_with_intern(name));
}

static int symbol_has_function (KLObject* symbol_object)
{
  return is_not_null(symbol_object) &&
    is_not_null(get_kl_symbol_function(symbol_object));
}

static int hush_stdout (void)
{
  KLObject* hush_symbol_object = lookup_named_symbol("*hush*");
  KLObject* hush_value;

  if (is_null(hush_symbol_object))
    return 0;

  hush_value = get_kl_symbol_variable_value(hush_symbol_object);
  return hush_value == get_true_boolean_object();
}

void eval_print_expression (char* expr)
{
  KLObject* eval_symbol_object = lookup_named_symbol("eval");
  KLObject* hd_symbol_object = lookup_named_symbol("hd");
  KLObject* read_from_string_symbol_object = lookup_named_symbol("read-from-string");
  KLObject* result;

  if (symbol_has_function(eval_symbol_object) &&
      symbol_has_function(hd_symbol_object) &&
      symbol_has_function(read_from_string_symbol_object)) {
    KLObject* form =
      CONS(eval_symbol_object,
           CONS(CONS(hd_symbol_object,
                     CONS(CONS(read_from_string_symbol_object,
                               CONS(create_kl_string_with_intern(expr), EL)),
                          EL)),
                EL));

    result = eval_in_global_environments(form);
  } else {
    FILE* file = tmpfile();
    KLObject* stream;
    KLObject* object;

    if (is_null(file))
      throw_kl_exception("Failed to open stream");

    fwrite(expr, 1, strlen(expr), file);
    rewind(file);
    stream = create_std_kl_stream(file, get_in_symbol_object());
    object = read_string(stream);
    fclose(file);

    if (is_null(object))
      return;

    result = eval_in_global_environments(object);
  }

  if (!hush_stdout())
    println_kl_object_display(result);
}

void set_shen_hush (int on)
{
  KLObject* hush_symbol_object = lookup_named_symbol("*hush*");

  if (is_null(hush_symbol_object))
    return;

  set_kl_symbol_variable_value(hush_symbol_object,
                               on ? get_true_boolean_object()
                                  : get_false_boolean_object());
}

void eval_load_file (char* file_path)
{
  KLObject* load_symbol_object = lookup_named_symbol("load");

  if (symbol_has_function(load_symbol_object))
    eval_in_global_environments(CONS(load_symbol_object,
                                     CONS(create_kl_string_with_intern(file_path),
                                          EL)));
  else
    load_kl_path(file_path);
}

void run_script (char* file_path, int argc, char** argv)
{
  KLObject* list_object = EL;
  KLObject* load_symbol_object;
  int i;

  for (i = argc - 1; i >= 0; --i)
    list_object = CONS(create_kl_string_with_intern(argv[i]), list_object);

  list_object = CONS(create_kl_string_with_intern(file_path), list_object);
  set_kl_symbol_variable_value(get_earmuff_argv_symbol_object(), list_object);

  load_symbol_object = lookup_named_symbol("load");

  if (symbol_has_function(load_symbol_object))
    eval_in_global_environments(CONS(load_symbol_object,
                                     CONS(create_kl_string_with_intern(file_path),
                                          EL)));
  else
    load_kl_path(file_path);
}

void run_kl_repl (void)
{
  KLObject* std_input_stream_object = get_std_input_stream_object();

  while (true) {
    jmp_buf jump_buffer;

    if (sigsetjmp(jump_buffer, 0) == 0) {
      KLObject* exception_object = create_kl_exception();
    
      set_kl_exception_jump_buffer(exception_object, &jump_buffer);
      push_stack(get_trapped_kl_exception_stack(), exception_object);
    
      while (true) {
        KLObject* read_object;

        printf("> ");
        read_object = read_string(std_input_stream_object);

        if (is_null(read_object))
          return;

        printlnln_kl_object(eval_in_global_environments(read_object));
      }
    } else {
      KLObject* exception_object = pop_stack(get_trapped_kl_exception_stack());
      char* error_message =
        get_exception_error_message(get_exception(exception_object));
      
      printf("Exception: %s\n\n", error_message);
    }
  }
}

void run_shen_repl (void)
{
  KLObject * shen_string_object = create_kl_string_with_intern("shen.shen");
  KLObject* shen_symbol_object = lookup_symbol_table(shen_string_object);
  KLObject* list_object = CONS(shen_symbol_object, EL);

  eval_kl_object(list_object, get_global_function_environment(),
                 get_global_variable_environment());
}
