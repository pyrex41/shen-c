#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boolean.h"
#include "emit.h"
#include "list.h"
#include "number.h"
#include "reader.h"
#include "stream.h"
#include "string.h"
#include "symbol.h"
#include "symbol_pool.h"

typedef struct CBuf {
  char* data;
  size_t len;
  size_t cap;
} CBuf;

typedef struct Bind {
  char name[256];
  int temp;
} Bind;

typedef struct DefunRec {
  char name[256];
  char cname[256];
  long arity;
} DefunRec;

typedef struct Emit {
  CBuf helpers;
  CBuf defuns;
  CBuf *stmt;
  Bind binds[256];
  int nbind;
  DefunRec defun_recs[4096];
  int ndefun;
  int kernel_tops[4096];
  int nkernel_tops;
  int user_tops[4096];
  int nuser_tops;
  int temp;
  int lambda_id;
  int trap_id;
  int toplevel_id;
  long defun_count;
  long toplevel_count;
  long lambda_count;
  int in_user;
  char self_name[256];
  char self_cname[256];
  int self_params[256];
  int nself_params;
  int self_arity;
  int allow_self_goto;
} Emit;

static void cbuf_init (CBuf* b)
{
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static void cbuf_grow (CBuf* b, size_t need)
{
  size_t cap;

  if (b->len + need + 1 <= b->cap)
    return;

  cap = b->cap ? b->cap : 256;

  while (cap < b->len + need + 1)
    cap *= 2;

  b->data = realloc(b->data, cap);
  b->cap = cap;

  if (b->data == NULL)
    throw_kl_exception("emit buffer realloc failed");
}

static void cbuf_printf (CBuf* b, const char* fmt, ...)
{
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  if (n < 0)
    throw_kl_exception("emit vsnprintf failed");

  cbuf_grow(b, (size_t)n);
  va_start(ap, fmt);
  vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
}

static void cbuf_cstring (CBuf* b, const char* s)
{
  cbuf_printf(b, "\"");

  for (; s != NULL && *s != '\0'; ++s) {
    unsigned char c = (unsigned char)*s;

    if (c == '\\' || c == '"')
      cbuf_printf(b, "\\%c", c);
    else if (c == '\n')
      cbuf_printf(b, "\\n");
    else if (c == '\r')
      cbuf_printf(b, "\\r");
    else if (c == '\t')
      cbuf_printf(b, "\\t");
    else if (c < 32 || c >= 127)
      cbuf_printf(b, "\\%03o", c);
    else
      cbuf_printf(b, "%c", c);
  }

  cbuf_printf(b, "\"");
}

static int is_named_symbol (KLObject* object, const char* name)
{
  return is_kl_symbol(object) &&
    strcmp(get_string(get_kl_symbol_name(object)), name) == 0;
}

static const char* symbol_name (KLObject* object)
{
  return get_string(get_kl_symbol_name(object));
}

static int lookup_bind (Emit* e, const char* name)
{
  int i;

  for (i = e->nbind - 1; i >= 0; --i) {
    if (strcmp(e->binds[i].name, name) == 0)
      return e->binds[i].temp;
  }

  return -1;
}

static void push_bind (Emit* e, const char* name, int temp)
{
  if (e->nbind >= 256)
    throw_kl_exception("emit binding stack overflow");

  snprintf(e->binds[e->nbind].name, sizeof(e->binds[e->nbind].name), "%s", name);
  e->binds[e->nbind].temp = temp;
  e->nbind++;
}

static int fresh (Emit* e)
{
  return e->temp++;
}

static void sanitize_ident (const char* name, char* out, size_t cap)
{
  static const char* keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while", "bool", "true", "false",
    "main", NULL
  };
  size_t n = 0;
  size_t i;
  const char* p;

  if (cap < 8) {
    out[0] = '\0';
    return;
  }

  out[n++] = 'k';
  out[n++] = 'l';
  out[n++] = '_';

  for (p = name; *p != '\0' && n + 4 < cap; ++p) {
    unsigned char c = (unsigned char)*p;

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
      out[n++] = (char)c;
    } else {
      static const char hex[] = "0123456789abcdef";

      out[n++] = '_';
      out[n++] = hex[c >> 4];
      out[n++] = hex[c & 15];
    }
  }

  out[n] = '\0';

  for (i = 0; keywords[i] != NULL; ++i) {
    if (strcmp(out, keywords[i]) == 0) {
      snprintf(out, cap, "klx_%s", keywords[i]);
      break;
    }
  }
}

static int emit_expr (Emit* e, KLObject* expr, int tail);

static int emit_value (Emit* e, KLObject* expr)
{
  return emit_expr(e, expr, 0);
}

static int emit_atom (Emit* e, KLObject* expr)
{
  int t = fresh(e);

  if (is_empty_kl_list(expr)) {
    cbuf_printf(e->stmt, "  KLObject* t%d = shen_empty_list(ctx);\n", t);
    return t;
  }

  if (is_kl_boolean(expr)) {
    cbuf_printf(e->stmt, "  KLObject* t%d = %s(ctx);\n", t,
                get_boolean(expr) ? "shen_true" : "shen_false");
    return t;
  }

  if (is_kl_number_l(expr)) {
    cbuf_printf(e->stmt, "  KLObject* t%d = shen_number_l(ctx, %ld);\n",
                t, get_kl_number_number_l(expr));
    return t;
  }

  if (is_kl_number_d(expr)) {
    cbuf_printf(e->stmt, "  KLObject* t%d = shen_number_d(ctx, %.17g);\n",
                t, get_kl_number_number_d(expr));
    return t;
  }

  if (is_kl_string(expr)) {
    cbuf_printf(e->stmt, "  KLObject* t%d = shen_string(ctx, ", t);
    cbuf_cstring(e->stmt, get_string(expr));
    cbuf_printf(e->stmt, ");\n");
    return t;
  }

  if (is_kl_symbol(expr)) {
    const char* name = symbol_name(expr);
    int bound = lookup_bind(e, name);

    if (bound >= 0)
      return bound;

    cbuf_printf(e->stmt, "  KLObject* t%d = shen_intern(ctx, ", t);
    cbuf_cstring(e->stmt, name);
    cbuf_printf(e->stmt, ");\n");
    return t;
  }

  throw_kl_exception("emit: unsupported atom");
  return t;
}

static int emit_if (Emit* e, KLObject* list, int tail)
{
  int t;
  int test;
  int then_t;
  int else_t;

  if (get_kl_list_size(list) != 4)
    throw_kl_exception("emit: if arity");

  t = fresh(e);
  test = emit_value(e, CADR(list));
  cbuf_printf(e->stmt, "  KLObject* t%d;\n", t);
  cbuf_printf(e->stmt,
              "  if (!shen_is_boolean(ctx, t%d))\n"
              "    shen_simple_error(ctx, \"Test should be a boolean value\");\n"
              "  if (shen_boolean_value(ctx, t%d)) {\n",
              test, test);
  then_t = emit_expr(e, CADDR(list), tail);
  cbuf_printf(e->stmt, "    t%d = t%d;\n  } else {\n", t, then_t);
  else_t = emit_expr(e, CADDDR(list), tail);
  cbuf_printf(e->stmt, "    t%d = t%d;\n  }\n", t, else_t);

  return t;
}

static int emit_and_or (Emit* e, KLObject* list, int is_and, int tail)
{
  int t;
  int left;
  int right;

  if (get_kl_list_size(list) != 3)
    throw_kl_exception("emit: and/or arity");

  t = fresh(e);
  left = emit_value(e, CADR(list));
  cbuf_printf(e->stmt, "  KLObject* t%d;\n", t);
  cbuf_printf(e->stmt,
              "  if (!shen_is_boolean(ctx, t%d))\n"
              "    shen_simple_error(ctx, \"Arguments should be boolean values\");\n",
              left);

  if (is_and)
    cbuf_printf(e->stmt, "  if (!shen_boolean_value(ctx, t%d)) {\n    t%d = t%d;\n  } else {\n",
                left, t, left);
  else
    cbuf_printf(e->stmt, "  if (shen_boolean_value(ctx, t%d)) {\n    t%d = t%d;\n  } else {\n",
                left, t, left);

  /* Right-hand and/or must stay non-tail: the boolean check reads the
   * result, and shen_tail_apply returns NULL when it bounces. */
  (void)tail;
  right = emit_value(e, CADDR(list));
  cbuf_printf(e->stmt,
              "    if (!shen_is_boolean(ctx, t%d))\n"
              "      shen_simple_error(ctx, \"Arguments should be boolean values\");\n"
              "    t%d = t%d;\n  }\n",
              right, t, right);

  return t;
}

static int emit_cond (Emit* e, KLObject* list, int tail)
{
  int t = fresh(e);
  KLObject* cases = CDR(list);

  if (is_empty_kl_list(cases))
    throw_kl_exception("emit: empty cond");

  cbuf_printf(e->stmt, "  KLObject* t%d;\n", t);
  cbuf_printf(e->stmt, "  {\n");

  while (!is_empty_kl_list(cases)) {
    KLObject* clause = CAR(cases);
    int test;
    int then_t;

    if (!is_kl_list(clause) || get_kl_list_size(clause) != 2)
      throw_kl_exception("emit: cond clause");

    test = emit_value(e, CAR(clause));
    cbuf_printf(e->stmt,
                "    if (!shen_is_boolean(ctx, t%d))\n"
                "      shen_simple_error(ctx, \"Case test should be a boolean value\");\n"
                "    if (shen_boolean_value(ctx, t%d)) {\n",
                test, test);
    then_t = emit_expr(e, CADR(clause), tail);
    cbuf_printf(e->stmt, "      t%d = t%d;\n    } else {\n", t, then_t);
    cases = CDR(cases);
  }

  cbuf_printf(e->stmt,
              "      shen_simple_error(ctx, \"No true case found for cond\");\n"
              "      t%d = shen_empty_list(ctx);\n",
              t);

  cases = CDR(list);

  while (!is_empty_kl_list(cases)) {
    cbuf_printf(e->stmt, "    }\n");
    cases = CDR(cases);
  }

  cbuf_printf(e->stmt, "  }\n");

  return t;
}

static int emit_let (Emit* e, KLObject* list, int tail)
{
  KLObject* var;
  int value;
  int body;
  int saved;

  if (get_kl_list_size(list) != 4)
    throw_kl_exception("emit: let arity");

  var = CADR(list);

  if (!is_kl_symbol(var))
    throw_kl_exception("emit: let variable");

  value = emit_value(e, CADDR(list));
  saved = e->nbind;
  push_bind(e, symbol_name(var), value);
  body = emit_expr(e, CADDDR(list), tail);
  e->nbind = saved;

  return body;
}

static int emit_do (Emit* e, KLObject* list, int tail)
{
  int last;

  if (get_kl_list_size(list) != 3)
    throw_kl_exception("emit: do arity");

  (void)emit_value(e, CADR(list));
  last = emit_expr(e, CADDR(list), tail);

  return last;
}

static void collect_free (Emit* e, KLObject* expr, const char* extra,
                          char names[][256], int* nnames)
{
  if (is_kl_symbol(expr)) {
    const char* name = symbol_name(expr);
    int i;

    if (lookup_bind(e, name) < 0)
      return;

    if (extra != NULL && strcmp(name, extra) == 0)
      return;

    for (i = 0; i < *nnames; ++i) {
      if (strcmp(names[i], name) == 0)
        return;
    }

    if (*nnames >= 64)
      return;

    snprintf(names[*nnames], 256, "%s", name);
    (*nnames)++;
    return;
  }

  if (!is_non_empty_kl_list(expr))
    return;

  {
    KLObject* p = expr;

    if (is_named_symbol(CAR(expr), "lambda") && get_kl_list_size(expr) == 3 &&
        is_kl_symbol(CADR(expr))) {
      collect_free(e, CADDR(expr), symbol_name(CADR(expr)), names, nnames);
      return;
    }

    if (is_named_symbol(CAR(expr), "let") && get_kl_list_size(expr) == 4 &&
        is_kl_symbol(CADR(expr))) {
      collect_free(e, CADDR(expr), extra, names, nnames);
      collect_free(e, CADDDR(expr), symbol_name(CADR(expr)), names, nnames);
      return;
    }

    while (!is_empty_kl_list(p)) {
      collect_free(e, CAR(p), extra, names, nnames);
      p = CDR(p);
    }
  }
}

static int emit_lambda_or_freeze (Emit* e, KLObject* list, int is_freeze)
{
  char frees[64][256];
  int nfree = 0;
  int id = e->lambda_id++;
  int t = fresh(e);
  int i;
  int saved_bind;
  CBuf body;
  CBuf* saved_stmt;
  KLObject* param = NULL;
  KLObject* lam_body;
  int result;
  int param_temp = -1;
  long arity = is_freeze ? 0 : 1;

  if (is_freeze) {
    if (get_kl_list_size(list) != 2)
      throw_kl_exception("emit: freeze arity");

    lam_body = CADR(list);
  } else {
    if (get_kl_list_size(list) != 3)
      throw_kl_exception("emit: lambda arity");

    param = CADR(list);

    if (!is_kl_symbol(param))
      throw_kl_exception("emit: lambda parameter");

    lam_body = CADDR(list);
  }

  collect_free(e, lam_body, is_freeze ? NULL : symbol_name(param), frees, &nfree);

  cbuf_init(&body);
  saved_stmt = e->stmt;
  saved_bind = e->nbind;
  e->stmt = &body;
  {
    int saved_goto = e->allow_self_goto;

    e->allow_self_goto = 0;

  cbuf_printf(&body,
              "static KLObject* lambda_%d (KLObject* function_object, "
              "Vector* arguments, Environment* function_environment, "
              "Environment* variable_environment)\n{\n"
              "  shen_context* ctx = &shen_root_context;\n"
              "  Vector* caps = shen_native_captures(ctx, function_object);\n"
              "  (void)function_environment;\n"
              "  (void)variable_environment;\n",
              id);

  for (i = 0; i < nfree; ++i) {
    int cap_temp = fresh(e);

    cbuf_printf(&body, "  KLObject* t%d = shen_vector_get(ctx, caps, %d);\n",
                cap_temp, i);
    push_bind(e, frees[i], cap_temp);
  }

  if (!is_freeze) {
    param_temp = fresh(e);
    cbuf_printf(&body,
                "  {\n"
                "    KLObject** objects = shen_arguments(ctx, function_object, arguments);\n"
                "    KLObject* t%d = objects[0];\n",
                param_temp);
    push_bind(e, symbol_name(param), param_temp);
  } else {
    cbuf_printf(&body, "  (void)arguments;\n");
  }

  result = emit_expr(e, lam_body, 1);
  cbuf_printf(&body, "    return t%d;\n", result);

  if (!is_freeze)
    cbuf_printf(&body, "  }\n");

  cbuf_printf(&body, "}\n\n");
    e->allow_self_goto = saved_goto;
  }
  e->nbind = saved_bind;
  e->stmt = saved_stmt;
  cbuf_printf(&e->helpers, "%s", body.data ? body.data : "");
  e->lambda_count++;

  cbuf_printf(e->stmt, "  KLObject* t%d;\n  {\n    Vector* caps_%d = shen_vector(ctx, %d);\n",
              t, id, nfree);

  for (i = 0; i < nfree; ++i) {
    int bound = lookup_bind(e, frees[i]);

    cbuf_printf(e->stmt, "    shen_vector_set(ctx, caps_%d, %d, t%d);\n",
                id, i, bound);
  }

  cbuf_printf(e->stmt,
              "    t%d = shen_native_closure(ctx, %ld, &lambda_%d, caps_%d);\n"
              "  }\n",
              t, arity, id, id);

  return t;
}

static int emit_trap (Emit* e, KLObject* list)
{
  int id = e->trap_id++;
  int t = fresh(e);
  int i;
  int ncap = e->nbind;
  CBuf body;
  CBuf handler;
  CBuf* saved_stmt;
  int saved_bind;
  int saved_goto;
  Bind saved_binds[256];
  int body_t;
  int handler_t;
  int applied;

  if (get_kl_list_size(list) != 3)
    throw_kl_exception("emit: trap-error arity");

  cbuf_init(&body);
  saved_stmt = e->stmt;
  saved_bind = e->nbind;
  saved_goto = e->allow_self_goto;
  memcpy(saved_binds, e->binds, sizeof(saved_binds));
  e->stmt = &body;
  e->allow_self_goto = 0;
  cbuf_printf(&body,
              "static KLObject* trap_body_%d (void* data)\n{\n"
              "  shen_context* ctx = &shen_root_context;\n"
              "  Vector* env = (Vector*)data;\n",
              id);

  for (i = 0; i < ncap; ++i) {
    int cap_temp = fresh(e);

    cbuf_printf(&body, "  KLObject* t%d = shen_vector_get(ctx, env, %d);\n",
                cap_temp, i);
    e->binds[i].temp = cap_temp;
  }

  e->nbind = ncap;
  body_t = emit_value(e, CADR(list));
  cbuf_printf(&body, "  return t%d;\n}\n\n", body_t);
  memcpy(e->binds, saved_binds, sizeof(saved_binds));
  e->nbind = saved_bind;
  e->stmt = saved_stmt;
  cbuf_printf(&e->helpers, "%s", body.data ? body.data : "");

  cbuf_init(&handler);
  e->stmt = &handler;
  cbuf_printf(&handler,
              "static KLObject* trap_handler_%d (KLObject* exception, void* data)\n{\n"
              "  shen_context* ctx = &shen_root_context;\n"
              "  Vector* env = (Vector*)data;\n"
              "  Vector* args;\n",
              id);

  for (i = 0; i < ncap; ++i) {
    int cap_temp = fresh(e);

    cbuf_printf(&handler, "  KLObject* t%d = shen_vector_get(ctx, env, %d);\n",
                cap_temp, i);
    e->binds[i].temp = cap_temp;
  }

  e->nbind = ncap;
  handler_t = emit_value(e, CADDR(list));
  applied = fresh(e);
  cbuf_printf(&handler,
              "  args = shen_vector(ctx, 1);\n"
              "  shen_vector_set(ctx, args, 0, exception);\n"
              "  {\n    KLObject* t%d = shen_apply(ctx, t%d, args);\n"
              "    return t%d;\n  }\n}\n\n",
              applied, handler_t, applied);
  memcpy(e->binds, saved_binds, sizeof(saved_binds));
  e->nbind = saved_bind;
  e->stmt = saved_stmt;
  e->allow_self_goto = saved_goto;
  cbuf_printf(&e->helpers, "%s", handler.data ? handler.data : "");

  cbuf_printf(e->stmt, "  KLObject* t%d;\n  {\n    Vector* tenv_%d = shen_vector(ctx, %d);\n",
              t, id, ncap);

  for (i = 0; i < ncap; ++i)
    cbuf_printf(e->stmt, "    shen_vector_set(ctx, tenv_%d, %d, t%d);\n",
                id, i, e->binds[i].temp);

  cbuf_printf(e->stmt,
              "    t%d = shen_trap_error(ctx, trap_body_%d, "
              "trap_handler_%d, tenv_%d);\n  }\n",
              t, id, id, id);

  return t;
}

static void emit_defun_into (Emit* e, KLObject* list);

static int is_do_call (KLObject* form)
{
  return is_non_empty_kl_list(form) &&
    is_named_symbol(CAR(form), "do") &&
    get_kl_list_size(form) == 3;
}

static void flatten_do_chain (KLObject* form, KLObject** out, int* n, int cap)
{
  if (is_do_call(form)) {
    flatten_do_chain(CADR(form), out, n, cap);
    flatten_do_chain(CADDR(form), out, n, cap);
    return;
  }

  if (*n >= cap)
    throw_kl_exception("emit: do-chain too long");

  out[(*n)++] = form;
}

static int is_self_tail (Emit* e, KLObject* list, int tail)
{
  KLObject* head;
  long n;

  if (!tail || !e->allow_self_goto || e->self_name[0] == '\0')
    return 0;

  head = CAR(list);

  if (!is_kl_symbol(head) || lookup_bind(e, symbol_name(head)) >= 0)
    return 0;

  if (strcmp(symbol_name(head), e->self_name) != 0)
    return 0;

  n = get_kl_list_size(list) - 1;

  return n == e->self_arity;
}

static int emit_apply (Emit* e, KLObject* list, int tail)
{
  long n = get_kl_list_size(list) - 1;
  int head;
  int t;
  KLObject* args = CDR(list);
  int arg_temps[256];
  long i;

  if (n < 0)
    n = 0;

  if (n > 256)
    throw_kl_exception("emit: apply arity");

  if (is_self_tail(e, list, tail)) {
    for (i = 0; i < n; ++i) {
      arg_temps[i] = emit_value(e, CAR(args));
      args = CDR(args);
    }

    t = fresh(e);
    cbuf_printf(e->stmt, "  {\n");

    for (i = 0; i < n; ++i)
      cbuf_printf(e->stmt, "    KLObject* n%d_%ld = t%d;\n", t, i, arg_temps[i]);

    for (i = 0; i < n; ++i)
      cbuf_printf(e->stmt, "    t%d = n%d_%ld;\n", e->self_params[i], t, i);

    cbuf_printf(e->stmt, "    goto tail_start_%s;\n  }\n", e->self_cname);
    cbuf_printf(e->stmt, "  KLObject* t%d = NULL;\n", t);

    return t;
  }

  t = fresh(e);

  if (is_kl_symbol(CAR(list)) && lookup_bind(e, symbol_name(CAR(list))) < 0) {
    const char* helper = tail ? "shen_tail_apply_direct" : "shen_apply_direct";

    for (i = 0; i < n; ++i) {
      arg_temps[i] = emit_value(e, CAR(args));
      args = CDR(args);
    }

    if (n == 0) {
      cbuf_printf(e->stmt, "  KLObject* t%d = %s(ctx, ", t, helper);
      cbuf_cstring(e->stmt, symbol_name(CAR(list)));
      cbuf_printf(e->stmt, ", 0, NULL);\n");
      return t;
    }

    cbuf_printf(e->stmt, "  KLObject* t%d;\n  {\n    KLObject* a%d[] = {", t, t);

    for (i = 0; i < n; ++i) {
      if (i > 0)
        cbuf_printf(e->stmt, ", ");

      cbuf_printf(e->stmt, "t%d", arg_temps[i]);
    }

    cbuf_printf(e->stmt, "};\n    t%d = %s(ctx, ", t, helper);
    cbuf_cstring(e->stmt, symbol_name(CAR(list)));
    cbuf_printf(e->stmt, ", %ld, a%d);\n  }\n", n, t);

    return t;
  }

  head = emit_value(e, CAR(list));

  for (i = 0; i < n; ++i) {
    arg_temps[i] = emit_value(e, CAR(args));
    args = CDR(args);
  }

  cbuf_printf(e->stmt, "  KLObject* t%d;\n  {\n    Vector* a%d = shen_vector(ctx, %ld);\n",
              t, t, n);

  for (i = 0; i < n; ++i)
    cbuf_printf(e->stmt, "    shen_vector_set(ctx, a%d, %ld, t%d);\n",
                t, i, arg_temps[i]);

  cbuf_printf(e->stmt, "    t%d = %s(ctx, t%d, a%d);\n  }\n",
              t, tail ? "shen_tail_apply" : "shen_apply", head, t);

  return t;
}

static int emit_eval_kl (Emit* e, KLObject* list)
{
  int arg;
  int t;

  if (get_kl_list_size(list) != 2)
    throw_kl_exception("emit: eval-kl arity");

  arg = emit_value(e, CADR(list));
  t = fresh(e);
  cbuf_printf(e->stmt, "  KLObject* t%d = shen_eval_kl(ctx, t%d);\n", t, arg);

  return t;
}

static int emit_type (Emit* e, KLObject* list, int tail)
{
  if (get_kl_list_size(list) != 3)
    throw_kl_exception("emit: type arity");

  return emit_expr(e, CADR(list), tail);
}

/* Exact-arity unbound klcompile prims: ABI helpers, not intern+Vector+apply.
 * Locals of those names and partial application stay on shen_apply.
 * vector? is a kernel defun (absvector? plus slot 0); do not alias it.
 * if is already a special form (emit_if). */
static int emit_prim_inline (Emit* e, KLObject* list)
{
  KLObject* head = CAR(list);
  const char* name;
  const char* helper = NULL;
  long n;
  int a;
  int b;
  int t;

  if (!is_kl_symbol(head) || lookup_bind(e, symbol_name(head)) >= 0)
    return -1;

  name = symbol_name(head);
  n = get_kl_list_size(list) - 1;

  if (n == 2) {
    if (strcmp(name, "+") == 0)
      helper = "shen_add";
    else if (strcmp(name, "-") == 0)
      helper = "shen_sub";
    else if (strcmp(name, "*") == 0)
      helper = "shen_mul";
    else if (strcmp(name, "/") == 0)
      helper = "shen_div";
    else if (strcmp(name, "<") == 0)
      helper = "shen_lt";
    else if (strcmp(name, ">") == 0)
      helper = "shen_gt";
    else if (strcmp(name, "<=") == 0)
      helper = "shen_lte";
    else if (strcmp(name, ">=") == 0)
      helper = "shen_gte";
    else if (strcmp(name, "=") == 0)
      helper = "shen_eq";
    else if (strcmp(name, "cons") == 0)
      helper = "shen_cons";

    if (helper != NULL) {
      a = emit_value(e, CADR(list));
      b = emit_value(e, CADDR(list));
      t = fresh(e);
      cbuf_printf(e->stmt, "  KLObject* t%d = %s(ctx, t%d, t%d);\n",
                  t, helper, a, b);
      return t;
    }
  }

  if (n == 1) {
    if (strcmp(name, "hd") == 0)
      helper = "shen_hd";
    else if (strcmp(name, "tl") == 0)
      helper = "shen_tl";
    else if (strcmp(name, "cons?") == 0)
      helper = "shen_cons_p";
    else if (strcmp(name, "number?") == 0)
      helper = "shen_number_p";
    else if (strcmp(name, "string?") == 0)
      helper = "shen_string_p";
    else if (strcmp(name, "symbol?") == 0)
      helper = "shen_symbol_p";
    else if (strcmp(name, "absvector?") == 0)
      helper = "shen_absvector_p";

    if (helper != NULL) {
      a = emit_value(e, CADR(list));
      t = fresh(e);
      cbuf_printf(e->stmt, "  KLObject* t%d = %s(ctx, t%d);\n", t, helper, a);
      return t;
    }
  }

  return -1;
}

static int emit_expr (Emit* e, KLObject* expr, int tail)
{
  KLObject* head;
  int inlined;

  if (!is_non_empty_kl_list(expr))
    return emit_atom(e, expr);

  head = CAR(expr);

  if (is_kl_symbol(head) && lookup_bind(e, symbol_name(head)) < 0) {
    if (is_named_symbol(head, "if"))
      return emit_if(e, expr, tail);
    if (is_named_symbol(head, "and"))
      return emit_and_or(e, expr, 1, tail);
    if (is_named_symbol(head, "or"))
      return emit_and_or(e, expr, 0, tail);
    if (is_named_symbol(head, "cond"))
      return emit_cond(e, expr, tail);
    if (is_named_symbol(head, "let"))
      return emit_let(e, expr, tail);
    if (is_named_symbol(head, "do"))
      return emit_do(e, expr, tail);
    if (is_named_symbol(head, "lambda"))
      return emit_lambda_or_freeze(e, expr, 0);
    if (is_named_symbol(head, "freeze"))
      return emit_lambda_or_freeze(e, expr, 1);
    if (is_named_symbol(head, "trap-error"))
      return emit_trap(e, expr);
    if (is_named_symbol(head, "type"))
      return emit_type(e, expr, tail);
    if (is_named_symbol(head, "eval-kl"))
      return emit_eval_kl(e, expr);
    if (is_named_symbol(head, "defun")) {
      int t;

      emit_defun_into(e, expr);
      t = fresh(e);
      cbuf_printf(e->stmt, "  KLObject* t%d = shen_intern(ctx, ", t);
      cbuf_cstring(e->stmt, symbol_name(CADR(expr)));
      cbuf_printf(e->stmt, ");\n");

      return t;
    }

    inlined = emit_prim_inline(e, expr);

    if (inlined >= 0)
      return inlined;
  }

  return emit_apply(e, expr, tail);
}

static void emit_defun_into (Emit* e, KLObject* list)
{
  KLObject* name_object;
  KLObject* params;
  KLObject* body;
  char cname[256];
  CBuf fn;
  CBuf* saved_stmt;
  int saved_bind;
  int saved_goto;
  int saved_arity;
  int saved_nself;
  int saved_params[256];
  char saved_name[256];
  char saved_cname[256];
  int result;
  long arity;
  long i;
  KLObject* p;

  if (get_kl_list_size(list) != 4)
    throw_kl_exception("emit: defun arity");

  name_object = CADR(list);
  params = CADDR(list);
  body = CADDDR(list);

  if (!is_kl_symbol(name_object) || !is_kl_list(params))
    throw_kl_exception("emit: defun shape");

  sanitize_ident(symbol_name(name_object), cname, sizeof(cname));
  arity = is_empty_kl_list(params) ? 0 : get_kl_list_size(params);

  if (arity > 256)
    throw_kl_exception("emit: defun arity");

  if (e->ndefun >= 4096)
    throw_kl_exception("emit: too many defuns");

  snprintf(e->defun_recs[e->ndefun].name,
           sizeof(e->defun_recs[e->ndefun].name), "%s",
           symbol_name(name_object));
  snprintf(e->defun_recs[e->ndefun].cname,
           sizeof(e->defun_recs[e->ndefun].cname), "%s_%d", cname, e->ndefun);
  e->defun_recs[e->ndefun].arity = arity;
  snprintf(cname, sizeof(cname), "%s", e->defun_recs[e->ndefun].cname);
  e->ndefun++;
  e->defun_count++;

  cbuf_init(&fn);
  saved_stmt = e->stmt;
  saved_bind = e->nbind;
  saved_goto = e->allow_self_goto;
  saved_arity = e->self_arity;
  saved_nself = e->nself_params;
  memcpy(saved_params, e->self_params, sizeof(saved_params));
  snprintf(saved_name, sizeof(saved_name), "%s", e->self_name);
  snprintf(saved_cname, sizeof(saved_cname), "%s", e->self_cname);
  e->stmt = &fn;
  e->nbind = 0;
  snprintf(e->self_name, sizeof(e->self_name), "%s", symbol_name(name_object));
  snprintf(e->self_cname, sizeof(e->self_cname), "%s", cname);
  e->self_arity = (int)arity;
  e->nself_params = 0;
  e->allow_self_goto = 1;

  cbuf_printf(&fn,
              "static KLObject* native_%s (KLObject* function_object, "
              "Vector* arguments, Environment* function_environment, "
              "Environment* variable_environment)\n{\n"
              "  shen_context* ctx = &shen_root_context;\n"
              "  (void)function_environment;\n"
              "  (void)variable_environment;\n",
              cname);

  if (arity > 0) {
    cbuf_printf(&fn,
                "  {\n    KLObject** objects = shen_arguments(ctx, function_object, arguments);\n");
    p = params;
    i = 0;

    while (!is_empty_kl_list(p)) {
      int t = fresh(e);

      if (!is_kl_symbol(CAR(p)))
        throw_kl_exception("emit: defun parameter");

      cbuf_printf(&fn, "    KLObject* t%d = objects[%ld];\n", t, i);
      push_bind(e, symbol_name(CAR(p)), t);
      e->self_params[e->nself_params++] = t;
      p = CDR(p);
      i++;
    }
  } else {
    cbuf_printf(&fn, "  (void)function_object;\n  (void)arguments;\n  {\n");
  }

  cbuf_printf(&fn, "  tail_start_%s: ;\n", cname);

  if (arity == 1 &&
      strcmp(symbol_name(name_object), "shen.lambda-entry") == 0) {
    int arg = e->self_params[0];
    int skip = fresh(e);

    cbuf_printf(&fn,
                "    if (t%d == NULL || "
                "get_kl_object_type(t%d) != KL_TYPE_SYMBOL) {\n"
                "      KLObject* t%d = shen_empty_list(ctx);\n"
                "      return t%d;\n    }\n",
                arg, arg, skip, skip);
  }

  /* shen.demodulate must intern/apply shen.demod at runtime.
   * synonyms-h eval-redefines shen.demod; identity-folding demodulate
   * drops those rewrites (c-minus preamble synonym). `=` is structural
   * so identity demod still terminates after walk rebuilds conses. */

  if (arity == 0 && is_do_call(body)) {
    KLObject* steps[4096];
    int nsteps = 0;
    int s;
    int saved_init_goto;

    flatten_do_chain(body, steps, &nsteps, 4096);
    saved_init_goto = e->allow_self_goto;
    e->allow_self_goto = 0;

    for (s = 0; s < nsteps; ++s) {
      CBuf step;
      int step_result;

      cbuf_init(&step);
      e->stmt = &step;
      e->nbind = 0;
      cbuf_printf(&step,
                  "static KLObject* init_step_%s_%d (void)\n{\n"
                  "  shen_context* ctx = &shen_root_context;\n",
                  cname, s);
      step_result = emit_expr(e, steps[s], 0);
      cbuf_printf(&step, "  return t%d;\n}\n\n", step_result);
      cbuf_printf(&e->helpers, "%s", step.data ? step.data : "");
    }

    e->stmt = &fn;
    e->nbind = 0;
    e->allow_self_goto = saved_init_goto;

    for (s = 0; s < nsteps; ++s)
      cbuf_printf(&fn, "    (void)init_step_%s_%d();\n", cname, s);

    result = fresh(e);
    cbuf_printf(&fn, "    KLObject* t%d = shen_empty_list(ctx);\n", result);
  } else {
    result = emit_expr(e, body, 1);
  }

  cbuf_printf(&fn, "    return t%d;\n  }\n}\n\n", result);
  e->stmt = saved_stmt;
  e->nbind = saved_bind;
  e->allow_self_goto = saved_goto;
  e->self_arity = saved_arity;
  e->nself_params = saved_nself;
  memcpy(e->self_params, saved_params, sizeof(saved_params));
  snprintf(e->self_name, sizeof(e->self_name), "%s", saved_name);
  snprintf(e->self_cname, sizeof(e->self_cname), "%s", saved_cname);
  cbuf_printf(&e->defuns, "%s", fn.data ? fn.data : "");
}

static void emit_toplevel (Emit* e, KLObject* form)
{
  CBuf fn;
  CBuf* saved_stmt;
  int saved_bind;
  int id = e->toplevel_id++;
  int result;

  cbuf_init(&fn);
  saved_stmt = e->stmt;
  saved_bind = e->nbind;
  e->stmt = &fn;
  e->nbind = 0;
  cbuf_printf(&fn,
              "static KLObject* toplevel_%d (void)\n{\n"
              "  shen_context* ctx = &shen_root_context;\n",
              id);
  result = emit_expr(e, form, 0);
  cbuf_printf(&fn, "  return t%d;\n}\n\n", result);
  e->stmt = saved_stmt;
  e->nbind = saved_bind;
  cbuf_printf(&e->defuns, "%s", fn.data ? fn.data : "");

  if (e->in_user)
    e->user_tops[e->nuser_tops++] = id;
  else
    e->kernel_tops[e->nkernel_tops++] = id;

  e->toplevel_count++;
}

static void emit_forms (Emit* e, KLObject** forms, long n)
{
  long i;

  for (i = 0; i < n; ++i) {
    KLObject* form = forms[i];

    if (is_non_empty_kl_list(form) && is_named_symbol(CAR(form), "defun"))
      emit_defun_into(e, form);
    else
      emit_toplevel(e, form);
  }
}

int shen_read_kl_path (const char* path, KLObject*** forms_out, long* n_out)
{
  FILE* file = fopen(path, "r");
  KLObject* stream;
  KLObject** forms;
  long n = 0;
  long cap = 64;

  if (file == NULL)
    return -1;

  stream = create_std_kl_stream(file, get_in_symbol_object());
  forms = malloc(sizeof(KLObject*) * (size_t)cap);

  while (1) {
    KLObject* form = read_string(stream);

    if (form == NULL)
      break;

    if (n >= cap) {
      cap *= 2;
      forms = realloc(forms, sizeof(KLObject*) * (size_t)cap);
    }

    forms[n++] = form;
  }

  fclose(file);
  *forms_out = forms;
  *n_out = n;

  return 0;
}

int shen_emit_program (FILE* out,
                       KLObject** kernel_forms, long nkernel,
                       KLObject** user_forms, long nuser,
                       const char* init_name,
                       const char* source_label,
                       ShenEmitReport* report)
{
  return shen_emit_program_ex(out, kernel_forms, nkernel, user_forms, nuser,
                              init_name, source_label, NULL, report);
}

int shen_emit_program_ex (FILE* out,
                          KLObject** kernel_forms, long nkernel,
                          KLObject** user_forms, long nuser,
                          const char* init_name,
                          const char* source_label,
                          const char* extra_in_main,
                          ShenEmitReport* report)
{
  Emit e;
  int i;

  memset(&e, 0, sizeof(e));
  cbuf_init(&e.helpers);
  cbuf_init(&e.defuns);

  e.in_user = 0;
  emit_forms(&e, kernel_forms, nkernel);
  e.in_user = 1;
  emit_forms(&e, user_forms, nuser);

  fprintf(out,
          "/* Generated by shen-c option 5 rung 1 (KL defuns -> C NativeFunctions).\n"
          " * Allocations stay on shen_context / Boehm. Not Chicken, not malloc/free\n"
          " * KLObject, not C++. Not a source-string eval wrap.\n"
          " * Self-tails: goto tail_start_*. Other tails: shen_tail_apply trampoline.\n"
          " * Source: %s\n"
          " */\n"
          "#include \"abi.h\"\n\n",
          source_label ? source_label : "shaken KLambda");

  if (e.helpers.data != NULL)
    fputs(e.helpers.data, out);

  if (e.defuns.data != NULL)
    fputs(e.defuns.data, out);

  fprintf(out,
          "static void shen_generated_install (void)\n{\n"
          "  shen_context* ctx = &shen_root_context;\n");

  for (i = 0; i < e.ndefun; ++i) {
    fprintf(out, "  shen_register_defun(ctx, ");
    {
      CBuf namebuf;

      cbuf_init(&namebuf);
      cbuf_cstring(&namebuf, e.defun_recs[i].name);
      fputs(namebuf.data, out);
    }
    fprintf(out, ", %ld, &native_%s);\n",
            e.defun_recs[i].arity, e.defun_recs[i].cname);
  }

  fprintf(out, "}\n\nstatic void shen_generated_run_kernel (void)\n{\n");

  for (i = 0; i < e.nkernel_tops; ++i)
    fprintf(out, "  (void)toplevel_%d();\n", e.kernel_tops[i]);

  fprintf(out, "}\n\nstatic void shen_generated_run_user (void)\n{\n");

  for (i = 0; i < e.nuser_tops; ++i)
    fprintf(out, "  (void)toplevel_%d();\n", e.user_tops[i]);

  fprintf(out,
          "}\n\nint main (void)\n{\n"
          "  shen_context* ctx = &shen_root_context;\n"
          "  shen_boot(ctx, \".\");\n"
          "  shen_generated_install();\n"
          "  shen_apply_port_overwrites();\n"
          "  shen_generated_run_kernel();\n");

  if (init_name != NULL && init_name[0] != '\0') {
    CBuf namebuf;

    cbuf_init(&namebuf);
    cbuf_cstring(&namebuf, init_name);
    fprintf(out,
            "  {\n    Vector* init_args = shen_vector(ctx, 0);\n"
            "    (void)shen_apply(ctx, shen_intern(ctx, %s), init_args);\n  }\n",
            namebuf.data);
  }

  if (extra_in_main != NULL && extra_in_main[0] != '\0')
    fputs(extra_in_main, out);

  fprintf(out,
          "  shen_generated_run_user();\n"
          "  return 0;\n}\n");

  if (report != NULL) {
    report->defuns = e.defun_count;
    report->toplevels = e.toplevel_count;
    report->lambdas = e.lambda_count;
  }

  return 0;
}

static int overlay_ident_ok (const char* ident)
{
  const char* p;

  if (ident == NULL || ident[0] == '\0')
    return 0;

  if (!(ident[0] == '_' ||
        (ident[0] >= 'A' && ident[0] <= 'Z') ||
        (ident[0] >= 'a' && ident[0] <= 'z')))
    return 0;

  for (p = ident + 1; *p != '\0'; ++p) {
    if (!(*p == '_' ||
          (*p >= 'A' && *p <= 'Z') ||
          (*p >= 'a' && *p <= 'z') ||
          (*p >= '0' && *p <= '9')))
      return 0;
  }

  return 1;
}

int shen_emit_overlay (FILE* out,
                       KLObject** forms, long nforms,
                       const char* module_ident,
                       const char* label,
                       uint64_t source_fnv,
                       uint64_t kernel_fnv,
                       ShenEmitReport* report)
{
  Emit e;
  int i;
  CBuf labelbuf;

  if (out == NULL || !overlay_ident_ok(module_ident))
    return -1;

  memset(&e, 0, sizeof(e));
  cbuf_init(&e.helpers);
  cbuf_init(&e.defuns);

  e.in_user = 1;

  for (i = 0; i < nforms; ++i) {
    KLObject* form = forms[i];

    if (is_non_empty_kl_list(form) && is_named_symbol(CAR(form), "defun"))
      emit_defun_into(&e, form);
  }

  fprintf(out,
          "/* Generated AOT overlay module (bootstrap defuns -> NativeFunctions).\n"
          " * Install after (load) of the matching .shen. Not a second main.\n"
          " * Not a source-string eval wrap. Source: %s\n"
          " */\n"
          "#include \"abi.h\"\n\n",
          label ? label : module_ident);

  if (e.helpers.data != NULL)
    fputs(e.helpers.data, out);

  if (e.defuns.data != NULL)
    fputs(e.defuns.data, out);

  fprintf(out, "static const ShenOverlayNameArity shen_overlay_compiled_%s[] = {\n",
          module_ident);

  for (i = 0; i < e.ndefun; ++i) {
    CBuf namebuf;

    cbuf_init(&namebuf);
    cbuf_cstring(&namebuf, e.defun_recs[i].name);
    fprintf(out, "  {%s, %ld},\n", namebuf.data, e.defun_recs[i].arity);
  }

  fprintf(out,
          "};\n\nstatic void shen_overlay_install_%s (shen_context* ctx)\n{\n",
          module_ident);

  for (i = 0; i < e.ndefun; ++i) {
    CBuf namebuf;

    cbuf_init(&namebuf);
    cbuf_cstring(&namebuf, e.defun_recs[i].name);
    fprintf(out, "  shen_register_defun(ctx, %s, %ld, &native_%s);\n",
            namebuf.data, e.defun_recs[i].arity, e.defun_recs[i].cname);
  }

  cbuf_init(&labelbuf);
  cbuf_cstring(&labelbuf, label ? label : module_ident);
  fprintf(out,
          "}\n\nconst ShenOverlayModule shen_overlay_module_%s = {\n"
          "  %s,\n"
          "  SHEN_OVERLAY_FORMAT,\n"
          "  %" PRIu64 "ULL,\n"
          "  %" PRIu64 "ULL,\n"
          "  shen_overlay_compiled_%s,\n"
          "  %d,\n"
          "  shen_overlay_install_%s\n"
          "};\n",
          module_ident,
          labelbuf.data ? labelbuf.data : "\"overlay\"",
          source_fnv,
          kernel_fnv,
          module_ident,
          e.ndefun,
          module_ident);

  if (report != NULL) {
    report->defuns = e.defun_count;
    report->toplevels = e.toplevel_count;
    report->lambdas = e.lambda_count;
  }

  return 0;
}
