#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "abi.h"
#include "evaluator.h"
#include "function.h"
#include "gc.h"
#include "init.h"
#include "number.h"
#include "symbol.h"
#include "tc_cache.h"

static int failures = 0;

static void expect (int condition, const char* message)
{
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

/* Generated-style defun: NativeFunction that conses and applies + . */
static KLObject* native_sum3 (KLObject* function_object, Vector* arguments,
                              Environment* function_environment,
                              Environment* variable_environment)
{
  shen_context* ctx = &shen_root_context;
  KLObject** objects = shen_arguments(ctx, function_object, arguments);
  Vector* plus_arguments = shen_vector(ctx, 2);
  KLObject* plus = shen_intern(ctx, "+");
  KLObject* partial;

  (void)function_environment;
  (void)variable_environment;

  shen_vector_set(ctx, plus_arguments, 0, objects[0]);
  shen_vector_set(ctx, plus_arguments, 1, objects[1]);
  partial = shen_apply(ctx, plus, plus_arguments);
  shen_vector_set(ctx, plus_arguments, 0, partial);
  shen_vector_set(ctx, plus_arguments, 1, objects[2]);

  return shen_apply(ctx, plus, plus_arguments);
}

static void test_intern_cons (void)
{
  shen_context* ctx = &shen_root_context;
  KLObject* plus = shen_intern(ctx, "+");
  KLObject* one = shen_number_l(ctx, 1);
  KLObject* two = shen_number_l(ctx, 2);
  KLObject* list = shen_cons(ctx, one, shen_cons(ctx, two, shen_empty_list(ctx)));
  KLObject* name;

  expect(plus != NULL, "intern +");
  expect(is_kl_symbol(plus), "intern + is a symbol");
  expect(shen_symbol_function(ctx, plus) != NULL, "+ has a NativeFunction");
  expect(is_primitive_kl_function(shen_symbol_function(ctx, plus)),
         "+ is a primitive NativeFunction");
  expect(shen_hd(ctx, list) == one, "hd");
  expect(shen_hd(ctx, shen_tl(ctx, list)) == two, "tl hd");
  expect(shen_intern(ctx, "+") == plus, "intern is table-stable");
  expect(shen_intern(ctx, "true") == shen_intern(ctx, "true"), "intern true");

  name = shen_string(ctx, "hello");
  expect(strcmp(shen_string_value(ctx, name), "hello") == 0, "string intern");
  expect(shen_number_l_value(ctx, shen_add(ctx, one, two)) == 3, "shen_add longs");
  expect(shen_number_l_value(ctx, shen_sub(ctx, two, one)) == 1, "shen_sub longs");
  expect(shen_number_l_value(ctx, shen_mul(ctx, two, one)) == 2, "shen_mul longs");
  expect(shen_number_l_value(ctx, shen_div(ctx, two, one)) == 2, "shen_div longs");
  expect(shen_eq(ctx, one, one) == shen_true(ctx), "shen_eq identical");
  expect(shen_eq(ctx, one, two) == shen_false(ctx), "shen_eq distinct longs");
  expect(shen_cons_p(ctx, list) == shen_true(ctx), "shen_cons_p pair");
  expect(shen_cons_p(ctx, shen_empty_list(ctx)) == shen_false(ctx),
         "shen_cons_p empty");
  expect(shen_lt(ctx, one, two) == shen_true(ctx), "shen_lt longs");
  expect(shen_gt(ctx, two, one) == shen_true(ctx), "shen_gt longs");
  expect(shen_lte(ctx, one, one) == shen_true(ctx), "shen_lte longs");
  expect(shen_gte(ctx, two, two) == shen_true(ctx), "shen_gte longs");
  expect(shen_number_p(ctx, one) == shen_true(ctx), "shen_number_p");
  expect(shen_string_p(ctx, name) == shen_true(ctx), "shen_string_p");
  expect(shen_symbol_p(ctx, plus) == shen_true(ctx), "shen_symbol_p interned");
  expect(shen_symbol_p(ctx, one) == shen_false(ctx), "shen_symbol_p number");
  expect(shen_absvector_p(ctx, one) == shen_false(ctx), "shen_absvector_p number");
}

static void test_apply_add (void)
{
  shen_context* ctx = &shen_root_context;
  KLObject* plus = shen_intern(ctx, "+");
  KLObject* function_object = shen_symbol_function(ctx, plus);
  Vector* arguments = shen_vector(ctx, 2);
  NativeFunction* native_function;
  KLObject* result;

  shen_vector_set(ctx, arguments, 0, shen_number_l(ctx, 40));
  shen_vector_set(ctx, arguments, 1, shen_number_l(ctx, 2));

  native_function = get_primitive_function_native_function(
    get_kl_function_primitive_function(function_object));
  result = native_function(function_object, arguments,
                           NULL, NULL);

  expect(shen_is_number_l(ctx, result), "NativeFunction + returns a long");
  expect(shen_number_l_value(ctx, result) == 42, "NativeFunction (+ 40 2) == 42");

  result = shen_apply(ctx, plus, arguments);
  expect(shen_number_l_value(ctx, result) == 42, "shen_apply interned +");

  result = shen_apply(ctx, function_object, arguments);
  expect(shen_number_l_value(ctx, result) == 42, "shen_apply function object +");
}

static void test_register_defun (void)
{
  shen_context* ctx = &shen_root_context;
  Vector* arguments = shen_vector(ctx, 3);
  KLObject* result;

  shen_register_defun(ctx, "sum3", 3, &native_sum3);
  shen_vector_set(ctx, arguments, 0, shen_number_l(ctx, 10));
  shen_vector_set(ctx, arguments, 1, shen_number_l(ctx, 20));
  shen_vector_set(ctx, arguments, 2, shen_number_l(ctx, 12));
  result = shen_apply(ctx, shen_intern(ctx, "sum3"), arguments);

  expect(shen_is_number_l(ctx, result), "sum3 returns a long");
  expect(shen_number_l_value(ctx, result) == 42, "sum3 NativeFunction == 42");
}

static KLObject* trap_boom (void* data)
{
  (void)data;
  shen_simple_error(&shen_root_context, "boom");
  return NULL;
}

static KLObject* trap_handler (KLObject* exception, void* data)
{
  (void)data;

  return shen_error_to_string(&shen_root_context, exception);
}

static KLObject* native_trap_then_plus (KLObject* function_object,
                                        Vector* arguments,
                                        Environment* function_environment,
                                        Environment* variable_environment)
{
  shen_context* ctx = &shen_root_context;
  Vector* args;

  (void)function_object;
  (void)arguments;
  (void)function_environment;
  (void)variable_environment;
  (void)shen_trap_error(ctx, trap_boom, trap_handler, NULL);
  args = shen_vector(ctx, 2);
  shen_vector_set(ctx, args, 0, shen_number_l(ctx, 10));
  shen_vector_set(ctx, args, 1, shen_number_l(ctx, 32));

  return shen_tail_apply(ctx, shen_intern(ctx, "+"), args);
}

static void test_trap_error (void)
{
  shen_context* ctx = &shen_root_context;
  KLObject* result = shen_trap_error(ctx, trap_boom, trap_handler, NULL);
  Vector* no_args;

  expect(is_kl_string(result), "trap-error handler returns a string");
  expect(strcmp(shen_string_value(ctx, result), "boom") == 0,
         "trap-error message is boom");

  shen_register_defun(ctx, "trap-then-plus", 0, &native_trap_then_plus);
  no_args = shen_vector(ctx, 0);
  result = shen_apply(ctx, shen_intern(ctx, "trap-then-plus"), no_args);
  expect(shen_is_number_l(ctx, result), "trap then tail-apply + returns a long");
  expect(shen_number_l_value(ctx, result) == 42,
         "trap-error restores trampoline state");
}

static void test_eval_kl_available (void)
{
  shen_context* ctx = &shen_root_context;
  KLObject* seven = shen_number_l(ctx, 7);
  KLObject* result = shen_eval_kl(ctx, seven);

  expect(result == seven, "eval-kl is available for needs-eval (self-eval number)");
}

/* macroexpand-shaped: (lambda Z (tl Z)) over a *macros* assoc cell. */
static KLObject* native_lambda_tl (KLObject* function_object, Vector* arguments,
                                   Environment* function_environment,
                                   Environment* variable_environment)
{
  shen_context* ctx = &shen_root_context;
  KLObject** objects = shen_arguments(ctx, function_object, arguments);

  (void)function_environment;
  (void)variable_environment;

  return shen_tl(ctx, objects[0]);
}

static KLObject* native_freeze_seven (KLObject* function_object, Vector* arguments,
                                      Environment* function_environment,
                                      Environment* variable_environment)
{
  (void)function_object;
  (void)arguments;
  (void)function_environment;
  (void)variable_environment;

  return shen_number_l(&shen_root_context, 7);
}

static void test_native_closure_cons_map_apply (void)
{
  shen_context* ctx = &shen_root_context;
  KLObject* fn = shen_native_closure(ctx, 1, &native_lambda_tl, NULL);
  KLObject* name = shen_intern(ctx, "demo-macro");
  KLObject* pair = shen_cons(ctx, name, fn);
  KLObject* macros = shen_cons(ctx, pair, shen_empty_list(ctx));
  Vector* map_arguments = shen_vector(ctx, 2);
  KLObject* mapped;
  KLObject* cell;
  Vector* apply_arguments;
  KLObject* applied;
  KLObject* quoted_pair;
  KLObject* eval_form;
  KLObject* eval_result;
  KLObject* freeze;
  KLObject* freeze_result;

  expect(is_kl_function(fn), "wrapped NativeFunction is KL_TYPE_FUNCTION");
  expect(is_primitive_kl_function(fn),
         "wrapped NativeFunction is PrimitiveFunction");
  expect(!is_closure_kl_function(fn),
         "wrapped NativeFunction is not a tree-walker Closure");

  shen_vector_set(ctx, map_arguments, 0, fn);
  shen_vector_set(ctx, map_arguments, 1, macros);
  mapped = shen_apply(ctx, shen_intern(ctx, "map"), map_arguments);
  cell = shen_hd(ctx, mapped);

  expect(cell == fn, "map/tl of assoc cell is the wrapped primitive");
  expect(is_kl_function(cell), "mapped cell is a boxed function");
  expect(is_primitive_kl_function(cell),
         "mapped cell is_primitive_kl_function, not raw NativeFunction*");
  expect(!is_closure_kl_function(cell),
         "mapped cell is not a Closure pun of native_function");

  apply_arguments = shen_vector(ctx, 1);
  shen_vector_set(ctx, apply_arguments, 0, pair);
  applied = shen_apply(ctx, cell, apply_arguments);
  expect(applied == fn, "shen_apply of mapped (lambda Z (tl Z)) succeeds");

  quoted_pair = shen_cons(ctx, shen_intern(ctx, "c.quote"),
                          shen_cons(ctx, pair, shen_empty_list(ctx)));
  eval_form = shen_cons(ctx, fn, shen_cons(ctx, quoted_pair, shen_empty_list(ctx)));
  eval_result = shen_eval_kl(ctx, eval_form);
  expect(eval_result == fn,
         "eval-kl of (wrapped-fn quoted-pair) does not unwrap to NativeFunction*");
  expect(is_primitive_kl_function(eval_result),
         "eval-kl application result stays PrimitiveFunction");

  freeze = shen_native_closure(ctx, 0, &native_freeze_seven, NULL);
  freeze_result = eval_simple_closure_function_application(freeze);
  expect(shen_is_number_l(ctx, freeze_result),
         "arity-0 primitive freeze returns a number");
  expect(shen_number_l_value(ctx, freeze_result) == 7,
         "arity-0 primitive freeze via eval_simple_closure is 7");
}

/* Non-tail native recursion: would SIGSEGV on an 8MiB C stack without a
 * bounded trampoline of generated NativeFunctions. */
static KLObject* native_deep (KLObject* function_object, Vector* arguments,
                              Environment* function_environment,
                              Environment* variable_environment)
{
  shen_context* ctx = &shen_root_context;
  KLObject** objects = shen_arguments(ctx, function_object, arguments);
  long n;
  Vector* next;
  KLObject* inner;

  (void)function_environment;
  (void)variable_environment;
  n = shen_number_l_value(ctx, objects[0]);

  if (n <= 0)
    return shen_number_l(ctx, 0);

  next = shen_vector(ctx, 1);
  shen_vector_set(ctx, next, 0, shen_number_l(ctx, n - 1));
  inner = shen_apply(ctx, shen_intern(ctx, "deep-native"), next);

  return shen_number_l(ctx, shen_number_l_value(ctx, inner) + 1);
}

static void test_native_stack_trampoline (void)
{
  shen_context* ctx = &shen_root_context;
  Vector* arguments = shen_vector(ctx, 1);
  KLObject* result;

  shen_register_defun(ctx, "deep-native", 1, &native_deep);
  shen_vector_set(ctx, arguments, 0, shen_number_l(ctx, 40000));
  result = shen_apply(ctx, shen_intern(ctx, "deep-native"), arguments);
  expect(shen_is_number_l(ctx, result), "deep-native returns a long");
  expect(shen_number_l_value(ctx, result) == 40000,
         "deep-native 40000 hops a fresh C stack and returns 40000");
}

/* Fat C frames (t* / system-S-h shaped): Darwin stackaddr bounds missed
 * these and SIGSEGV'd before apply_depth 8192. */
static KLObject* native_fat (KLObject* function_object, Vector* arguments,
                             Environment* function_environment,
                             Environment* variable_environment)
{
  shen_context* ctx = &shen_root_context;
  KLObject** objects = shen_arguments(ctx, function_object, arguments);
  volatile char pad[65536];
  long n;
  Vector* next;
  KLObject* inner;

  (void)function_environment;
  (void)variable_environment;
  pad[0] = 1;
  pad[65535] = 1;
  n = shen_number_l_value(ctx, objects[0]);

  if (n <= 0) {
    pad[1] = (char)(pad[0] | pad[65535]);
    return shen_number_l(ctx, 0);
  }

  next = shen_vector(ctx, 1);
  shen_vector_set(ctx, next, 0, shen_number_l(ctx, n - 1));
  inner = shen_apply(ctx, shen_intern(ctx, "fat-native"), next);

  return shen_number_l(ctx, shen_number_l_value(ctx, inner) + 1);
}

static void test_native_fat_stack_trampoline (void)
{
  shen_context* ctx = &shen_root_context;
  Vector* arguments = shen_vector(ctx, 1);
  KLObject* result;

  shen_register_defun(ctx, "fat-native", 1, &native_fat);
  shen_vector_set(ctx, arguments, 0, shen_number_l(ctx, 400));
  result = shen_apply(ctx, shen_intern(ctx, "fat-native"), arguments);
  expect(shen_is_number_l(ctx, result), "fat-native returns a long");
  expect(shen_number_l_value(ctx, result) == 400,
         "fat-native 400 hops past 64KiB frames without SIGSEGV 139");
}

static KLObject* native_ov_const7 (KLObject* function_object, Vector* arguments,
                                  Environment* function_environment,
                                  Environment* variable_environment)
{
  (void)function_object;
  (void)arguments;
  (void)function_environment;
  (void)variable_environment;

  return shen_number_l(&shen_root_context, 7);
}

static KLObject* native_ov_const9 (KLObject* function_object, Vector* arguments,
                                  Environment* function_environment,
                                  Environment* variable_environment)
{
  (void)function_object;
  (void)arguments;
  (void)function_environment;
  (void)variable_environment;

  return shen_number_l(&shen_root_context, 9);
}

static void overlay_install_const9 (shen_context* ctx)
{
  shen_register_defun(ctx, "ov-probe", 0, &native_ov_const9);
}

static void test_overlay_install (void)
{
  shen_context* ctx = &shen_root_context;
  static ShenOverlayNameArity compiled[1];
  static ShenOverlayModule module;
  const unsigned char live[] = "interpreter.shen source bytes";
  uint64_t kernel_fnv;
  Vector* args = shen_vector(ctx, 0);
  KLObject* result;
  KLObject* before;
  KLObject* params;
  KLObject* body;
  KLObject* defun;

  compiled[0].name = "ov-probe";
  compiled[0].arity = 0;
  kernel_fnv = shen_kernel_digest("shen/src/kl");
  expect(kernel_fnv != 0xcbf29ce484222325ULL,
         "kernel digest mixes .kl files");

  shen_overlay_set_kernel_dir("shen/src/kl");
  shen_register_defun(ctx, "ov-probe", 0, &native_ov_const7);
  before = shen_apply(ctx, shen_intern(ctx, "ov-probe"), args);
  expect(shen_number_l_value(ctx, before) == 7, "pre-overlay ov-probe is 7");

  module.label = "ov-probe-test";
  module.format = "wrong-format";
  module.source_fnv = shen_fnv64(live, sizeof(live) - 1);
  module.kernel_fnv = kernel_fnv;
  module.compiled = compiled;
  module.ncompiled = 1;
  module.install = overlay_install_const9;
  expect(shen_install_overlay_if_match(ctx, &module, live, sizeof(live) - 1) == 0,
         "format mismatch is silent fallback");
  result = shen_apply(ctx, shen_intern(ctx, "ov-probe"), args);
  expect(shen_number_l_value(ctx, result) == 7,
         "format mismatch leaves walker/native in place");

  module.format = SHEN_OVERLAY_FORMAT;
  module.source_fnv = 1;
  expect(shen_install_overlay_if_match(ctx, &module, live, sizeof(live) - 1) == 0,
         "source hash mismatch is silent fallback");
  result = shen_apply(ctx, shen_intern(ctx, "ov-probe"), args);
  expect(shen_number_l_value(ctx, result) == 7, "hash mismatch does not install");

  module.source_fnv = shen_fnv64(live, sizeof(live) - 1);
  compiled[0].arity = 1;
  expect(shen_install_overlay_if_match(ctx, &module, live, sizeof(live) - 1) == 0,
         "arity mismatch is silent fallback");
  result = shen_apply(ctx, shen_intern(ctx, "ov-probe"), args);
  expect(shen_number_l_value(ctx, result) == 7, "arity mismatch does not install");

  compiled[0].arity = 0;
  expect(shen_install_overlay_if_match(ctx, &module, live, sizeof(live) - 1) == 1,
         "matching overlay installs");
  result = shen_apply(ctx, shen_intern(ctx, "ov-probe"), args);
  expect(shen_number_l_value(ctx, result) == 9, "installed overlay native wins");

  params = shen_empty_list(ctx);
  body = shen_number_l(ctx, 3);
  defun = shen_cons(ctx, shen_intern(ctx, "defun"),
                    shen_cons(ctx, shen_intern(ctx, "ov-probe"),
                              shen_cons(ctx, params,
                                        shen_cons(ctx, body,
                                                  shen_empty_list(ctx)))));
  (void)shen_eval_kl(ctx, defun);
  result = shen_apply(ctx, shen_intern(ctx, "ov-probe"), args);
  expect(shen_number_l_value(ctx, result) == 3,
         "later defun over overlay name wins");
}

static KLObject* eval_form (const char* name, KLObject* arg)
{
  shen_context* ctx = &shen_root_context;

  return shen_eval_kl(ctx,
                      shen_cons(ctx, shen_intern(ctx, name),
                                shen_cons(ctx, arg, shen_empty_list(ctx))));
}

static KLObject* load_path (const char* path)
{
  shen_context* ctx = &shen_root_context;
  Vector* args = shen_vector(ctx, 1);

  shen_vector_set(ctx, args, 0, shen_string(ctx, path));
  return shen_apply(ctx, shen_intern(ctx, "load"), args);
}

static int kernel_loaded = 0;
static long kernel_gensym = 0;

static long read_gensym (void)
{
  KLObject* v = get_kl_symbol_variable_value(
    shen_intern(&shen_root_context, "shen.*gensym*"));

  if (v == NULL || !is_kl_number_l(v))
    return 0;

  return get_kl_number_number_l(v);
}

static void restore_gensym (void)
{
  set_kl_symbol_variable_value(
    shen_intern(&shen_root_context, "shen.*gensym*"),
    shen_number_l(&shen_root_context, kernel_gensym));
}

static void ensure_kernel (void)
{
  if (kernel_loaded)
    return;

  initialize_shen_kernel();
  kernel_gensym = read_gensym();
  kernel_loaded = 1;
}

static int count_tc_files (const char* dir)
{
  char cmd[4096];
  FILE* p;
  int count = -1;

  snprintf(cmd, sizeof(cmd),
           "find '%s' -name '*.tc' ! -name '*.tc.tmp' | wc -l", dir);
  p = popen(cmd, "r");
  if (p == NULL)
    return -1;
  if (fscanf(p, "%d", &count) != 1)
    count = -1;
  pclose(p);
  return count;
}

static void write_text (const char* path, const char* text)
{
  FILE* f = fopen(path, "w");

  expect(f != NULL, "open scratch shen file");
  if (f != NULL) {
    fputs(text, f);
    fclose(f);
  }
}

static void test_tc_cache_record_replay (void)
{
  shen_context* ctx = &shen_root_context;
  char scratch[] = "/tmp/shen-c-tc-XXXXXX";
  char file[4096];
  uint64_t hits = 0;
  uint64_t misses = 0;
  KLObject* loaded;
  KLObject* n;
  const char* v1 =
    "(define idf\n  {A --> A}\n  X -> X)\n\n"
    "(define dup\n  {A --> (list A)}\n  X -> [X X])\n";
  const char* v2 =
    "(define idf\n  {A --> A}\n  X -> X)\n\n"
    "(define dup\n  {A --> (list A)}\n  X -> [X X X])\n";
  KLObject* orig_shen_to_kl;

  ensure_kernel();
  expect(mkdtemp(scratch) != NULL, "mkdtemp tc-cache scratch");
  snprintf(file, sizeof(file), "%s/prog.shen", scratch);

  orig_shen_to_kl = shen_symbol_function(ctx, shen_intern(ctx, "shen.shen->kl"));
  shen_tc_cache_install(scratch, 0, "shen/src/kl");
  expect(shen_symbol_function(ctx, shen_intern(ctx, "shen.shen->kl")) ==
         orig_shen_to_kl,
         "tc-cache does not wrap shen.shen->kl");

  write_text(file, v1);
  restore_gensym();
  (void)eval_form("tc", shen_intern(ctx, "+"));
  loaded = load_path(file);
  expect(is_kl_symbol(loaded), "first load returns a symbol");
  expect(shen_tc_cache_stats(&hits, &misses) == 1, "stats after record");
  expect(hits == 0 && misses == 1, "first load misses");
  expect(count_tc_files(scratch) == 1, "record writes one cache file");
  (void)eval_form("tc", shen_intern(ctx, "-"));
  n = shen_eval_kl(ctx,
                   shen_cons(ctx, shen_intern(ctx, "length"),
                             shen_cons(ctx,
                                       shen_cons(ctx, shen_intern(ctx, "dup"),
                                                 shen_cons(ctx,
                                                           shen_number_l(ctx, 7),
                                                           shen_empty_list(ctx))),
                                       shen_empty_list(ctx))));
  expect(shen_is_number_l(ctx, n) && shen_number_l_value(ctx, n) == 2,
         "recorded load defines dup as pair");

  shen_tc_cache_install(scratch, 0, "shen/src/kl");
  restore_gensym();
  (void)eval_form("tc", shen_intern(ctx, "+"));
  loaded = load_path(file);
  expect(is_kl_symbol(loaded), "replay load returns a symbol");
  expect(shen_tc_cache_stats(&hits, &misses) == 1, "stats after replay");
  expect(hits == 1 && misses == 0, "second session hits");
  (void)eval_form("tc", shen_intern(ctx, "-"));
  n = shen_eval_kl(ctx,
                   shen_cons(ctx, shen_intern(ctx, "length"),
                             shen_cons(ctx,
                                       shen_cons(ctx, shen_intern(ctx, "dup"),
                                                 shen_cons(ctx,
                                                           shen_number_l(ctx, 7),
                                                           shen_empty_list(ctx))),
                                       shen_empty_list(ctx))));
  expect(shen_is_number_l(ctx, n) && shen_number_l_value(ctx, n) == 2,
         "replayed load keeps dup as pair");

  write_text(file, v2);
  shen_tc_cache_install(scratch, 0, "shen/src/kl");
  restore_gensym();
  (void)eval_form("tc", shen_intern(ctx, "+"));
  loaded = load_path(file);
  expect(is_kl_symbol(loaded), "edited load returns a symbol");
  expect(shen_tc_cache_stats(&hits, &misses) == 1, "stats after edit");
  expect(hits == 0 && misses == 1, "edited file misses");
  (void)eval_form("tc", shen_intern(ctx, "-"));
  n = shen_eval_kl(ctx,
                   shen_cons(ctx, shen_intern(ctx, "length"),
                             shen_cons(ctx,
                                       shen_cons(ctx, shen_intern(ctx, "dup"),
                                                 shen_cons(ctx,
                                                           shen_number_l(ctx, 7),
                                                           shen_empty_list(ctx))),
                                       shen_empty_list(ctx))));
  expect(shen_is_number_l(ctx, n) && shen_number_l_value(ctx, n) == 3,
         "edited dup triples");

  write_text(file, v1);
  shen_tc_cache_install(scratch, 0, "shen/src/kl");
  restore_gensym();
  (void)eval_form("tc", shen_intern(ctx, "+"));
  loaded = load_path(file);
  expect(shen_tc_cache_stats(&hits, &misses) == 1, "stats after v1 restore");
  expect(hits == 1 && misses == 0, "v1 entry replays again");
  (void)eval_form("tc", shen_intern(ctx, "-"));
}

static char tc_bad_path[4096];
static int tc_bad_failed = 0;

static KLObject* tc_bad_body (void* data)
{
  (void)data;
  return load_path(tc_bad_path);
}

static KLObject* tc_bad_fail (KLObject* exception, void* data)
{
  (void)exception;
  (void)data;
  tc_bad_failed = 1;
  return shen_false(&shen_root_context);
}

static void test_tc_cache_type_errors_uncached (void)
{
  shen_context* ctx = &shen_root_context;
  char scratch[] = "/tmp/shen-c-tc-bad-XXXXXX";
  uint64_t hits = 0;
  uint64_t misses = 0;
  int round;
  char cmd[4096];
  FILE* p;
  int count = -1;

  ensure_kernel();
  expect(mkdtemp(scratch) != NULL, "mkdtemp type-error scratch");
  snprintf(tc_bad_path, sizeof(tc_bad_path), "%s/bad.shen", scratch);
  write_text(tc_bad_path,
             "(define bad\n  {number --> number}\n  X -> (cn X \"!\"))\n");

  for (round = 0; round < 2; ++round) {
    shen_tc_cache_install(scratch, 0, "shen/src/kl");
    restore_gensym();
    (void)eval_form("tc", shen_intern(ctx, "+"));
    tc_bad_failed = 0;
    (void)shen_trap_error(ctx, tc_bad_body, tc_bad_fail, NULL);
    expect(tc_bad_failed == 1, "ill-typed load errors");
    expect(shen_tc_cache_stats(&hits, &misses) == 1, "stats on bad load");
    expect(hits == 0 && misses == 1, "failed load is a miss");
    (void)eval_form("tc", shen_intern(ctx, "-"));
  }

  snprintf(cmd, sizeof(cmd),
           "find '%s' -name '*.tc' ! -name '*.tc.tmp' | wc -l", scratch);
  p = popen(cmd, "r");
  expect(p != NULL, "count bad-load cache files");
  if (p != NULL) {
    if (fscanf(p, "%d", &count) != 1)
      count = -1;
    pclose(p);
  }
  expect(count == 0, "failed loads must never be recorded");
}

int main (void)
{
  shen_boot(&shen_root_context, ".");

  expect(shen_root_context.gc_ready == 1, "shen_context GC is initialized");
  expect(GC_base(shen_number_l(&shen_root_context, 1)) != NULL,
         "numbers live on Boehm GC heap");

  test_intern_cons();
  test_apply_add();
  test_register_defun();
  test_trap_error();
  test_eval_kl_available();
  test_native_closure_cons_map_apply();
  test_native_stack_trampoline();
  test_native_fat_stack_trampoline();
  test_overlay_install();
  test_tc_cache_record_replay();
  test_tc_cache_type_errors_uncached();

  if (failures != 0) {
    fprintf(stderr, "%d abi test(s) failed\n", failures);
    return 1;
  }

  printf("abi tests ok\n");
  return 0;
}
