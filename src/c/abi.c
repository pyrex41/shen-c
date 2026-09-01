#include <dirent.h>
#include <pthread.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#include "abi.h"
#include "boolean.h"
#include "environment.h"
#include "evaluator.h"
#include "exception.h"
#include "function.h"
#include "gc.h"
#include "init.h"
#include "list.h"
#include "number.h"
#include "overwrite.h"
#include "primitive.h"
#include "stack.h"
#include "stream.h"
#include "string.h"
#include "symbol.h"
#include "tc_cache.h"

/* libgc is built with threads; headers hide these unless GC_THREADS. */
extern void GC_allow_register_threads (void);
extern int GC_pthread_create (pthread_t*, const pthread_attr_t*,
                              void* (*)(void*), void*);
extern int GC_pthread_join (pthread_t, void**);

static NativeFunction* partial_native = NULL;
static int apply_depth = 0;
static int bounce_pending = 0;
static KLObject* bounce_fn = NULL;
static Vector* bounce_args = NULL;

#define SHEN_INTERN_PTR_SLOTS 8192

static struct {
  const char* key;
  KLObject* object;
} intern_ptr_cache[SHEN_INTERN_PTR_SLOTS];

/* Measured wrapping-complete t-star / system-S-h: sample graph ~19k
 * native frames + ~23k shen_apply on Darwin 8MiB thread stack, then
 * SIGSEGV 139. Tail bounce does not apply (if-false chains). Hop
 * generated NativeFunction apply onto a fresh C stack; do not rewrite
 * eval_kl_object. */
#define SHEN_STACK_MARGIN (4 * 1024 * 1024)
#define SHEN_TRAMPOLINE_STACK (16 * 1024 * 1024)
#define SHEN_MAX_TRAMPOLINE_HOPS 256

/* Darwin pthread_get_stacksize_np reports 512KiB on worker threads even
 * after pthread_attr_setstacksize(16MiB), and stackaddr high/low is not
 * stable across OS versions. Measure used stack from the frame at thread
 * start; hop generated NativeFunctions only. */
static __thread char* c_stack_start = NULL;
static __thread size_t c_stack_limit = 0;
static __thread int trampoline_hops = 0;

static size_t shen_thread_stack_limit (void)
{
  struct rlimit rl;

  if (trampoline_hops > 0)
    return SHEN_TRAMPOLINE_STACK;

  if (getrlimit(RLIMIT_STACK, &rl) == 0 &&
      rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0)
    return (size_t)rl.rlim_cur;

  return 8 * 1024 * 1024;
}

static void shen_note_stack_bounds (void)
{
  c_stack_start = (char*)__builtin_frame_address(0);
  c_stack_limit = shen_thread_stack_limit();
}

static int shen_c_stack_low (void)
{
  char* here;
  ptrdiff_t used;

  if (c_stack_start == NULL)
    shen_note_stack_bounds();

  here = (char*)__builtin_frame_address(0);
  used = c_stack_start - here;

  if (used < 0)
    used = -used;

  return (size_t)used + (size_t)SHEN_STACK_MARGIN >= c_stack_limit;
}

typedef struct NativeTrampoline {
  KLObject* function_object;
  Vector* arguments;
  KLObject* result;
  char error[1024];
  int threw;
  int hops;
  void* stack_map;
  size_t stack_map_size;
  size_t stack_usable;
} NativeTrampoline;

static int alloc_trampoline_stack (NativeTrampoline* job)
{
  long page = sysconf(_SC_PAGESIZE);
  size_t guard;
  void* map;

  if (page <= 0)
    page = 4096;

  guard = (size_t)page;
  job->stack_map_size = SHEN_TRAMPOLINE_STACK + guard;
  map = mmap(NULL, job->stack_map_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (map == MAP_FAILED) {
    job->stack_map = NULL;
    return -1;
  }

  if (mprotect(map, guard, PROT_NONE) != 0) {
    munmap(map, job->stack_map_size);
    job->stack_map = NULL;
    return -1;
  }

  job->stack_map = map;
  job->stack_usable = SHEN_TRAMPOLINE_STACK;
  return 0;
}

static void free_trampoline_stack (NativeTrampoline* job)
{
  if (job->stack_map != NULL)
    munmap(job->stack_map, job->stack_map_size);

  job->stack_map = NULL;
}

static void* native_trampoline_worker (void* data)
{
  NativeTrampoline* job = (NativeTrampoline*)data;
  Stack* saved_traps = get_trapped_kl_exception_stack();
  Stack* local_traps = create_stack();
  int saved_depth = apply_depth;
  int saved_bounce = bounce_pending;
  KLObject* saved_fn = bounce_fn;
  Vector* saved_args = bounce_args;
  jmp_buf jump_buffer;

  /* Private trap stack: a jmp_buf on this mmap stack must not stay on
   * the global trap list after join (freed stack -> SIGSEGV 139). */
  trampoline_hops = job->hops;
  apply_depth = 0;
  bounce_pending = 0;
  bounce_fn = NULL;
  bounce_args = NULL;
  set_trapped_kl_exception_stack(local_traps);
  c_stack_start = (char*)__builtin_frame_address(0);
  c_stack_limit = job->stack_usable != 0 ? job->stack_usable
                                         : SHEN_TRAMPOLINE_STACK;

  if (sigsetjmp(jump_buffer, 0) == 0) {
    KLObject* exception_object = create_kl_exception();

    set_kl_exception_jump_buffer(exception_object, &jump_buffer);
    push_stack(local_traps, exception_object);
    job->result = shen_apply(&shen_root_context, job->function_object,
                             job->arguments);
    pop_stack(local_traps);
  } else {
    KLObject* exception_object = pop_stack(local_traps);
    char* message =
      get_exception_error_message(get_exception(exception_object));

    job->threw = 1;
    snprintf(job->error, sizeof(job->error), "%s",
             message != NULL ? message : "native trampoline error");
  }

  apply_depth = saved_depth;
  bounce_pending = saved_bounce;
  bounce_fn = saved_fn;
  bounce_args = saved_args;
  set_trapped_kl_exception_stack(saved_traps);
  return NULL;
}

static KLObject* apply_on_fresh_stack (KLObject* function_object,
                                       Vector* arguments)
{
  NativeTrampoline job;
  pthread_attr_t attr;
  pthread_t thread;
  int hops = trampoline_hops + 1;
  void* stack_addr;

  if (hops > SHEN_MAX_TRAMPOLINE_HOPS)
    throw_kl_exception("native apply stack exhausted");

  memset(&job, 0, sizeof(job));
  job.function_object = function_object;
  job.arguments = arguments;
  job.hops = hops;

  if (alloc_trampoline_stack(&job) != 0)
    throw_kl_exception("native apply trampoline stack mmap failed");

  stack_addr = (char*)job.stack_map + (job.stack_map_size - job.stack_usable);
  pthread_attr_init(&attr);
  (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  /* Darwin GC_pthread_create ignores setstacksize (workers stay 512KiB).
   * mmap + setstack is the actual 16MiB stack. */
  if (pthread_attr_setstack(&attr, stack_addr, job.stack_usable) != 0) {
    pthread_attr_destroy(&attr);
    free_trampoline_stack(&job);
    throw_kl_exception("native apply trampoline setstack failed");
  }

  if (GC_pthread_create(&thread, &attr, native_trampoline_worker, &job) != 0) {
    pthread_attr_destroy(&attr);
    free_trampoline_stack(&job);
    throw_kl_exception("native apply trampoline spawn failed");
  }

  pthread_attr_destroy(&attr);
  GC_pthread_join(thread, NULL);
  free_trampoline_stack(&job);

  if (job.threw)
    throw_kl_exception(job.error);

  return job.result;
}

static KLObject* intern_kl_string (const char* string)
{
  KLObject* string_object = lookup_string_table((char*)string);

  if (is_not_null(string_object))
    return string_object;

  size_t length = strlen(string);
  char* copy = malloc(length + 1);

  memcpy(copy, string, length + 1);

  return create_kl_string_with_intern(copy);
}

void shen_boot (shen_context* ctx, const char* home_path)
{
  if (ctx->gc_ready == 0)
    shen_context_init(ctx);

  initialize_runtime(home_path);
  GC_allow_register_threads();
  shen_note_stack_bounds();
  /* Generated apps never load writer.kl; still need pr / C overwrites. */
  shen_apply_port_overwrites();
}

void shen_apply_port_overwrites (void)
{
  register_overwrite_sys_primitive_kl_functions();
  register_overwrite_core_primitive_kl_functions();
  register_overwrite_reader_primitive_kl_functions();
  register_overwrite_writer_primitive_kl_functions();
  register_overwrite_toplevel_primitive_kl_functions();
  register_overwrite_macros_primitive_kl_functions();
  register_overwrite_yacc_primitive_kl_functions();
  /* After generated_install on AOT apps: wrap current load / typecheck. */
  shen_tc_cache_install_from_env();
}

static KLObject* shen_intern_uncached (shen_context* ctx, const char* name)
{
  KLObject* string_object = intern_kl_string(name);
  KLObject* symbol_object;

  (void)ctx;

  if (is_kl_string_equal(string_object, true_string_object))
    return get_true_boolean_object();
  else if (is_kl_string_equal(string_object, false_string_object))
    return get_false_boolean_object();

  symbol_object = lookup_symbol_table(string_object);

  if (is_null(symbol_object)) {
    symbol_object = create_kl_symbol(string_object);
    extend_symbol_table(string_object, symbol_object);
  }

  return symbol_object;
}

KLObject* shen_intern (shen_context* ctx, const char* name)
{
  if (name == NULL)
    throw_kl_exception("intern of null");

  return shen_intern_uncached(ctx, name);
}

/* AOT call-target literals only. Do not use for reusable buffers. */
static KLObject* shen_intern_static (shen_context* ctx, const char* name)
{
  uintptr_t key;
  size_t idx;
  KLObject* object;

  if (name == NULL)
    throw_kl_exception("intern of null");

  key = (uintptr_t)name;
  idx = ((key >> 3) ^ (key >> 16)) & (SHEN_INTERN_PTR_SLOTS - 1);

  if (intern_ptr_cache[idx].key == name)
    return intern_ptr_cache[idx].object;

  object = shen_intern_uncached(ctx, name);
  intern_ptr_cache[idx].key = name;
  intern_ptr_cache[idx].object = object;

  return object;
}

KLObject* shen_cons (shen_context* ctx, KLObject* head, KLObject* tail)
{
  (void)ctx;

  return CONS(head, tail);
}

KLObject* shen_hd (shen_context* ctx, KLObject* list)
{
  (void)ctx;

  return CAR(list);
}

KLObject* shen_tl (shen_context* ctx, KLObject* list)
{
  (void)ctx;

  return CDR(list);
}

KLObject* shen_empty_list (shen_context* ctx)
{
  (void)ctx;

  return get_empty_kl_list();
}

KLObject* shen_add (shen_context* ctx, KLObject* x, KLObject* y)
{
  (void)ctx;

  if (is_kl_number_l(x) && is_kl_number_l(y))
    return create_kl_number_l(get_kl_number_number_l(x) +
                              get_kl_number_number_l(y));

  if (!is_kl_number(x) || !is_kl_number(y))
    throw_kl_exception("arguments to + must be numbers");

  return add_kl_number(x, y);
}

KLObject* shen_sub (shen_context* ctx, KLObject* x, KLObject* y)
{
  (void)ctx;

  if (is_kl_number_l(x) && is_kl_number_l(y))
    return create_kl_number_l(get_kl_number_number_l(x) -
                              get_kl_number_number_l(y));

  if (!is_kl_number(x) || !is_kl_number(y))
    throw_kl_exception("arguments to - must be numbers");

  return subtract_kl_number(x, y);
}

static KLObject* shen_pred (int yes)
{
  return yes ? get_true_boolean_object() : get_false_boolean_object();
}

KLObject* shen_mul (shen_context* ctx, KLObject* x, KLObject* y)
{
  (void)ctx;

  if (is_kl_number_l(x) && is_kl_number_l(y))
    return create_kl_number_l(get_kl_number_number_l(x) *
                              get_kl_number_number_l(y));

  if (!is_kl_number(x) || !is_kl_number(y))
    throw_kl_exception("arguments to * must be numbers");

  return multiply_kl_number(x, y);
}

KLObject* shen_div (shen_context* ctx, KLObject* x, KLObject* y)
{
  (void)ctx;

  if (!is_kl_number(x) || !is_kl_number(y))
    throw_kl_exception("arguments to / must be numbers");

  return divide_kl_number(x, y);
}

KLObject* shen_lt (shen_context* ctx, KLObject* x, KLObject* y)
{
  (void)ctx;

  if (is_kl_number_l(x) && is_kl_number_l(y))
    return shen_pred(get_kl_number_number_l(x) < get_kl_number_number_l(y));

  return shen_pred(is_kl_number_less(x, y));
}

KLObject* shen_gt (shen_context* ctx, KLObject* x, KLObject* y)
{
  (void)ctx;

  if (is_kl_number_l(x) && is_kl_number_l(y))
    return shen_pred(get_kl_number_number_l(x) > get_kl_number_number_l(y));

  return shen_pred(is_kl_number_greater(x, y));
}

KLObject* shen_lte (shen_context* ctx, KLObject* x, KLObject* y)
{
  (void)ctx;

  if (is_kl_number_l(x) && is_kl_number_l(y))
    return shen_pred(get_kl_number_number_l(x) <= get_kl_number_number_l(y));

  return shen_pred(is_kl_number_less_or_equal(x, y));
}

KLObject* shen_gte (shen_context* ctx, KLObject* x, KLObject* y)
{
  (void)ctx;

  if (is_kl_number_l(x) && is_kl_number_l(y))
    return shen_pred(get_kl_number_number_l(x) >= get_kl_number_number_l(y));

  return shen_pred(is_kl_number_greater_or_equal(x, y));
}

KLObject* shen_eq (shen_context* ctx, KLObject* x, KLObject* y)
{
  (void)ctx;

  if (x == y)
    return get_true_boolean_object();

  return shen_pred(is_kl_object_equal(x, y));
}

KLObject* shen_cons_p (shen_context* ctx, KLObject* x)
{
  (void)ctx;

  return shen_pred(is_non_empty_kl_list(x));
}

KLObject* shen_number_p (shen_context* ctx, KLObject* x)
{
  (void)ctx;

  return shen_pred(is_kl_number(x));
}

KLObject* shen_string_p (shen_context* ctx, KLObject* x)
{
  (void)ctx;

  return shen_pred(is_kl_string(x));
}

KLObject* shen_symbol_p (shen_context* ctx, KLObject* x)
{
  (void)ctx;

  return shen_pred(is_kl_symbol(x));
}

KLObject* shen_absvector_p (shen_context* ctx, KLObject* x)
{
  (void)ctx;

  return shen_pred(is_kl_vector(x));
}

KLObject* shen_number_l (shen_context* ctx, long x)
{
  (void)ctx;

  return create_kl_number_l(x);
}

KLObject* shen_number_d (shen_context* ctx, double x)
{
  (void)ctx;

  return create_kl_number_d(x);
}

long shen_number_l_value (shen_context* ctx, KLObject* number_object)
{
  (void)ctx;

  return get_kl_number_number_l(number_object);
}

int shen_is_number_l (shen_context* ctx, KLObject* object)
{
  (void)ctx;

  return is_kl_number_l(object) ? 1 : 0;
}

KLObject* shen_string (shen_context* ctx, const char* string)
{
  (void)ctx;

  return intern_kl_string(string);
}

const char* shen_string_value (shen_context* ctx, KLObject* string_object)
{
  (void)ctx;

  return get_string(string_object);
}

KLObject* shen_symbol (shen_context* ctx, const char* name)
{
  return shen_intern(ctx, name);
}

KLObject* shen_symbol_function (shen_context* ctx, KLObject* symbol_object)
{
  (void)ctx;

  return get_kl_symbol_function(symbol_object);
}

Vector* shen_vector (shen_context* ctx, long size)
{
  (void)ctx;

  return create_vector(size);
}

void shen_vector_set (shen_context* ctx, Vector* vector, long index,
                      KLObject* object)
{
  (void)ctx;

  set_vector_element(vector, index, object);
}

KLObject* shen_vector_get (shen_context* ctx, Vector* vector, long index)
{
  (void)ctx;

  return get_vector_element(vector, index);
}

KLObject** shen_arguments (shen_context* ctx, KLObject* function_object,
                           Vector* arguments)
{
  (void)ctx;

  return get_kl_function_arguments_with_count_check(function_object, arguments);
}

void shen_register_defun (shen_context* ctx, const char* name, long arity,
                          NativeFunction* native_function)
{
  KLObject* symbol_object = shen_intern(ctx, name);
  KLObject* function_object =
    create_primitive_kl_function(arity, native_function);

  set_primitive_function_may_trampoline(
    get_kl_function_primitive_function(function_object), 1);
  set_kl_symbol_function(symbol_object, function_object);
}

KLObject* shen_true (shen_context* ctx)
{
  (void)ctx;

  return get_true_boolean_object();
}

KLObject* shen_false (shen_context* ctx)
{
  (void)ctx;

  return get_false_boolean_object();
}

int shen_is_boolean (shen_context* ctx, KLObject* object)
{
  (void)ctx;

  if (object == NULL)
    throw_kl_exception("boolean test on null");

  return is_kl_boolean(object) ? 1 : 0;
}

int shen_boolean_value (shen_context* ctx, KLObject* boolean_object)
{
  (void)ctx;

  return get_boolean(boolean_object) ? 1 : 0;
}

KLObject* shen_native_closure (shen_context* ctx, long arity,
                               NativeFunction* native_function,
                               Vector* captures)
{
  KLObject* function_object =
    create_primitive_kl_function(arity, native_function);
  PrimitiveFunction* primitive_function =
    get_kl_function_primitive_function(function_object);

  (void)ctx;
  set_primitive_function_captures(primitive_function, captures);
  set_primitive_function_may_trampoline(primitive_function, 1);

  return function_object;
}

Vector* shen_native_captures (shen_context* ctx, KLObject* function_object)
{
  Vector* captures;

  (void)ctx;

  if (!is_primitive_kl_function(function_object))
    throw_kl_exception("native captures: not a primitive function");

  captures = get_primitive_function_captures(
    get_kl_function_primitive_function(function_object));

  if (captures == NULL)
    throw_kl_exception("native captures: missing capture vector");

  return captures;
}

static KLObject* apply_user_function (KLObject* function_object, Vector* arguments)
{
  UserFunction* user_function = get_kl_function_user_function(function_object);
  long argument_size = (is_null(arguments)) ? 0 : get_vector_size(arguments);
  Vector* parameters = get_user_function_parameters(user_function);
  long parameter_size = (is_null(parameters)) ? 0 : get_vector_size(parameters);
  Environment* variable_environment = get_global_variable_environment();
  ShenLexMark mark = shen_lex_mark();
  KLObject* result;
  long i;

  check_function_argument_size(argument_size, parameter_size);
  shen_lex_enter_frame();

  if (is_not_null(arguments) && is_not_null(parameters)) {
    variable_environment =
      extend_environment_n(get_vector_objects(parameters),
                           get_vector_objects(arguments),
                           argument_size, variable_environment);

    for (i = 0; i < argument_size; ++i)
      shen_lex_bind(get_vector_element(parameters, i),
                    get_vector_element(arguments, i));
  }

  result = eval_kl_object(get_user_function_body(user_function),
                          get_global_function_environment(), variable_environment);
  shen_lex_rewind(mark);

  return result;
}

static KLObject* apply_closure (KLObject* function_object, Vector* arguments)
{
  Closure* closure = get_kl_function_closure(function_object);
  KLObject* parameter_object = get_closure_parameter(closure);
  long argument_size = (is_null(arguments)) ? 0 : get_vector_size(arguments);
  long parameter_size = (is_null(parameter_object)) ? 0 : 1;
  Environment* function_environment =
    get_closure_parent_function_environment(closure);
  Environment* variable_environment =
    get_closure_parent_variable_environment(closure);
  KLObject* argument_object = NULL;
  ShenLexMark mark = shen_lex_mark();
  KLObject* result;

  check_function_argument_size(argument_size, parameter_size);
  shen_lex_enter_frame();

  if (parameter_size > 0) {
    argument_object = get_vector_element(arguments, 0);
    variable_environment =
      extend_environment(parameter_object, argument_object, variable_environment);
    shen_lex_bind(parameter_object, argument_object);
  }

  result = eval_kl_object(get_closure_body(closure), function_environment,
                          variable_environment);
  shen_lex_rewind(mark);

  return result;
}

static long function_arity (KLObject* function_object)
{
  if (is_primitive_kl_function(function_object))
    return get_primitive_function_parameter_size(
      get_kl_function_primitive_function(function_object));

  if (is_user_kl_function(function_object)) {
    Vector* parameters =
      get_user_function_parameters(get_kl_function_user_function(function_object));

    return is_null(parameters) ? 0 : get_vector_size(parameters);
  }

  if (is_closure_kl_function(function_object))
    return is_null(get_closure_parameter(get_kl_function_closure(function_object)))
      ? 0 : 1;

  return -1;
}

static Vector* vector_slice (Vector* arguments, long start, long count)
{
  Vector* slice = create_vector(count);
  long i;

  for (i = 0; i < count; ++i)
    set_vector_element(slice, i, get_vector_element(arguments, start + i));

  return slice;
}

static KLObject* apply_exact (KLObject* function_object, Vector* arguments)
{
  PrimitiveFunction* primitive_function;
  NativeFunction* native_function;

  /* Hop generated NativeFunctions only. C primitives and certified
   * eval_kl_object user/closure application stay on this C stack. */
  if (is_primitive_kl_function(function_object) &&
      get_primitive_function_may_trampoline(
        get_kl_function_primitive_function(function_object)) &&
      shen_c_stack_low())
    return apply_on_fresh_stack(function_object, arguments);

  if (is_primitive_kl_function(function_object)) {
    primitive_function = get_kl_function_primitive_function(function_object);
    native_function = get_primitive_function_native_function(primitive_function);

    return native_function(function_object, arguments,
                           get_global_function_environment(),
                           get_global_variable_environment());
  }

  if (is_user_kl_function(function_object))
    return apply_user_function(function_object, arguments);

  if (is_closure_kl_function(function_object))
    return apply_closure(function_object, arguments);

  throw_kl_exception("apply expects a function or interned symbol");

  return NULL;
}

static KLObject* native_partial (KLObject* function_object, Vector* arguments,
                                 Environment* function_environment,
                                 Environment* variable_environment)
{
  shen_context* ctx = &shen_root_context;
  Vector* captures = shen_native_captures(ctx, function_object);
  KLObject* target = get_vector_element(captures, 0);
  long prefix = get_vector_size(captures) - 1;
  long extra = is_null(arguments) ? 0 : get_vector_size(arguments);
  Vector* all = create_vector(prefix + extra);
  long i;

  (void)function_environment;
  (void)variable_environment;

  for (i = 0; i < prefix; ++i)
    set_vector_element(all, i, get_vector_element(captures, i + 1));

  for (i = 0; i < extra; ++i)
    set_vector_element(all, prefix + i, get_vector_element(arguments, i));

  return shen_apply(ctx, target, all);
}

static KLObject* apply_dispatch (shen_context* ctx, KLObject* function_or_symbol,
                                 Vector* arguments)
{
  KLObject* function_object = function_or_symbol;
  long have = is_null(arguments) ? 0 : get_vector_size(arguments);
  long want;

  if (is_kl_symbol(function_or_symbol)) {
    function_object = get_kl_symbol_function(function_or_symbol);

    if (is_null(function_object))
      throw_kl_exception("No function bound to symbol");
  }

  want = function_arity(function_object);

  if (want < 0)
    throw_kl_exception("apply expects a function or interned symbol");

  if (have == want)
    return apply_exact(function_object, arguments);

  if (have < want) {
    Vector* captures = create_vector(have + 1);
    long i;

    set_vector_element(captures, 0, function_object);

    for (i = 0; i < have; ++i)
      set_vector_element(captures, i + 1, get_vector_element(arguments, i));

    if (partial_native == NULL)
      partial_native = &native_partial;

    return shen_native_closure(ctx, want - have, partial_native, captures);
  }

  {
    Vector* first = vector_slice(arguments, 0, want);
    KLObject* result = apply_exact(function_object, first);
    Vector* rest = vector_slice(arguments, want, have - want);

    if (bounce_pending) {
      KLObject* fn = bounce_fn;
      Vector* args = bounce_args;

      bounce_pending = 0;
      result = shen_apply(ctx, fn, args);
    }

    return shen_apply(ctx, result, rest);
  }
}

KLObject* shen_apply (shen_context* ctx, KLObject* function_or_symbol,
                      Vector* arguments)
{
  KLObject* result;
  int saved_bounce = bounce_pending;
  KLObject* saved_fn = bounce_fn;
  Vector* saved_args = bounce_args;

  if (function_or_symbol == NULL)
    throw_kl_exception("apply of null");

  apply_depth++;
  bounce_pending = 0;

  for (;;) {
    result = apply_dispatch(ctx, function_or_symbol, arguments);

    if (!bounce_pending) {
      apply_depth--;
      bounce_pending = saved_bounce;
      bounce_fn = saved_fn;
      bounce_args = saved_args;
      return result;
    }

    function_or_symbol = bounce_fn;
    arguments = bounce_args;
    bounce_pending = 0;
  }
}

KLObject* shen_tail_apply (shen_context* ctx, KLObject* function_or_symbol,
                           Vector* arguments)
{
  if (apply_depth > 0) {
    bounce_fn = function_or_symbol;
    bounce_args = arguments;
    bounce_pending = 1;
    return NULL;
  }

  return shen_apply(ctx, function_or_symbol, arguments);
}

static Vector* vector_from_args (long n, KLObject** arguments)
{
  Vector* vector;
  long i;

  if (n <= 0)
    return NULL;

  vector = create_vector(n);

  for (i = 0; i < n; ++i)
    set_vector_element(vector, i, arguments[i]);

  return vector;
}

KLObject* shen_apply_direct (shen_context* ctx, const char* name, long n,
                             KLObject** arguments)
{
  KLObject* symbol_object;
  Vector stack_vector;

  if (n < 0)
    throw_kl_exception("apply_direct negative arity");

  symbol_object = shen_intern_static(ctx, name);
  stack_vector.size = n;
  stack_vector.objects = n == 0 ? NULL : arguments;

  return shen_apply(ctx, symbol_object, &stack_vector);
}

KLObject* shen_tail_apply_direct (shen_context* ctx, const char* name, long n,
                                  KLObject** arguments)
{
  if (apply_depth > 0) {
    bounce_fn = shen_intern_static(ctx, name);
    bounce_args = vector_from_args(n, arguments);
    bounce_pending = 1;
    return NULL;
  }

  return shen_apply_direct(ctx, name, n, arguments);
}

KLObject* shen_trap_error (shen_context* ctx, shen_trap_body body,
                           shen_trap_handler handler, void* data)
{
  jmp_buf jump_buffer;
  int saved_depth = apply_depth;
  int saved_bounce = bounce_pending;
  KLObject* saved_fn = bounce_fn;
  Vector* saved_args = bounce_args;
  ShenLexMark saved_lex = shen_lex_mark();

  (void)ctx;

  if (sigsetjmp(jump_buffer, 0) == 0) {
    KLObject* exception_object = create_kl_exception();
    KLObject* object;

    set_kl_exception_jump_buffer(exception_object, &jump_buffer);
    push_stack(get_trapped_kl_exception_stack(), exception_object);
    object = body(data);
    pop_stack(get_trapped_kl_exception_stack());
    apply_depth = saved_depth;
    bounce_pending = saved_bounce;
    bounce_fn = saved_fn;
    bounce_args = saved_args;
    shen_lex_rewind(saved_lex);

    return object;
  }

  {
    KLObject* exception_object = pop_stack(get_trapped_kl_exception_stack());

    apply_depth = saved_depth;
    bounce_pending = saved_bounce;
    bounce_fn = saved_fn;
    bounce_args = saved_args;
    shen_lex_rewind(saved_lex);

    if (is_null((void*)handler))
      return exception_object;

    return handler(exception_object, data);
  }
}

void shen_simple_error (shen_context* ctx, const char* message)
{
  size_t length = strlen(message);
  char* copy = malloc(length + 1);

  (void)ctx;
  memcpy(copy, message, length + 1);
  throw_kl_exception(copy);
}

KLObject* shen_error_to_string (shen_context* ctx, KLObject* exception_object)
{
  char* error_message =
    get_exception_error_message(get_exception(exception_object));

  (void)ctx;

  return intern_kl_string(error_message);
}

KLObject* shen_eval_kl (shen_context* ctx, KLObject* object)
{
  (void)ctx;

  return eval_kl_object(object, get_global_function_environment(),
                        get_global_variable_environment());
}

#define SHEN_OVERLAY_MAX 16

static const ShenOverlayModule* overlay_modules[SHEN_OVERLAY_MAX];
static int overlay_nmodules = 0;
static char overlay_kernel_dir[4096];
static KLObject* overlay_wrapped_load = NULL;
static int overlay_load_wrapped = 0;
static int overlay_kernel_fnv_ready = 0;
static uint64_t overlay_kernel_fnv_cache = 0;

uint64_t shen_fnv64 (const unsigned char* bytes, size_t n)
{
  uint64_t h = 0xcbf29ce484222325ULL;
  size_t i;

  for (i = 0; i < n; ++i) {
    h ^= (uint64_t)bytes[i];
    h *= 0x100000001b3ULL;
  }

  return h;
}

int shen_fnv64_file (const char* path, uint64_t* hash_out)
{
  FILE* file;
  unsigned char buf[8192];
  size_t n;
  uint64_t h = 0xcbf29ce484222325ULL;

  if (path == NULL || hash_out == NULL)
    return -1;

  file = fopen(path, "rb");

  if (file == NULL)
    return -1;

  while ((n = fread(buf, 1, sizeof(buf), file)) > 0) {
    size_t i;

    for (i = 0; i < n; ++i) {
      h ^= (uint64_t)buf[i];
      h *= 0x100000001b3ULL;
    }
  }

  if (ferror(file)) {
    fclose(file);
    return -1;
  }

  fclose(file);
  *hash_out = h;

  return 0;
}

static int overlay_name_cmp (const void* a, const void* b)
{
  return strcmp(*(char* const*)a, *(char* const*)b);
}

uint64_t shen_kernel_digest (const char* kernel_dir)
{
  DIR* dir;
  struct dirent* ent;
  char* names[512];
  int nnames = 0;
  uint64_t h = 0xcbf29ce484222325ULL;
  int i;

  if (kernel_dir == NULL || kernel_dir[0] == '\0')
    return h;

  dir = opendir(kernel_dir);

  if (dir == NULL)
    return h;

  while ((ent = readdir(dir)) != NULL) {
    const char* name = ent->d_name;
    size_t len;
    char path[4096];
    struct stat st;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;

    len = strlen(name);

    if (len < 4 || strcmp(name + len - 3, ".kl") != 0)
      continue;

    snprintf(path, sizeof(path), "%s/%s", kernel_dir, name);

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      continue;

    if (nnames >= 512)
      break;

    {
      char* copy = malloc(len + 1);

      memcpy(copy, name, len + 1);
      names[nnames++] = copy;
    }
  }

  closedir(dir);
  qsort(names, (size_t)nnames, sizeof(names[0]), overlay_name_cmp);

  for (i = 0; i < nnames; ++i) {
    char path[4096];
    uint64_t file_h = 0xcbf29ce484222325ULL;
    unsigned char le[8];
    int b;
    size_t j;

    {
      const unsigned char* nb = (const unsigned char*)names[i];
      size_t nlen = strlen(names[i]);

      for (j = 0; j < nlen; ++j) {
        h ^= (uint64_t)nb[j];
        h *= 0x100000001b3ULL;
      }
    }

    snprintf(path, sizeof(path), "%s/%s", kernel_dir, names[i]);

    if (shen_fnv64_file(path, &file_h) != 0)
      file_h = shen_fnv64(NULL, 0);

    for (b = 0; b < 8; ++b)
      le[b] = (unsigned char)((file_h >> (8 * b)) & 0xff);

    for (b = 0; b < 8; ++b) {
      h ^= (uint64_t)le[b];
      h *= 0x100000001b3ULL;
    }
  }

  return h;
}

void shen_overlay_set_kernel_dir (const char* kernel_dir)
{
  overlay_kernel_fnv_ready = 0;

  if (kernel_dir == NULL) {
    overlay_kernel_dir[0] = '\0';
    return;
  }

  snprintf(overlay_kernel_dir, sizeof(overlay_kernel_dir), "%s", kernel_dir);
}

void shen_register_overlay (const ShenOverlayModule* module)
{
  int i;

  if (module == NULL)
    return;

  for (i = 0; i < overlay_nmodules; ++i) {
    if (overlay_modules[i] == module)
      return;
  }

  if (overlay_nmodules >= SHEN_OVERLAY_MAX)
    return;

  overlay_modules[overlay_nmodules++] = module;
}

int shen_install_overlay (shen_context* ctx, const ShenOverlayModule* module)
{
  size_t i;

  if (module == NULL)
    return 0;

  for (i = 0; i < module->ncompiled; ++i) {
    KLObject* symbol_object = shen_intern(ctx, module->compiled[i].name);
    KLObject* function_object = shen_symbol_function(ctx, symbol_object);
    long arity;

    if (is_null(function_object) || !is_kl_function(function_object))
      return 0;

    arity = function_arity(function_object);

    if (arity != module->compiled[i].arity)
      return 0;
  }

  if (module->install != NULL)
    module->install(ctx);

  return 1;
}

static const char* overlay_kernel_dir_live (char* fallback, size_t fallback_cap)
{
  char* home;

  if (overlay_kernel_dir[0] != '\0')
    return overlay_kernel_dir;

  home = get_shen_c_home_path();

  if (home == NULL || home[0] == '\0')
    return NULL;

  snprintf(fallback, fallback_cap, "%sshen/src/kl", home);

  return fallback;
}

int shen_install_overlay_if_match (shen_context* ctx,
                                   const ShenOverlayModule* module,
                                   const unsigned char* live_src,
                                   size_t live_len)
{
  char fallback[4096];
  const char* kernel_dir;

  if (module == NULL || module->format == NULL)
    return 0;

  if (strcmp(module->format, SHEN_OVERLAY_FORMAT) != 0)
    return 0;

  if (shen_fnv64(live_src, live_len) != module->source_fnv)
    return 0;

  kernel_dir = overlay_kernel_dir_live(fallback, sizeof(fallback));

  if (kernel_dir == NULL)
    return 0;

  if (!overlay_kernel_fnv_ready) {
    overlay_kernel_fnv_cache = shen_kernel_digest(kernel_dir);
    overlay_kernel_fnv_ready = 1;
  }

  if (overlay_kernel_fnv_cache != module->kernel_fnv)
    return 0;

  return shen_install_overlay(ctx, module);
}

static void overlay_try_path (shen_context* ctx, const char* path)
{
  FILE* file;
  unsigned char* bytes;
  long size;
  int i;

  if (path == NULL)
    return;

  file = fopen(path, "rb");

  if (file == NULL)
    return;

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return;
  }

  size = ftell(file);

  if (size < 0) {
    fclose(file);
    return;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return;
  }

  bytes = malloc((size_t)size + 1);

  if (size > 0 && fread(bytes, 1, (size_t)size, file) != (size_t)size) {
    fclose(file);
    return;
  }

  fclose(file);

  for (i = 0; i < overlay_nmodules; ++i) {
    if (shen_install_overlay_if_match(ctx, overlay_modules[i], bytes,
                                      (size_t)size))
      break;
  }
}

static KLObject* native_overlay_load (KLObject* function_object, Vector* arguments,
                                      Environment* function_environment,
                                      Environment* variable_environment)
{
  shen_context* ctx = &shen_root_context;
  KLObject** objects = shen_arguments(ctx, function_object, arguments);
  KLObject* path_object = objects[0];
  KLObject* result;

  (void)function_environment;
  (void)variable_environment;

  result = shen_apply(ctx, overlay_wrapped_load, arguments);

  if (is_kl_string(path_object))
    overlay_try_path(ctx, get_string(path_object));

  return result;
}

void shen_wrap_load_for_overlays (void)
{
  shen_context* ctx = &shen_root_context;
  KLObject* load_symbol;
  KLObject* current;

  if (overlay_load_wrapped || overlay_nmodules == 0)
    return;

  load_symbol = shen_intern(ctx, "load");
  current = shen_symbol_function(ctx, load_symbol);

  if (is_null(current))
    return;

  overlay_wrapped_load = current;
  {
    KLObject* wrapper = create_primitive_kl_function(1, &native_overlay_load);

    /* Thin post-load hook: do not trampoline the wrapper itself. The
     * captured load native still hops when stack is low. */
    set_kl_symbol_function(load_symbol, wrapper);
  }
  overlay_load_wrapped = 1;
}
