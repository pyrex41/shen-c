#ifndef SHEN_C_BOOLEAN_H
#define SHEN_C_BOOLEAN_H

#include <stdbool.h>

#include "kl.h"

extern KLObject* true_boolean_object;
extern KLObject* false_boolean_object;

inline bool get_boolean (KLObject* boolean_object)
{
  return kl_as_word(boolean_object) == KL_TRUE_TAG;
}

inline KLObject* create_kl_boolean (bool boolean)
{
  return kl_from_word(boolean ? KL_TRUE_TAG : KL_FALSE_TAG);
}

inline void initialize_true_boolean_object (void)
{
  true_boolean_object = kl_from_word(KL_TRUE_TAG);
}

inline void initialize_false_boolean_object (void)
{
  false_boolean_object = kl_from_word(KL_FALSE_TAG);
}

inline void initialize_boolean_objects (void)
{
  initialize_true_boolean_object();
  initialize_false_boolean_object();
}

inline KLObject* get_true_boolean_object (void)
{
  return true_boolean_object;
}

inline KLObject* get_false_boolean_object (void)
{
  return false_boolean_object;
}

inline bool is_kl_boolean (KLObject* object)
{
  uintptr_t tag = kl_as_word(object) & KL_TAG_MASK;

  return tag == KL_FALSE_TAG || tag == KL_TRUE_TAG;
}

inline char* kl_boolean_to_string (KLObject* boolean_object)
{
  return (get_boolean(boolean_object)) ? "true" : "false";
}

inline bool is_kl_boolean_equal (KLObject* left_object, KLObject* right_object)
{
  return left_object == right_object;
}

#endif
