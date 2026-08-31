#include "context.h"

shen_context shen_root_context = {0};

extern void shen_context_init (shen_context* ctx);
extern void* shen_gc_malloc (shen_context* ctx, size_t size);
extern void* shen_gc_realloc (shen_context* ctx, void* pointer, size_t size);
