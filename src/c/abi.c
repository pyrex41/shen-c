#include <pthread.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
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
#include "string.h"
#include "symbol.h"

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
}

KLObject* shen_intern (shen_context* ctx, const char* name)
{
  KLObject* string_object = intern_kl_string(name);

  (void)ctx;

  if (is_kl_string_equal(string_object, true_string_object))
    return get_true_boolean_object();
  else if (is_kl_string_equal(string_object, false_string_object))
    return get_false_boolean_object();

  KLObject* symbol_object = lookup_symbol_table(string_object);

  if (is_null(symbol_object)) {
    symbol_object = create_kl_symbol(string_object);
    extend_symbol_table(string_object, symbol_object);
  }

  return symbol_object;
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
  Environment* function_environment = create_environment();
  Environment* variable_environment = create_environment();

  check_function_argument_size(argument_size, parameter_size);
  set_parent_environment(function_environment, get_global_function_environment());
  set_parent_environment(variable_environment, get_global_variable_environment());

  if (is_not_null(arguments) && is_not_null(parameters)) {
    KLObject** argument_objects = get_vector_objects(arguments);
    KLObject** parameter_objects = get_vector_objects(parameters);
    long i;

    for (i = 0; i < argument_size; ++i)
      extend_environment(parameter_objects[i], argument_objects[i],
                         variable_environment);
  }

  return eval_kl_object(get_user_function_body(user_function),
                        function_environment, variable_environment);
}

static KLObject* apply_closure (KLObject* function_object, Vector* arguments)
{
  Closure* closure = get_kl_function_closure(function_object);
  KLObject* parameter_object = get_closure_parameter(closure);
  long argument_size = (is_null(arguments)) ? 0 : get_vector_size(arguments);
  long parameter_size = (is_null(parameter_object)) ? 0 : 1;
  Environment* function_environment = create_environment();
  Environment* variable_environment = create_environment();
  KLObject* argument_object = NULL;

  check_function_argument_size(argument_size, parameter_size);
  set_parent_environment(function_environment,
                         get_closure_parent_function_environment(closure));
  set_parent_environment(variable_environment,
                         get_closure_parent_variable_environment(closure));

  if (parameter_size > 0) {
    argument_object = get_vector_element(arguments, 0);
    extend_environment(parameter_object, argument_object, variable_environment);
  }

  return eval_kl_object(get_closure_body(closure), function_environment,
                        variable_environment);
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

KLObject* shen_trap_error (shen_context* ctx, shen_trap_body body,
                           shen_trap_handler handler, void* data)
{
  jmp_buf jump_buffer;
  int saved_depth = apply_depth;
  int saved_bounce = bounce_pending;
  KLObject* saved_fn = bounce_fn;
  Vector* saved_args = bounce_args;

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

    return object;
  }

  {
    KLObject* exception_object = pop_stack(get_trapped_kl_exception_stack());

    apply_depth = saved_depth;
    bounce_pending = saved_bounce;
    bounce_fn = saved_fn;
    bounce_args = saved_args;

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
