#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "abi.h"
#include "boolean.h"
#include "exception.h"
#include "function.h"
#include "list.h"
#include "number.h"
#include "stream.h"
#include "string.h"
#include "symbol.h"
#include "tc_cache.h"
#include "vector.h"

/* On-disk / key version. Bump on any keying or stream change.
 * Distinct from rust "shentc3" so cache dirs are not mixed. */
#define TC_FORMAT "shentc3-c"
#define TC_VERSION "shen-c-1"

typedef struct ShenFnv {
  uint64_t h;
} ShenFnv;

typedef struct TcEntry {
  int has_hash;
  uint64_t arg_hash;
  int has_val;
  KLObject* val;
  long gensym_after;
} TcEntry;

typedef struct TcStream {
  TcEntry* entries;
  size_t n;
  size_t cap;
  size_t cursor;
} TcStream;

typedef struct TcLoadCtx {
  uint64_t key;
  uint64_t file_hash;
  long gensym_start;
  long gensym_end_recorded;
  int replay;
  int poisoned;
  ShenFnv digest;
  TcStream tc;
  uint64_t* ckpt;
  size_t nckpt;
  size_t ckpt_cap;
  size_t ckpt_cursor;
  unsigned tc_depth;
} TcLoadCtx;

typedef struct TcState {
  char dir[4096];
  uint64_t chain;
  TcLoadCtx* stack;
  size_t nstack;
  size_t stack_cap;
  int stats_on;
  uint64_t hits;
  uint64_t misses;
} TcState;

static TcState* tc_state = NULL;
static KLObject* tc_orig_load = NULL;
static KLObject* tc_orig_typecheck = NULL;
static KLObject* tc_wrap_load_fn = NULL;
static KLObject* tc_wrap_typecheck_fn = NULL;

static NativeFunction native_tc_load;
static NativeFunction native_tc_typecheck;

static void fnv_init (ShenFnv* h)
{
  h->h = 0xcbf29ce484222325ULL;
}

static void fnv_write (ShenFnv* h, const unsigned char* bytes, size_t n)
{
  size_t i;

  for (i = 0; i < n; ++i) {
    h->h ^= (uint64_t)bytes[i];
    h->h *= 0x100000001b3ULL;
  }
}

static void fnv_u64 (ShenFnv* h, uint64_t v)
{
  unsigned char le[8];
  int i;

  for (i = 0; i < 8; ++i)
    le[i] = (unsigned char)((v >> (8 * i)) & 0xff);

  fnv_write(h, le, 8);
}

static uint64_t fnv_finish (ShenFnv h)
{
  return h.h;
}

static int mkdir_p (const char* path)
{
  char tmp[4096];
  size_t len;
  size_t i;

  if (path == NULL || path[0] == '\0')
    return -1;

  if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
    return -1;

  len = strlen(tmp);

  if (len == 0)
    return -1;

  if (tmp[len - 1] == '/')
    tmp[--len] = '\0';

  for (i = 1; i < len; ++i) {
    if (tmp[i] == '/') {
      tmp[i] = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
      tmp[i] = '/';
    }
  }

  if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
    return -1;

  return 0;
}

static int is_our_native (KLObject* function_object, NativeFunction* native)
{
  if (is_null(function_object) || !is_primitive_kl_function(function_object))
    return 0;

  return get_primitive_function_native_function(
           get_kl_function_primitive_function(function_object)) == native;
}

static long gensym_counter (void)
{
  KLObject* sym = shen_intern(&shen_root_context, "shen.*gensym*");
  KLObject* v = get_kl_symbol_variable_value(sym);

  if (is_null(v) || !is_kl_number_l(v))
    return 0;

  return get_kl_number_number_l(v);
}

static void pin_gensym_forward (long to)
{
  if (to > gensym_counter()) {
    KLObject* sym = shen_intern(&shen_root_context, "shen.*gensym*");

    set_kl_symbol_variable_value(sym, shen_number_l(&shen_root_context, to));
  }
}

static int tc_is_on (int* on_out)
{
  KLObject* sym = shen_intern(&shen_root_context, "shen.*tc*");
  KLObject* v = get_kl_symbol_variable_value(sym);

  if (is_null(v))
    return 0;

  *on_out = !(is_kl_boolean(v) && get_boolean(v) == 0);
  return 1;
}

static uint64_t chain_step (uint64_t chain, uint64_t file_hash, int tc_on)
{
  ShenFnv h;

  fnv_init(&h);
  fnv_u64(&h, chain);
  fnv_u64(&h, file_hash);
  fnv_write(&h, (const unsigned char*)(tc_on ? "+" : "-"), 1);
  return fnv_finish(h);
}

static uint64_t kernel_seed (const char* kernel_dir)
{
  ShenFnv h;

  fnv_init(&h);
  fnv_write(&h, (const unsigned char*)TC_FORMAT, strlen(TC_FORMAT));
  fnv_u64(&h, shen_kernel_digest(kernel_dir));
  return fnv_finish(h);
}

static void poison_top (void)
{
  if (tc_state != NULL && tc_state->nstack > 0)
    tc_state->stack[tc_state->nstack - 1].poisoned = 1;
}

static int hash_value (KLObject* v, ShenFnv* h)
{
  if (is_null(v))
    return 0;

  if (is_non_empty_kl_list(v)) {
    fnv_write(h, (const unsigned char*)"c", 1);
    if (!hash_value(CAR(v), h))
      return 0;
    return hash_value(CDR(v), h);
  }

  if (is_empty_kl_list(v)) {
    fnv_write(h, (const unsigned char*)"n", 1);
    return 1;
  }

  if (is_kl_boolean(v)) {
    fnv_write(h, (const unsigned char*)(get_boolean(v) ? "t" : "u"), 1);
    return 1;
  }

  if (is_kl_number_l(v)) {
    fnv_write(h, (const unsigned char*)"i", 1);
    fnv_u64(h, (uint64_t)get_kl_number_number_l(v));
    return 1;
  }

  if (is_kl_number_d(v)) {
    uint64_t bits;
    double d = get_kl_number_number_d(v);

    memcpy(&bits, &d, sizeof(bits));
    fnv_write(h, (const unsigned char*)"f", 1);
    fnv_u64(h, bits);
    return 1;
  }

  if (is_kl_symbol(v)) {
    char* name = get_string(get_kl_symbol_name(v));
    size_t n;

    if (name == NULL)
      return 0;

    n = strlen(name);
    fnv_write(h, (const unsigned char*)"y", 1);
    fnv_u64(h, (uint64_t)n);
    fnv_write(h, (const unsigned char*)name, n);
    return 1;
  }

  if (is_kl_string(v)) {
    char* s = get_string(v);
    size_t n;

    if (s == NULL)
      return 0;

    n = strlen(s);
    fnv_write(h, (const unsigned char*)"s", 1);
    fnv_u64(h, (uint64_t)n);
    fnv_write(h, (const unsigned char*)s, n);
    return 1;
  }

  return 0;
}

static int hash_args (Vector* arguments, uint64_t* out)
{
  ShenFnv h;
  long n;
  long i;

  fnv_init(&h);
  n = is_null(arguments) ? 0 : get_vector_size(arguments);
  fnv_u64(&h, (uint64_t)n);

  for (i = 0; i < n; ++i) {
    if (!hash_value(get_vector_element(arguments, i), &h))
      return 0;
  }

  *out = fnv_finish(h);
  return 1;
}

typedef struct TcBuf {
  char* data;
  size_t len;
  size_t cap;
} TcBuf;

static void tbuf_init (TcBuf* b)
{
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static void tbuf_grow (TcBuf* b, size_t need)
{
  size_t cap = b->cap == 0 ? 256 : b->cap;

  while (cap < need)
    cap *= 2;

  b->data = realloc(b->data, cap);
  b->cap = cap;
}

static void tbuf_put (TcBuf* b, char c)
{
  if (b->len + 1 >= b->cap)
    tbuf_grow(b, b->len + 2);

  b->data[b->len++] = c;
  b->data[b->len] = '\0';
}

static void tbuf_puts (TcBuf* b, const char* s)
{
  size_t n = strlen(s);

  if (b->len + n + 1 >= b->cap)
    tbuf_grow(b, b->len + n + 2);

  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static void tbuf_printf (TcBuf* b, const char* fmt, ...)
  __attribute__((format(printf, 2, 3)));

static void tbuf_printf (TcBuf* b, const char* fmt, ...)
{
  va_list ap;
  int n;
  char stack[128];

  va_start(ap, fmt);
  n = vsnprintf(stack, sizeof(stack), fmt, ap);
  va_end(ap);

  if (n < 0)
    return;

  if ((size_t)n < sizeof(stack)) {
    tbuf_puts(b, stack);
    return;
  }

  {
    char* heap = malloc((size_t)n + 1);
    va_list ap2;

    va_start(ap2, fmt);
    vsnprintf(heap, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    tbuf_puts(b, heap);
  }
}

static int serialize_value (KLObject* v, TcBuf* out)
{
  if (is_null(v))
    return 0;

  if (is_non_empty_kl_list(v)) {
    tbuf_put(out, 'c');
    if (!serialize_value(CAR(v), out))
      return 0;
    return serialize_value(CDR(v), out);
  }

  if (is_empty_kl_list(v)) {
    tbuf_put(out, 'n');
    return 1;
  }

  if (is_kl_boolean(v)) {
    tbuf_put(out, get_boolean(v) ? 't' : 'u');
    return 1;
  }

  if (is_kl_number_l(v)) {
    tbuf_printf(out, "i%ld;", get_kl_number_number_l(v));
    return 1;
  }

  if (is_kl_number_d(v)) {
    uint64_t bits;
    double d = get_kl_number_number_d(v);

    memcpy(&bits, &d, sizeof(bits));
    tbuf_printf(out, "f%016" PRIx64 ";", bits);
    return 1;
  }

  if (is_kl_symbol(v)) {
    char* name = get_string(get_kl_symbol_name(v));

    if (name == NULL)
      return 0;

    tbuf_printf(out, "y%zu:", strlen(name));
    tbuf_puts(out, name);
    return 1;
  }

  if (is_kl_string(v)) {
    char* s = get_string(v);

    if (s == NULL)
      return 0;

    tbuf_printf(out, "s%zu:", strlen(s));
    tbuf_puts(out, s);
    return 1;
  }

  return 0;
}

typedef struct TcParser {
  const unsigned char* bytes;
  size_t len;
  size_t pos;
} TcParser;

static int p_byte (TcParser* p, unsigned char* out)
{
  if (p->pos >= p->len)
    return 0;

  *out = p->bytes[p->pos++];
  return 1;
}

static int p_take (TcParser* p, size_t n, const unsigned char** out)
{
  if (p->pos + n > p->len)
    return 0;

  *out = p->bytes + p->pos;
  p->pos += n;
  return 1;
}

static int p_number_until (TcParser* p, unsigned char stop, long* out)
{
  size_t start = p->pos;
  char buf[64];
  size_t n;
  char* end = NULL;
  long v;

  while (p->pos < p->len && p->bytes[p->pos] != stop)
    p->pos++;

  if (p->pos >= p->len || p->bytes[p->pos] != stop)
    return 0;

  n = p->pos - start;

  if (n == 0 || n >= sizeof(buf))
    return 0;

  memcpy(buf, p->bytes + start, n);
  buf[n] = '\0';
  p->pos++;
  v = strtol(buf, &end, 10);

  if (end == buf || *end != '\0')
    return 0;

  *out = v;
  return 1;
}

static int parse_atom (TcParser* p, unsigned char tag, KLObject** out);
static int parse_value (TcParser* p, KLObject** out)
{
  KLObject* spine[256];
  int nspine = 0;
  unsigned char tag;

  while (1) {
    if (!p_byte(p, &tag))
      return 0;

    if (tag == 'c') {
      KLObject* car;

      if (nspine >= 256)
        return 0;

      if (!parse_value(p, &car))
        return 0;

      spine[nspine++] = car;
      continue;
    }

    {
      KLObject* v;

      if (!parse_atom(p, tag, &v))
        return 0;

      while (nspine > 0) {
        nspine--;
        v = shen_cons(&shen_root_context, spine[nspine], v);
      }

      *out = v;
      return 1;
    }
  }
}

static int parse_atom (TcParser* p, unsigned char tag, KLObject** out)
{
  shen_context* ctx = &shen_root_context;

  if (tag == 'n') {
    *out = shen_empty_list(ctx);
    return 1;
  }

  if (tag == 't') {
    *out = shen_true(ctx);
    return 1;
  }

  if (tag == 'u') {
    *out = shen_false(ctx);
    return 1;
  }

  if (tag == 'i') {
    long v;

    if (!p_number_until(p, ';', &v))
      return 0;

    *out = shen_number_l(ctx, v);
    return 1;
  }

  if (tag == 'f') {
    const unsigned char* hex;
    char buf[17];
    uint64_t bits;
    double d;
    char* end = NULL;

    if (!p_take(p, 16, &hex))
      return 0;

    memcpy(buf, hex, 16);
    buf[16] = '\0';
    bits = strtoull(buf, &end, 16);

    if (end == buf || *end != '\0')
      return 0;

    if (p->pos >= p->len || p->bytes[p->pos] != ';')
      return 0;

    p->pos++;
    memcpy(&d, &bits, sizeof(d));
    *out = shen_number_d(ctx, d);
    return 1;
  }

  if (tag == 'y' || tag == 's') {
    long len;
    const unsigned char* s;
    char* copy;

    if (!p_number_until(p, ':', &len) || len < 0)
      return 0;

    if (!p_take(p, (size_t)len, &s))
      return 0;

    copy = malloc((size_t)len + 1);
    memcpy(copy, s, (size_t)len);
    copy[len] = '\0';

    if (tag == 'y')
      *out = shen_intern(ctx, copy);
    else
      *out = shen_string(ctx, copy);

    return 1;
  }

  return 0;
}

static int parse_entry (TcParser* p, TcEntry* e)
{
  unsigned char mark;
  unsigned char g;

  memset(e, 0, sizeof(*e));

  if (!p_byte(p, &mark))
    return 0;

  if (mark == '+') {
    const unsigned char* hex;
    char buf[17];
    char* end = NULL;

    if (!p_take(p, 16, &hex))
      return 0;

    memcpy(buf, hex, 16);
    buf[16] = '\0';
    e->arg_hash = strtoull(buf, &end, 16);

    if (end == buf || *end != '\0')
      return 0;

    e->has_hash = 1;
  } else if (mark == '-') {
    e->has_hash = 0;
  } else {
    return 0;
  }

  if (!p_byte(p, &mark))
    return 0;

  if (mark == '+') {
    if (!parse_value(p, &e->val))
      return 0;

    e->has_val = 1;
  } else if (mark == '-') {
    e->has_val = 0;
    e->val = NULL;
  } else {
    return 0;
  }

  if (!e->has_hash && e->has_val)
    return 0;

  if (!p_byte(p, &g) || g != 'g')
    return 0;

  if (!p_number_until(p, ';', &e->gensym_after))
    return 0;

  return 1;
}

typedef struct TcRecording {
  long gensym_start;
  long gensym_end;
  TcEntry* tc;
  size_t ntc;
  uint64_t* ckpt;
  size_t nckpt;
} TcRecording;

static int read_cache (const char* path, TcRecording* rec)
{
  FILE* file;
  unsigned char* bytes = NULL;
  long size;
  TcParser p;
  const char* header = TC_FORMAT " ";
  size_t header_len = strlen(header);
  long ntc;
  long nckpt;
  long i;

  memset(rec, 0, sizeof(*rec));
  file = fopen(path, "rb");

  if (file == NULL)
    return 0;

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 0;
  }

  size = ftell(file);

  if (size < 0) {
    fclose(file);
    return 0;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }

  bytes = malloc((size_t)size + 1);

  if (size > 0 && fread(bytes, 1, (size_t)size, file) != (size_t)size) {
    fclose(file);
    return 0;
  }

  fclose(file);

  if ((size_t)size < header_len ||
      memcmp(bytes, header, header_len) != 0)
    return 0;

  p.bytes = bytes;
  p.len = (size_t)size;
  p.pos = header_len;

  if (!p_number_until(&p, ' ', &rec->gensym_start))
    return 0;

  if (!p_number_until(&p, ' ', &rec->gensym_end))
    return 0;

  if (!p_number_until(&p, ' ', &ntc) || ntc < 0)
    return 0;

  if (!p_number_until(&p, '\n', &nckpt) || nckpt < 0)
    return 0;

  rec->ntc = (size_t)ntc;
  rec->nckpt = (size_t)nckpt;

  if (rec->ntc > 0)
    rec->tc = calloc(rec->ntc, sizeof(TcEntry));

  for (i = 0; i < ntc; ++i) {
    if (!parse_entry(&p, &rec->tc[i]))
      return 0;
  }

  if (rec->nckpt > 0)
    rec->ckpt = calloc(rec->nckpt, sizeof(uint64_t));

  for (i = 0; i < nckpt; ++i) {
    const unsigned char* hex;
    char buf[17];
    char* end = NULL;

    if (!p_take(&p, 16, &hex))
      return 0;

    memcpy(buf, hex, 16);
    buf[16] = '\0';
    rec->ckpt[i] = strtoull(buf, &end, 16);

    if (end == buf || *end != '\0')
      return 0;
  }

  return 1;
}

static void write_entries (const TcEntry* entries, size_t n, TcBuf* out)
{
  size_t i;

  for (i = 0; i < n; ++i) {
    const TcEntry* e = &entries[i];
    TcBuf text;

    if (e->has_hash)
      tbuf_printf(out, "+%016" PRIx64, e->arg_hash);
    else
      tbuf_put(out, '-');

    tbuf_init(&text);

    if (e->has_val && e->has_hash && serialize_value(e->val, &text)) {
      tbuf_put(out, '+');
      tbuf_puts(out, text.data);
    } else {
      tbuf_put(out, '-');
    }

    tbuf_printf(out, "g%ld;", e->gensym_after);
  }
}

static void write_cache (const char* path, const TcLoadCtx* ctx, long gensym_end)
{
  TcBuf out;
  char tmp[4096];
  FILE* file;
  size_t i;

  tbuf_init(&out);
  tbuf_printf(&out, "%s %ld %ld %zu %zu\n", TC_FORMAT, ctx->gensym_start,
              gensym_end, ctx->tc.n, ctx->nckpt);
  write_entries(ctx->tc.entries, ctx->tc.n, &out);

  for (i = 0; i < ctx->nckpt; ++i)
    tbuf_printf(&out, "%016" PRIx64, ctx->ckpt[i]);

  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
    return;

  file = fopen(tmp, "wb");

  if (file == NULL)
    return;

  if (out.len > 0 && fwrite(out.data, 1, out.len, file) != out.len) {
    fclose(file);
    unlink(tmp);
    return;
  }

  fclose(file);

  if (rename(tmp, path) != 0)
    unlink(tmp);
}

static void cache_path (char* out, size_t cap, uint64_t key)
{
  snprintf(out, cap, "%s/%016" PRIx64 ".tc", tc_state->dir, key);
}

static void stream_push (TcStream* s, const TcEntry* e)
{
  if (s->n >= s->cap) {
    size_t cap = s->cap == 0 ? 8 : s->cap * 2;
    s->entries = realloc(s->entries, cap * sizeof(TcEntry));
    s->cap = cap;
  }

  s->entries[s->n++] = *e;
}

static void ckpt_push (TcLoadCtx* ctx, uint64_t v)
{
  if (ctx->nckpt >= ctx->ckpt_cap) {
    size_t cap = ctx->ckpt_cap == 0 ? 4 : ctx->ckpt_cap * 2;
    ctx->ckpt = realloc(ctx->ckpt, cap * sizeof(uint64_t));
    ctx->ckpt_cap = cap;
  }

  ctx->ckpt[ctx->nckpt++] = v;
}

static void stack_push (TcLoadCtx ctx)
{
  if (tc_state->nstack >= tc_state->stack_cap) {
    size_t cap = tc_state->stack_cap == 0 ? 4 : tc_state->stack_cap * 2;
    tc_state->stack = realloc(tc_state->stack, cap * sizeof(TcLoadCtx));
    tc_state->stack_cap = cap;
  }

  tc_state->stack[tc_state->nstack++] = ctx;
}

typedef struct TcCall {
  KLObject* fn;
  Vector* arguments;
  int threw;
  KLObject* exception;
} TcCall;

static KLObject* tc_call_body (void* data)
{
  TcCall* c = data;

  return shen_apply(&shen_root_context, c->fn, c->arguments);
}

static KLObject* tc_call_fail (KLObject* exception, void* data)
{
  TcCall* c = data;

  c->threw = 1;
  c->exception = exception;
  return NULL;
}

static void rethrow_ex (KLObject* exception)
{
  char* msg = get_exception_error_message(get_exception(exception));

  throw_kl_exception(msg);
}

static KLObject* apply_orig_trapped (KLObject* fn, Vector* arguments, int* threw,
                                     KLObject** exception)
{
  TcCall call;

  call.fn = fn;
  call.arguments = arguments;
  call.threw = 0;
  call.exception = NULL;
  *threw = 0;
  *exception = NULL;

  {
    KLObject* r = shen_trap_error(&shen_root_context, tc_call_body, tc_call_fail,
                                  &call);

    if (call.threw) {
      *threw = 1;
      *exception = call.exception;
      return NULL;
    }

    return r;
  }
}

static KLObject* with_depth_orig (Vector* arguments)
{
  TcLoadCtx* top;
  int threw = 0;
  KLObject* exception = NULL;
  KLObject* r;

  if (tc_state->nstack == 0)
    return apply_orig_trapped(tc_orig_typecheck, arguments, &threw, &exception);

  top = &tc_state->stack[tc_state->nstack - 1];
  top->tc_depth++;
  r = apply_orig_trapped(tc_orig_typecheck, arguments, &threw, &exception);
  top = &tc_state->stack[tc_state->nstack - 1];
  if (top->tc_depth > 0)
    top->tc_depth--;

  if (threw)
    rethrow_ex(exception);

  return r;
}

static KLObject* stream_wrapper (Vector* arguments)
{
  int has_hash;
  uint64_t arg_hash = 0;
  TcLoadCtx* top;
  enum { PLAN_PASS, PLAN_RECORD, PLAN_CACHED } plan;
  KLObject* cached = NULL;
  long pin = 0;

  if (tc_state == NULL || tc_state->nstack == 0)
    return with_depth_orig(arguments);

  has_hash = hash_args(arguments, &arg_hash);
  top = &tc_state->stack[tc_state->nstack - 1];

  if (top->poisoned || top->tc_depth > 0) {
    plan = PLAN_PASS;
  } else if (top->replay) {
    if (top->tc.cursor < top->tc.n) {
      TcEntry* e = &top->tc.entries[top->tc.cursor];
      int hash_match = (e->has_hash == has_hash) &&
                       (!has_hash || e->arg_hash == arg_hash);

      if (hash_match) {
        top->tc.cursor++;
        fnv_u64(&top->digest, has_hash ? arg_hash : 0);
        if (e->has_val) {
          plan = PLAN_CACHED;
          cached = e->val;
          pin = e->gensym_after;
        } else {
          plan = PLAN_PASS;
        }
      } else {
        top->poisoned = 1;
        plan = PLAN_PASS;
      }
    } else {
      top->poisoned = 1;
      plan = PLAN_PASS;
    }
  } else {
    plan = PLAN_RECORD;
  }

  if (plan == PLAN_CACHED) {
    pin_gensym_forward(pin);
    return cached;
  }

  if (plan == PLAN_PASS)
    return with_depth_orig(arguments);

  {
    KLObject* r = with_depth_orig(arguments);
    int cacheable = !(is_kl_boolean(r) && get_boolean(r) == 0);
    TcEntry e;

    memset(&e, 0, sizeof(e));
    e.has_hash = has_hash;
    e.arg_hash = arg_hash;
    e.has_val = has_hash && cacheable;
    e.val = e.has_val ? r : NULL;
    e.gensym_after = gensym_counter();

    if (tc_state != NULL && tc_state->nstack > 0) {
      top = &tc_state->stack[tc_state->nstack - 1];
      fnv_u64(&top->digest, has_hash ? arg_hash : 0);
      stream_push(&top->tc, &e);
    }

    return r;
  }
}

static KLObject* native_tc_typecheck (KLObject* function_object, Vector* arguments,
                                      Environment* function_environment,
                                      Environment* variable_environment)
{
  (void)function_object;
  (void)function_environment;
  (void)variable_environment;

  if (is_null(tc_orig_typecheck))
    throw_kl_exception("tc-cache: shen.typecheck missing");

  return stream_wrapper(arguments);
}

static void describe_load (const char* path, const TcLoadCtx* ctx)
{
  size_t served;
  size_t i;

  if (ctx->replay)
    served = ctx->tc.cursor;
  else {
    served = 0;
    for (i = 0; i < ctx->tc.n; ++i)
      if (ctx->tc.entries[i].has_val)
        served++;
  }

  fprintf(stderr, "tc-cache: %s: %s tc %zu/%zu%s\n", path,
          ctx->replay ? "replayed" : "recorded", served, ctx->tc.n,
          ctx->poisoned ? " (POISONED)" : "");
}

static KLObject* native_tc_load (KLObject* function_object, Vector* arguments,
                                 Environment* function_environment,
                                 Environment* variable_environment)
{
  shen_context* ctx = &shen_root_context;
  KLObject** objects;
  KLObject* path_object;
  const char* path;
  uint64_t file_hash = 0;
  int tc_on = 0;
  long live_gensym;
  uint64_t key;
  char cpath[4096];
  TcLoadCtx load_ctx;
  int threw = 0;
  KLObject* exception = NULL;
  KLObject* result;
  int stats_on;

  (void)function_environment;
  (void)variable_environment;

  if (tc_state == NULL || is_null(tc_orig_load))
    throw_kl_exception("tc-cache: load wrapper without state");

  objects = shen_arguments(ctx, function_object, arguments);
  path_object = objects[0];

  if (!is_kl_string(path_object)) {
    poison_top();
    result = apply_orig_trapped(tc_orig_load, arguments, &threw, &exception);
    if (threw)
      rethrow_ex(exception);
    return result;
  }

  path = get_string(path_object);

  if (shen_fnv64_file(path, &file_hash) != 0) {
    poison_top();
    result = apply_orig_trapped(tc_orig_load, arguments, &threw, &exception);
    if (threw)
      rethrow_ex(exception);
    return result;
  }

  if (!tc_is_on(&tc_on)) {
    poison_top();
    result = apply_orig_trapped(tc_orig_load, arguments, &threw, &exception);
    if (threw)
      rethrow_ex(exception);
    return result;
  }

  live_gensym = gensym_counter();

  {
    ShenFnv h;
    size_t i;

    fnv_init(&h);
    fnv_write(&h, (const unsigned char*)TC_FORMAT, strlen(TC_FORMAT));
    fnv_write(&h, (const unsigned char*)TC_VERSION, strlen(TC_VERSION));
    fnv_u64(&h, tc_state->chain);

    for (i = 0; i < tc_state->nstack; ++i) {
      fnv_u64(&h, tc_state->stack[i].file_hash);
      fnv_u64(&h, fnv_finish(tc_state->stack[i].digest));
    }

    fnv_u64(&h, file_hash);
    fnv_write(&h, (const unsigned char*)(tc_on ? "+" : "-"), 1);
    fnv_u64(&h, (uint64_t)live_gensym);
    key = fnv_finish(h);
    stats_on = tc_state->stats_on;
  }

  cache_path(cpath, sizeof(cpath), key);
  memset(&load_ctx, 0, sizeof(load_ctx));
  load_ctx.key = key;
  load_ctx.file_hash = file_hash;
  fnv_init(&load_ctx.digest);

  {
    TcRecording rec;

    if (read_cache(cpath, &rec)) {
      pin_gensym_forward(rec.gensym_start);
      tc_state->hits++;
      load_ctx.gensym_start = live_gensym > rec.gensym_start ? live_gensym
                                                            : rec.gensym_start;
      load_ctx.gensym_end_recorded = rec.gensym_end;
      load_ctx.replay = 1;
      load_ctx.tc.entries = rec.tc;
      load_ctx.tc.n = rec.ntc;
      load_ctx.tc.cap = rec.ntc;
      load_ctx.ckpt = rec.ckpt;
      load_ctx.nckpt = rec.nckpt;
      load_ctx.ckpt_cap = rec.nckpt;
    } else {
      tc_state->misses++;
      load_ctx.gensym_start = live_gensym;
      load_ctx.replay = 0;
    }
  }

  stack_push(load_ctx);
  result = apply_orig_trapped(tc_orig_load, arguments, &threw, &exception);

  {
    TcLoadCtx done = tc_state->stack[--tc_state->nstack];
    uint64_t new_chain;

    if (threw) {
      poison_top();
      rethrow_ex(exception);
    }

    new_chain = chain_step(tc_state->chain, file_hash, tc_on);
    tc_state->chain = new_chain;

    if (done.replay)
      pin_gensym_forward(done.gensym_end_recorded);
    else if (!done.poisoned)
      write_cache(cpath, &done, gensym_counter());

    if (tc_state->nstack > 0) {
      TcLoadCtx* parent = &tc_state->stack[tc_state->nstack - 1];

      fnv_u64(&parent->digest, new_chain);

      if (parent->replay) {
        if (parent->ckpt_cursor < parent->nckpt &&
            parent->ckpt[parent->ckpt_cursor] == new_chain)
          parent->ckpt_cursor++;
        else
          parent->poisoned = 1;
      } else {
        ckpt_push(parent, new_chain);
      }
    }

    if (stats_on)
      describe_load(path, &done);
  }

  return result;
}

static void wrap_symbol (const char* name, long arity, NativeFunction* native,
                         KLObject** orig_slot, KLObject** wrap_slot)
{
  shen_context* ctx = &shen_root_context;
  KLObject* symbol = shen_intern(ctx, name);
  KLObject* current = shen_symbol_function(ctx, symbol);

  if (is_null(current))
    return;

  if (is_our_native(current, native))
    return;

  *orig_slot = current;

  if (is_null(*wrap_slot)) {
    KLObject* wrapper = create_primitive_kl_function(arity, native);

    *wrap_slot = wrapper;
  }

  set_kl_symbol_function(symbol, *wrap_slot);
}

static const char* kernel_dir_live (char* fallback, size_t fallback_cap)
{
  char* home = get_shen_c_home_path();

  if (home == NULL || home[0] == '\0')
    return "shen/src/kl";

  snprintf(fallback, fallback_cap, "%sshen/src/kl", home);
  return fallback;
}

void shen_tc_cache_install (const char* dir, int stats_on,
                            const char* kernel_dir)
{
  char fallback[4096];
  const char* kd = kernel_dir;

  if (dir == NULL || dir[0] == '\0')
    return;

  if (mkdir_p(dir) != 0) {
    fprintf(stderr, "tc-cache: create %s — disabled\n", dir);
    return;
  }

  if (kd == NULL || kd[0] == '\0')
    kd = kernel_dir_live(fallback, sizeof(fallback));

  if (tc_state == NULL)
    tc_state = calloc(1, sizeof(TcState));

  snprintf(tc_state->dir, sizeof(tc_state->dir), "%s", dir);
  tc_state->chain = kernel_seed(kd);
  tc_state->nstack = 0;
  tc_state->stats_on = stats_on;
  tc_state->hits = 0;
  tc_state->misses = 0;

  wrap_symbol("load", 1, &native_tc_load, &tc_orig_load, &tc_wrap_load_fn);
  wrap_symbol("shen.typecheck", 2, &native_tc_typecheck, &tc_orig_typecheck,
              &tc_wrap_typecheck_fn);
}

void shen_tc_cache_install_from_env (void)
{
  const char* dir = getenv("SHEN_C_TC_CACHE");
  const char* stats = getenv("SHEN_C_TC_CACHE_STATS");

  if (dir == NULL || dir[0] == '\0')
    return;

  shen_tc_cache_install(dir, stats != NULL && stats[0] != '\0', NULL);
}

int shen_tc_cache_stats (uint64_t* hits, uint64_t* misses)
{
  if (tc_state == NULL)
    return 0;

  if (hits != NULL)
    *hits = tc_state->hits;

  if (misses != NULL)
    *misses = tc_state->misses;

  return 1;
}
