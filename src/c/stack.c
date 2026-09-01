#include "list.h"
#include "stack.h"

extern KLObject* get_stack_top (Stack* stack);
extern void set_stack_top (Stack* stack, KLObject* top_object);
extern long get_stack_size (Stack* stack);
extern void set_stack_size (Stack* stack, long size);
extern Stack* create_stack (void);

void push_stack (Stack* stack, KLObject* object)
{
  KLObject* top_object = get_stack_top(stack);
  long new_stack_size = get_stack_size(stack) + 1;

  set_stack_top(stack, CONS(object, top_object));
  set_stack_size(stack, new_stack_size);
}

KLObject* pop_stack (Stack* stack)
{
  KLObject* top_object = get_stack_top(stack);

  if (is_empty_kl_list(top_object))
    return top_object;

  set_stack_top(stack, CDR(top_object));
  set_stack_size(stack, get_stack_size(stack) - 1);

  return CAR(top_object);
}

KLObject* peek_stack (Stack* stack)
{
  KLObject* top_object = get_stack_top(stack);

  if (is_empty_kl_list(top_object))
    return top_object;

  return CAR(top_object);
}
