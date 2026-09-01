#ifndef SHEN_C_NUMBER_H
#define SHEN_C_NUMBER_H

#include <stdbool.h>
#include <float.h>

#include "character.h"
#include "exception.h"
#include "kl.h"

KLObject* subtract_kl_number (KLObject* k, KLObject* l);
KLObject* multiply_kl_number (KLObject* k, KLObject* l);
KLObject* divide_kl_number (KLObject* k, KLObject* l);
bool is_kl_number_equal (KLObject* k, KLObject* l);
bool is_kl_number_greater (KLObject* k, KLObject* l);
bool is_kl_number_less (KLObject* k, KLObject* l);
bool is_kl_number_greater_or_equal (KLObject* k, KLObject* l);
bool is_kl_number_less_or_equal (KLObject* k, KLObject* l);

unsigned long count_unsigned_digits_length (unsigned long x);

inline KLNumberType get_number_number_type (Number* number)
{
  return number->number_type;
}

inline void set_number_number_type (Number* number, KLNumberType number_type)
{
  number->number_type = number_type;
}

inline long get_number_number_l (Number* number)
{
  return number->value.number_l;
}

inline void set_number_number_l (Number* number, long x)
{
  number->value.number_l = x;
}

inline double get_number_number_d (Number* number)
{
  return number->value.number_d;
}

inline void set_number_number_d (Number* number, double x)
{
  number->value.number_d = x;
}

inline Number* create_number_l (long x)
{
  Number* number = malloc(sizeof(Number));

  set_number_number_type(number, KL_NUMBER_TYPE_LONG);
  set_number_number_l(number, x);

  return number;
}

inline Number* create_number_d (double x)
{
  Number* number = malloc(sizeof(Number));

  set_number_number_type(number, KL_NUMBER_TYPE_DOUBLE);
  set_number_number_d(number, x);

  return number;
}

inline Number* get_number (KLObject* number_object)
{
  return &kl_untag(number_object)->value.number;
}

inline void set_number (KLObject* number_object, Number* number)
{
  kl_untag(number_object)->value.number = *number;
}

inline int kl_fixnum_fits (long x)
{
  const int bits = (int)(sizeof(intptr_t) * 8 - KL_FIXNUM_SHIFT);
  long max = (long)(((uintptr_t)1 << (bits - 1)) - 1);
  long min = -max - 1;

  return x >= min && x <= max;
}

inline long kl_fixnum_value (KLObject* object)
{
  return (long)((intptr_t)kl_as_word(object) >> KL_FIXNUM_SHIFT);
}

inline KLObject* kl_make_fixnum (long x)
{
  return kl_from_word(((uintptr_t)(intptr_t)x << KL_FIXNUM_SHIFT) |
                      KL_FIXNUM_TAG);
}

inline KLObject* create_kl_number_l (long x)
{
  if (kl_fixnum_fits(x))
    return kl_make_fixnum(x);

  /* Overflow longs stay pointer-free atomic heap cells. */
  KLObject* number_object = create_kl_object_atomic(KL_TYPE_NUMBER);

  kl_untag(number_object)->value.number.number_type = KL_NUMBER_TYPE_LONG;
  kl_untag(number_object)->value.number.value.number_l = x;

  return number_object;
}

inline KLObject* create_kl_number_d (double x)
{
  KLObject* number_object = create_kl_object_atomic(KL_TYPE_NUMBER);

  kl_untag(number_object)->value.number.number_type = KL_NUMBER_TYPE_DOUBLE;
  kl_untag(number_object)->value.number.value.number_d = x;

  return number_object;
}

inline long get_kl_number_number_l (KLObject* number_object)
{
  if (kl_is_fixnum(number_object))
    return kl_fixnum_value(number_object);

  return get_number_number_l(get_number(number_object));
}

inline double get_kl_number_number_d (KLObject* number_object)
{
  return get_number_number_d(get_number(number_object));
}

inline KLNumberType get_kl_number_number_type (KLObject* number_object)
{
  if (kl_is_fixnum(number_object))
    return KL_NUMBER_TYPE_LONG;

  return get_number_number_type(get_number(number_object));
}

inline bool is_kl_number (KLObject* object)
{
  return get_kl_object_type(object) == KL_TYPE_NUMBER;
}

inline bool is_kl_number_l (KLObject* object)
{
  if (kl_is_fixnum(object))
    return true;

  return (is_kl_number(object) &&
          get_kl_number_number_type(object) == KL_NUMBER_TYPE_LONG);
}

inline bool is_kl_number_d (KLObject* object)
{
  return (is_kl_number(object) &&
          get_kl_number_number_type(object) == KL_NUMBER_TYPE_DOUBLE);
}

inline KLObject* kl_number_l_to_kl_number_d (KLObject* number_object)
{
  return create_kl_number_d((double)get_kl_number_number_l(number_object));
}

inline KLObject* kl_number_d_to_kl_number_l (KLObject* number_object)
{
  return create_kl_number_l((long)get_kl_number_number_d(number_object));
}

inline long add_number_l_l (long x, long y) { return x + y; }
inline double add_number_l_d (long x, double y) { return (double)x + y; }
inline double add_number_d_l (double x, long y) { return x + (double)y; }
inline double add_number_d_d (double x, double y) { return x + y; }

inline KLObject* add_kl_number_l_l (KLObject* k, KLObject* l)
{
  long x = add_number_l_l(get_kl_number_number_l(k), get_kl_number_number_l(l));

  return  create_kl_number_l(x);
}

inline KLObject* add_kl_number_l_d (KLObject* k, KLObject* l)
{
  double x = add_number_l_d(get_kl_number_number_l(k), get_kl_number_number_d(l));

  return create_kl_number_d(x);
}

inline KLObject* add_kl_number_d_l (KLObject* k, KLObject* l)
{
  double x = add_number_d_l(get_kl_number_number_d(k), get_kl_number_number_l(l));

  return create_kl_number_d(x);
}

inline KLObject* add_kl_number_d_d (KLObject* k, KLObject* l)
{
  double x = add_number_d_d(get_kl_number_number_d(k), get_kl_number_number_d(l));

  return create_kl_number_d(x);
}

inline KLObject* add_kl_number (KLObject* k, KLObject* l)
{
  if (is_kl_number_l(k)) {
    if (is_kl_number_l(l))
      return add_kl_number_l_l(k, l);

    return add_kl_number_l_d(k, l);
  }

  if (is_kl_number_l(l))
    return add_kl_number_d_l(k, l);

  return add_kl_number_d_d(k, l);
}

#endif
