#ifndef SHEN_C_KL_H
#define SHEN_C_KL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>

#include "context.h"
#include "khash.h"

#define malloc(n) shen_gc_malloc(&shen_root_context, (n))
#define realloc(p, n) shen_gc_realloc(&shen_root_context, (p), (n))
#define calloc(m, n) shen_gc_malloc(&shen_root_context, (size_t)(m) * (size_t)(n))
#define free(m) GC_free(m)

typedef enum KLType {
  KL_TYPE_SYMBOL, KL_TYPE_STRING, KL_TYPE_NUMBER, KL_TYPE_BOOLEAN,
  KL_TYPE_FUNCTION, KL_TYPE_STREAM, KL_TYPE_EXCEPTION,
  KL_TYPE_LIST, KL_TYPE_VECTOR, KL_TYPE_DICTIONARY
} KLType;

typedef enum KLNumberType {
  KL_NUMBER_TYPE_LONG, KL_NUMBER_TYPE_DOUBLE
} KLNumberType;

typedef enum KLFunctionType {
  KL_FUNCTION_TYPE_PRIMITIVE_FUNCTION, KL_FUNCTION_TYPE_USER_FUNCTION,
  KL_FUNCTION_TYPE_CLOSURE
} KLFunctionType;

typedef enum KLStreamType {
  KL_STREAM_TYPE_IN, KL_STREAM_TYPE_OUT
} KLStreamType;

typedef enum DefunType {
  DEFUN_TYPE_NON_TAIL_CALL, DEFUN_TYPE_POSSIBLY_TAIL_CALL,
  DEFUN_TYPE_TAIL_CALL
} DefunType;

typedef struct KLObject KLObject;
typedef struct Symbol Symbol;
typedef struct Number Number;
typedef struct Function Function;
typedef struct Stream Stream;
typedef struct Exception Exception;
typedef struct Pair Pair;
typedef struct Vector Vector;
typedef struct Dictionary Dictionary;
typedef struct Environment Environment;
typedef struct LoopFramePair LoopFramePair;

typedef void (FunctionDeclaration) (void);
typedef KLObject* (NativeFunction) (KLObject* function_object, Vector* arguments,
                                    Environment* function_environment,
                                    Environment* variable_environment);

struct Number {
  KLNumberType number_type;
  union {
    long number_l;
    double number_d;
  } value;
};

struct Pair {
  KLObject* car;
  KLObject* cdr;
};

struct KLObject {
  KLType type;
  union {
    Symbol* symbol;
    char* string;
    Number number;
    bool boolean;
    Function* function;
    Stream* stream;
    Exception* exception;
    Pair pair;
    Vector* vector;
    Dictionary* dictionary;
  } value;
};

KHASH_MAP_INIT_STR(StringTable, KLObject*)
KHASH_MAP_INIT_STR(StringPairTable, Pair*)

#if UINTPTR_MAX == 0xffffffff
// 32bit
typedef khint_t kl_khint_ptr_t;
KHASH_MAP_INIT_INT(SymbolTable, KLObject*)

#elif UINTPTR_MAX == 0xffffffffffffffff
// 64bit
typedef khint64_t kl_khint_ptr_t;
KHASH_MAP_INIT_INT64(SymbolTable, KLObject*)

#else
#error Could not determine pointer size

#endif

struct Symbol {
  KLObject* name;
  KLObject* function;
  KLObject* variable_value;
  uint32_t id;
};

typedef struct PrimitiveFunction {
  long parameter_size;
  NativeFunction* native_function;
  Vector* captures;
  int may_trampoline;
} PrimitiveFunction;

typedef struct UserFunction {
  Vector* parameters;
  KLObject* body;
} UserFunction;

typedef struct Closure {
  KLObject* parameter;
  KLObject* body;
  Environment* parent_function_environment;
  Environment* parent_variable_environment;
} Closure;

struct Function {
  KLFunctionType function_type;
  union {
    PrimitiveFunction* primitive_function;
    UserFunction* user_function;
    Closure* closure;
  } value ;
};

struct Stream {
  FILE* file;
  KLStreamType stream_type;
};

struct Exception {
  char* error_message;
  jmp_buf* jump_buffer;
};

struct Vector {
  KLObject** objects;
  long size;
};

struct Dictionary {
  khash_t(StringPairTable)* table;
};

typedef struct EnvBinding {
  uint32_t id;
  KLObject* value;
} EnvBinding;

struct Environment {
  Environment* parent;
  uint32_t size;
  uint32_t id;
  KLObject* value;
  EnvBinding* binds;
};

typedef struct Stack {
  KLObject* top;
  long size;
} Stack;

typedef struct LoopFrame {
  Vector* arguments;
  Vector* parameters;
  jmp_buf* jump_buffer;
} LoopFrame;

struct LoopFramePair {
  LoopFrame* car;
  LoopFramePair* cdr;
};

typedef struct LoopFrameStack {
  LoopFramePair* top;
  long size;
} LoopFrameStack;

extern KLObject* empty_list_object;

/* Odd low tags: non-pointer immediates (Boehm ignores them).
 * Even low tags: heap cons/symbol/string. Other heap types stay
 * 8-aligned (tag 0). Nix bdw-gc has interior pointers; also
 * GC_register_displacement(2/4/6) in shen_context_init. */
#define KL_TAG_MASK ((uintptr_t)7)
#define KL_HEAP_TAG ((uintptr_t)0)
#define KL_FIXNUM_TAG ((uintptr_t)1)
#define KL_CONS_TAG ((uintptr_t)2)
#define KL_EMPTY_TAG ((uintptr_t)3)
#define KL_SYMBOL_TAG ((uintptr_t)4)
#define KL_FALSE_TAG ((uintptr_t)5)
#define KL_STRING_TAG ((uintptr_t)6)
#define KL_TRUE_TAG ((uintptr_t)7)
#define KL_FIXNUM_SHIFT 3

inline uintptr_t kl_as_word (KLObject* object)
{
  return (uintptr_t)object;
}

inline KLObject* kl_from_word (uintptr_t word)
{
  return (KLObject*)word;
}

inline KLObject* kl_untag (KLObject* object)
{
  return kl_from_word(kl_as_word(object) & ~KL_TAG_MASK);
}

inline uintptr_t kl_heap_tag (KLType type)
{
  if (type == KL_TYPE_LIST)
    return KL_CONS_TAG;

  if (type == KL_TYPE_SYMBOL)
    return KL_SYMBOL_TAG;

  if (type == KL_TYPE_STRING)
    return KL_STRING_TAG;

  return KL_HEAP_TAG;
}

inline KLObject* kl_tag_heap (KLObject* object, KLType type)
{
  return kl_from_word(kl_as_word(object) | kl_heap_tag(type));
}

inline int kl_is_immediate (KLObject* object)
{
  return (kl_as_word(object) & (uintptr_t)1) != 0;
}

inline int kl_is_fixnum (KLObject* object)
{
  return (kl_as_word(object) & KL_TAG_MASK) == KL_FIXNUM_TAG;
}

inline KLType get_kl_object_type (KLObject* object)
{
  uintptr_t tag = kl_as_word(object) & KL_TAG_MASK;

  if (tag == KL_HEAP_TAG)
    return object->type;

  if (tag == KL_FIXNUM_TAG)
    return KL_TYPE_NUMBER;

  if (tag == KL_CONS_TAG || tag == KL_EMPTY_TAG)
    return KL_TYPE_LIST;

  if (tag == KL_SYMBOL_TAG)
    return KL_TYPE_SYMBOL;

  if (tag == KL_STRING_TAG)
    return KL_TYPE_STRING;

  return KL_TYPE_BOOLEAN;
}

inline void set_kl_object_type (KLObject* object, KLType type)
{
  kl_untag(object)->type = type;
}

inline KLObject* create_kl_object (KLType type)
{
  KLObject* object = shen_gc_malloc(&shen_root_context, sizeof(KLObject));

  object->type = type;

  return kl_tag_heap(object, type);
}

inline KLObject* create_kl_object_atomic (KLType type)
{
  /* Doubles / overflow longs: no internal pointers. Tag 0. */
  KLObject* object = shen_gc_malloc_atomic(&shen_root_context, sizeof(KLObject));

  object->type = type;

  return object;
}

inline bool is_null (void* object)
{
  return object == NULL;
}

inline bool is_not_null (void* object)
{
  return object != NULL;
}

inline KLObject* get_pair_car (Pair* pair)
{
  return pair->car;
}

inline void set_pair_car (Pair* pair, KLObject* object)
{
  pair->car = object;
}

inline KLObject* get_pair_cdr (Pair* pair)
{
  return pair->cdr;
}

inline void set_pair_cdr (Pair* pair, KLObject* object)
{
  pair->cdr = object;
}

inline Pair* create_pair (KLObject* car_object, KLObject* cdr_object)
{
  Pair* pair = malloc(sizeof(Pair));

  set_pair_car(pair, car_object);
  set_pair_cdr(pair, cdr_object);

  return pair;
}

inline void initialize_empty_kl_list (void)
{
  empty_list_object = kl_from_word(KL_EMPTY_TAG);
}

inline KLObject* get_empty_kl_list (void)
{
  return empty_list_object;
}

inline bool is_empty_kl_list (KLObject* object)
{
  return kl_as_word(object) == KL_EMPTY_TAG;
}

#define EL get_empty_kl_list()

#endif
