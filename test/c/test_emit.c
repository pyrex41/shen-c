#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"

static int failures = 0;

static void expect (int condition, const char* message)
{
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

int main (void)
{
  shen_context* ctx;
  KLObject* form;
  KLObject* params;
  KLObject* body;
  KLObject* plus;
  FILE* out;
  char* generated = NULL;
  long size;
  ShenEmitReport report = {0};

  shen_boot(&shen_root_context, ".");
  ctx = &shen_root_context;

  plus = shen_intern(ctx, "+");
  params = shen_cons(ctx, shen_intern(ctx, "X"),
                     shen_cons(ctx, shen_intern(ctx, "Y"),
                               shen_empty_list(ctx)));
  body = shen_cons(ctx, plus,
                   shen_cons(ctx, shen_intern(ctx, "X"),
                             shen_cons(ctx, shen_intern(ctx, "Y"),
                                       shen_empty_list(ctx))));
  form = shen_cons(ctx, shen_intern(ctx, "defun"),
                   shen_cons(ctx, shen_intern(ctx, "add2"),
                             shen_cons(ctx, params,
                                       shen_cons(ctx, body,
                                                 shen_empty_list(ctx)))));

  {
    KLObject* n;
    KLObject* zero;
    KLObject* pred;
    KLObject* test;
    KLObject* recur;
    KLObject* if_form;
    KLObject* cd_params;
    KLObject* cd_form;
    KLObject* forms[2];

    n = shen_intern(ctx, "N");
    zero = shen_number_l(ctx, 0);
    pred = shen_cons(ctx, shen_intern(ctx, "-"),
                     shen_cons(ctx, n,
                               shen_cons(ctx, shen_number_l(ctx, 1),
                                         shen_empty_list(ctx))));
    test = shen_cons(ctx, shen_intern(ctx, "="),
                     shen_cons(ctx, n,
                               shen_cons(ctx, zero, shen_empty_list(ctx))));
    recur = shen_cons(ctx, shen_intern(ctx, "countdown"),
                      shen_cons(ctx, pred, shen_empty_list(ctx)));
    if_form = shen_cons(ctx, shen_intern(ctx, "if"),
                        shen_cons(ctx, test,
                                  shen_cons(ctx, zero,
                                            shen_cons(ctx, recur,
                                                      shen_empty_list(ctx)))));
    cd_params = shen_cons(ctx, n, shen_empty_list(ctx));
    cd_form = shen_cons(ctx, shen_intern(ctx, "defun"),
                        shen_cons(ctx, shen_intern(ctx, "countdown"),
                                  shen_cons(ctx, cd_params,
                                            shen_cons(ctx, if_form,
                                                      shen_empty_list(ctx)))));
    forms[0] = form;
    forms[1] = cd_form;

    out = tmpfile();
    expect(out != NULL, "tmpfile");

    if (out == NULL)
      return 1;

    expect(shen_emit_program(out, NULL, 0, forms, 2, NULL, "test_emit",
                             &report) == 0,
           "emit add2+countdown");
    expect(report.defuns == 2, "two defuns emitted");
  }

  fseek(out, 0, SEEK_END);
  size = ftell(out);
  fseek(out, 0, SEEK_SET);
  generated = malloc((size_t)size + 1);

  if (generated == NULL)
    return 1;

  if (size > 0)
    fread(generated, 1, (size_t)size, out);

  generated[size] = '\0';
  fclose(out);

  expect(strstr(generated, "shen_register_defun") != NULL,
         "generated C registers NativeFunction defun");
  expect(strstr(generated, "native_") != NULL, "generated C has native_*");
  expect(strstr(generated, "shen_apply") != NULL, "generated C applies primitives");
  expect(strstr(generated, "shen_apply_direct") != NULL ||
         strstr(generated, "shen_add") != NULL,
         "named calls are apply_direct or inlined prims");
  expect(strstr(generated, "shen_apply_port_overwrites") != NULL,
         "generated main re-applies C port overwrites after defuns");
  expect(strstr(generated, "shen_add") != NULL, "add2 inlines + to shen_add");
  expect(strstr(generated, "shen_sub") != NULL, "countdown inlines - to shen_sub");
  expect(strstr(generated, "shen_eq") != NULL, "countdown inlines = to shen_eq");
  expect(strstr(generated, "shen_intern(ctx, \"+\")") == NULL,
         "exact-arity + is not intern+apply");
  expect(strstr(generated, "shen_intern(ctx, \"-\")") == NULL,
         "exact-arity - is not intern+apply");
  expect(strstr(generated, "shen_intern(ctx, \"=\")") == NULL,
         "exact-arity = is not intern+apply");
  expect(strstr(generated, "goto tail_start_") != NULL,
         "self-tail countdown lowers to goto");
  expect(strstr(generated, "shen_eval_kl") == NULL,
         "add2 does not go through eval-kl");
  expect(strstr(generated, "(defun add2") == NULL,
         "generated C does not wrap the original defun source");

  free(generated);

  {
    KLObject* v;
    KLObject* w;
    KLObject* z;
    KLObject* e;
    KLObject* demod_call;
    KLObject* walk_call;
    KLObject* lambda;
    KLObject* let_form;
    KLObject* if_form;
    KLObject* trap;
    KLObject* handler;
    KLObject* params;
    KLObject* demodulate;
    KLObject* forms[1];
    ShenEmitReport demod_report = {0};

    v = shen_intern(ctx, "V");
    w = shen_intern(ctx, "W");
    z = shen_intern(ctx, "Z");
    e = shen_intern(ctx, "E");
    demod_call = shen_cons(ctx, shen_intern(ctx, "shen.demod"),
                           shen_cons(ctx, z, shen_empty_list(ctx)));
    lambda = shen_cons(ctx, shen_intern(ctx, "lambda"),
                       shen_cons(ctx, z,
                                 shen_cons(ctx, demod_call,
                                           shen_empty_list(ctx))));
    walk_call = shen_cons(ctx, shen_intern(ctx, "shen.walk"),
                          shen_cons(ctx, lambda,
                                    shen_cons(ctx, v, shen_empty_list(ctx))));
    if_form = shen_cons(ctx, shen_intern(ctx, "if"),
                        shen_cons(ctx,
                                  shen_cons(ctx, shen_intern(ctx, "="),
                                            shen_cons(ctx, w,
                                                      shen_cons(ctx, v,
                                                                shen_empty_list(ctx)))),
                                  shen_cons(ctx, v,
                                            shen_cons(ctx,
                                                      shen_cons(ctx, shen_intern(ctx, "shen.demodulate"),
                                                                shen_cons(ctx, w, shen_empty_list(ctx))),
                                                      shen_empty_list(ctx)))));
    let_form = shen_cons(ctx, shen_intern(ctx, "let"),
                         shen_cons(ctx, w,
                                   shen_cons(ctx, walk_call,
                                             shen_cons(ctx, if_form,
                                                       shen_empty_list(ctx)))));
    handler = shen_cons(ctx, shen_intern(ctx, "lambda"),
                        shen_cons(ctx, e,
                                  shen_cons(ctx, v, shen_empty_list(ctx))));
    trap = shen_cons(ctx, shen_intern(ctx, "trap-error"),
                     shen_cons(ctx, let_form,
                               shen_cons(ctx, handler, shen_empty_list(ctx))));
    params = shen_cons(ctx, v, shen_empty_list(ctx));
    demodulate = shen_cons(ctx, shen_intern(ctx, "defun"),
                           shen_cons(ctx, shen_intern(ctx, "shen.demodulate"),
                                     shen_cons(ctx, params,
                                               shen_cons(ctx, trap,
                                                         shen_empty_list(ctx)))));
    forms[0] = demodulate;
    out = tmpfile();
    expect(out != NULL, "demodulate tmpfile");
    expect(shen_emit_program(out, NULL, 0, forms, 1, NULL, "test_emit",
                             &demod_report) == 0,
           "emit shen.demodulate");
    fseek(out, 0, SEEK_END);
    size = ftell(out);
    fseek(out, 0, SEEK_SET);
    generated = malloc((size_t)size + 1);
    expect(generated != NULL, "demodulate generated buffer");
    if (generated != NULL && size > 0)
      fread(generated, 1, (size_t)size, out);
    if (generated != NULL)
      generated[size] = '\0';
    fclose(out);
    expect(generated != NULL &&
           strstr(generated, "apply_direct(ctx, \"shen.walk\"") != NULL,
           "demodulate apply_direct shen.walk (not identity-folded)");
    expect(generated != NULL &&
           strstr(generated, "apply_direct(ctx, \"shen.demod\"") != NULL,
           "demodulate apply_direct shen.demod (synonyms-h redefines it)");
    expect(generated != NULL && strstr(generated, "shen_native_closure") != NULL,
           "demodulate lambda stays shen_native_closure");
    expect(generated != NULL && strstr(generated, "eval_kl_object") == NULL,
           "demodulate is not eval_kl_object of source");
  }

  {
    FILE* overlay_out;
    ShenEmitReport overlay_report = {0};
    KLObject* overlay_forms[1];

    overlay_forms[0] = form;
    overlay_out = tmpfile();
    expect(overlay_out != NULL, "overlay tmpfile");
    expect(shen_emit_overlay(overlay_out, overlay_forms, 1, "interpreter",
                             "interpreter.shen", 0x11ULL, 0x22ULL,
                             &overlay_report) == 0,
           "emit overlay module");
    expect(overlay_report.defuns == 1, "overlay emits one defun");
    fseek(overlay_out, 0, SEEK_END);
    size = ftell(overlay_out);
    fseek(overlay_out, 0, SEEK_SET);
    free(generated);
    generated = malloc((size_t)size + 1);
    expect(generated != NULL, "overlay generated buffer");
    if (generated != NULL && size > 0)
      fread(generated, 1, (size_t)size, overlay_out);
    if (generated != NULL)
      generated[size] = '\0';
    fclose(overlay_out);
    expect(generated != NULL && strstr(generated, "int main") == NULL,
           "overlay is not a second main");
    expect(generated != NULL &&
           strstr(generated, "shen_generated_install") == NULL,
           "overlay does not emit shaken-app install");
    expect(generated != NULL &&
           strstr(generated, "SHEN_OVERLAY_FORMAT") != NULL,
           "overlay records format tag");
    expect(generated != NULL &&
           strstr(generated, "shen_overlay_module_interpreter") != NULL,
           "overlay exports module symbol");
    expect(generated != NULL && strstr(generated, "shen_register_defun") != NULL,
           "overlay install registers NativeFunction defuns");
    expect(generated != NULL && strstr(generated, "native_") != NULL,
           "overlay has compiled natives");
    expect(generated != NULL && strstr(generated, "eval_kl_object(") == NULL,
           "overlay is not eval_kl_object of sidecar source");
    expect(generated != NULL && strstr(generated, "shen_add") != NULL,
           "overlay add2 also inlines +");
  }

  {
    KLObject* x;
    KLObject* xs;
    KLObject* cons_body;
    KLObject* hd_body;
    KLObject* tl_body;
    KLObject* partial;
    KLObject* forms[4];
    ShenEmitReport list_report = {0};

    x = shen_intern(ctx, "X");
    xs = shen_intern(ctx, "Xs");
    cons_body = shen_cons(ctx, shen_intern(ctx, "cons"),
                          shen_cons(ctx, x,
                                    shen_cons(ctx, xs, shen_empty_list(ctx))));
    hd_body = shen_cons(ctx, shen_intern(ctx, "hd"),
                        shen_cons(ctx, xs, shen_empty_list(ctx)));
    tl_body = shen_cons(ctx, shen_intern(ctx, "tl"),
                        shen_cons(ctx, xs, shen_empty_list(ctx)));
    partial = shen_cons(ctx, shen_intern(ctx, "+"),
                        shen_cons(ctx, x, shen_empty_list(ctx)));
    forms[0] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "snoc"),
                                   shen_cons(ctx,
                                             shen_cons(ctx, x,
                                                       shen_cons(ctx, xs,
                                                                 shen_empty_list(ctx))),
                                             shen_cons(ctx, cons_body,
                                                       shen_empty_list(ctx)))));
    forms[1] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "first"),
                                   shen_cons(ctx, shen_cons(ctx, xs,
                                                            shen_empty_list(ctx)),
                                             shen_cons(ctx, hd_body,
                                                       shen_empty_list(ctx)))));
    forms[2] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "rest"),
                                   shen_cons(ctx, shen_cons(ctx, xs,
                                                            shen_empty_list(ctx)),
                                             shen_cons(ctx, tl_body,
                                                       shen_empty_list(ctx)))));
    forms[3] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "plus1"),
                                   shen_cons(ctx, shen_cons(ctx, x,
                                                            shen_empty_list(ctx)),
                                             shen_cons(ctx, partial,
                                                       shen_empty_list(ctx)))));
    out = tmpfile();
    expect(out != NULL, "list-prim tmpfile");
    expect(shen_emit_program(out, NULL, 0, forms, 4, NULL, "test_emit",
                             &list_report) == 0,
           "emit cons/hd/tl/partial+");
    fseek(out, 0, SEEK_END);
    size = ftell(out);
    fseek(out, 0, SEEK_SET);
    free(generated);
    generated = malloc((size_t)size + 1);
    expect(generated != NULL, "list-prim generated buffer");
    if (generated != NULL && size > 0)
      fread(generated, 1, (size_t)size, out);
    if (generated != NULL)
      generated[size] = '\0';
    fclose(out);
    expect(generated != NULL && strstr(generated, "shen_cons") != NULL,
           "cons inlines to shen_cons");
    expect(generated != NULL && strstr(generated, "shen_hd") != NULL,
           "hd inlines to shen_hd");
    expect(generated != NULL && strstr(generated, "shen_tl") != NULL,
           "tl inlines to shen_tl");
    expect(generated != NULL &&
           strstr(generated, "shen_intern(ctx, \"cons\")") == NULL,
           "exact-arity cons is not intern+apply");
    expect(generated != NULL &&
           strstr(generated, "apply_direct(ctx, \"+\"") != NULL,
           "partial + stays apply_direct");
    expect(generated != NULL && strstr(generated, "shen_native_closure") == NULL,
           "list prims are not wrapped as closures");
  }

  {
    KLObject* x;
    KLObject* y;
    KLObject* forms[6];
    ShenEmitReport pred_report = {0};

    x = shen_intern(ctx, "X");
    y = shen_intern(ctx, "Y");
    forms[0] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "pair?"),
                                   shen_cons(ctx, shen_cons(ctx, x,
                                                            shen_empty_list(ctx)),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, "cons?"),
                                                                 shen_cons(ctx, x, shen_empty_list(ctx))),
                                                       shen_empty_list(ctx)))));
    forms[1] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "same"),
                                   shen_cons(ctx,
                                             shen_cons(ctx, x,
                                                       shen_cons(ctx, y,
                                                                 shen_empty_list(ctx))),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, "="),
                                                                 shen_cons(ctx, x,
                                                                           shen_cons(ctx, y, shen_empty_list(ctx)))),
                                                       shen_empty_list(ctx)))));
    forms[2] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "times"),
                                   shen_cons(ctx,
                                             shen_cons(ctx, x,
                                                       shen_cons(ctx, y,
                                                                 shen_empty_list(ctx))),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, "*"),
                                                                 shen_cons(ctx, x,
                                                                           shen_cons(ctx, y, shen_empty_list(ctx)))),
                                                       shen_empty_list(ctx)))));
    forms[3] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "bigger"),
                                   shen_cons(ctx,
                                             shen_cons(ctx, x,
                                                       shen_cons(ctx, y,
                                                                 shen_empty_list(ctx))),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, ">"),
                                                                 shen_cons(ctx, x,
                                                                           shen_cons(ctx, y, shen_empty_list(ctx)))),
                                                       shen_empty_list(ctx)))));
    forms[4] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "nump"),
                                   shen_cons(ctx, shen_cons(ctx, x,
                                                            shen_empty_list(ctx)),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, "number?"),
                                                                 shen_cons(ctx, x, shen_empty_list(ctx))),
                                                       shen_empty_list(ctx)))));
    forms[5] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "times1"),
                                   shen_cons(ctx, shen_cons(ctx, x,
                                                            shen_empty_list(ctx)),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, "*"),
                                                                 shen_cons(ctx, x, shen_empty_list(ctx))),
                                                       shen_empty_list(ctx)))));
    out = tmpfile();
    expect(out != NULL, "pred-prim tmpfile");
    expect(shen_emit_program(out, NULL, 0, forms, 6, NULL, "test_emit",
                             &pred_report) == 0,
           "emit cons?/=/*/>/number?/partial*");
    fseek(out, 0, SEEK_END);
    size = ftell(out);
    fseek(out, 0, SEEK_SET);
    free(generated);
    generated = malloc((size_t)size + 1);
    expect(generated != NULL, "pred-prim generated buffer");
    if (generated != NULL && size > 0)
      fread(generated, 1, (size_t)size, out);
    if (generated != NULL)
      generated[size] = '\0';
    fclose(out);
    expect(generated != NULL && strstr(generated, "shen_cons_p") != NULL,
           "cons? inlines to shen_cons_p");
    expect(generated != NULL && strstr(generated, "shen_eq") != NULL,
           "= inlines to shen_eq");
    expect(generated != NULL && strstr(generated, "shen_mul") != NULL,
           "* inlines to shen_mul");
    expect(generated != NULL && strstr(generated, "shen_gt") != NULL,
           "> inlines to shen_gt");
    expect(generated != NULL && strstr(generated, "shen_number_p") != NULL,
           "number? inlines to shen_number_p");
    expect(generated != NULL &&
           strstr(generated, "shen_intern(ctx, \"cons?\")") == NULL,
           "exact-arity cons? is not intern+apply");
    expect(generated != NULL &&
           strstr(generated, "shen_intern(ctx, \"=\")") == NULL,
           "exact-arity = is not intern+apply");
    expect(generated != NULL &&
           strstr(generated, "apply_direct(ctx, \"*\"") != NULL,
           "partial * stays apply_direct");
  }

  {
    KLObject* x;
    KLObject* y;
    KLObject* forms[8];
    ShenEmitReport rest_report = {0};

    x = shen_intern(ctx, "X");
    y = shen_intern(ctx, "Y");
    forms[0] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "strp"),
                                   shen_cons(ctx, shen_cons(ctx, x,
                                                            shen_empty_list(ctx)),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, "string?"),
                                                                 shen_cons(ctx, x, shen_empty_list(ctx))),
                                                       shen_empty_list(ctx)))));
    forms[1] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "symp"),
                                   shen_cons(ctx, shen_cons(ctx, x,
                                                            shen_empty_list(ctx)),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, "symbol?"),
                                                                 shen_cons(ctx, x, shen_empty_list(ctx))),
                                                       shen_empty_list(ctx)))));
    forms[2] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "absp"),
                                   shen_cons(ctx, shen_cons(ctx, x,
                                                            shen_empty_list(ctx)),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, "absvector?"),
                                                                 shen_cons(ctx, x, shen_empty_list(ctx))),
                                                       shen_empty_list(ctx)))));
    forms[3] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "vecp"),
                                   shen_cons(ctx, shen_cons(ctx, x,
                                                            shen_empty_list(ctx)),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, "vector?"),
                                                                 shen_cons(ctx, x, shen_empty_list(ctx))),
                                                       shen_empty_list(ctx)))));
    forms[4] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "quot"),
                                   shen_cons(ctx,
                                             shen_cons(ctx, x,
                                                       shen_cons(ctx, y,
                                                                 shen_empty_list(ctx))),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, "/"),
                                                                 shen_cons(ctx, x,
                                                                           shen_cons(ctx, y, shen_empty_list(ctx)))),
                                                       shen_empty_list(ctx)))));
    forms[5] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "smaller"),
                                   shen_cons(ctx,
                                             shen_cons(ctx, x,
                                                       shen_cons(ctx, y,
                                                                 shen_empty_list(ctx))),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, "<"),
                                                                 shen_cons(ctx, x,
                                                                           shen_cons(ctx, y, shen_empty_list(ctx)))),
                                                       shen_empty_list(ctx)))));
    forms[6] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "atmost"),
                                   shen_cons(ctx,
                                             shen_cons(ctx, x,
                                                       shen_cons(ctx, y,
                                                                 shen_empty_list(ctx))),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, "<="),
                                                                 shen_cons(ctx, x,
                                                                           shen_cons(ctx, y, shen_empty_list(ctx)))),
                                                       shen_empty_list(ctx)))));
    forms[7] = shen_cons(ctx, shen_intern(ctx, "defun"),
                         shen_cons(ctx, shen_intern(ctx, "atleast"),
                                   shen_cons(ctx,
                                             shen_cons(ctx, x,
                                                       shen_cons(ctx, y,
                                                                 shen_empty_list(ctx))),
                                             shen_cons(ctx,
                                                       shen_cons(ctx, shen_intern(ctx, ">="),
                                                                 shen_cons(ctx, x,
                                                                           shen_cons(ctx, y, shen_empty_list(ctx)))),
                                                       shen_empty_list(ctx)))));
    out = tmpfile();
    expect(out != NULL, "rest-prim tmpfile");
    expect(shen_emit_program(out, NULL, 0, forms, 8, NULL, "test_emit",
                             &rest_report) == 0,
           "emit string?/symbol?/absvector?/vector?///</<=/>=");
    fseek(out, 0, SEEK_END);
    size = ftell(out);
    fseek(out, 0, SEEK_SET);
    free(generated);
    generated = malloc((size_t)size + 1);
    expect(generated != NULL, "rest-prim generated buffer");
    if (generated != NULL && size > 0)
      fread(generated, 1, (size_t)size, out);
    if (generated != NULL)
      generated[size] = '\0';
    fclose(out);
    expect(generated != NULL && strstr(generated, "shen_string_p") != NULL,
           "string? inlines to shen_string_p");
    expect(generated != NULL && strstr(generated, "shen_symbol_p") != NULL,
           "symbol? inlines to shen_symbol_p");
    expect(generated != NULL && strstr(generated, "shen_absvector_p") != NULL,
           "absvector? inlines to shen_absvector_p");
    expect(generated != NULL && strstr(generated, "shen_div") != NULL,
           "/ inlines to shen_div");
    expect(generated != NULL && strstr(generated, "shen_lt") != NULL,
           "< inlines to shen_lt");
    expect(generated != NULL && strstr(generated, "shen_lte") != NULL,
           "<= inlines to shen_lte");
    expect(generated != NULL && strstr(generated, "shen_gte") != NULL,
           ">= inlines to shen_gte");
    expect(generated != NULL &&
           strstr(generated, "shen_intern(ctx, \"string?\")") == NULL,
           "exact-arity string? is not intern+apply");
    expect(generated != NULL &&
           strstr(generated, "apply_direct(ctx, \"vector?\"") != NULL,
           "vector? stays apply_direct (kernel defun)");
  }

  if (failures != 0) {
    fprintf(stderr, "%d emit test(s) failed\n", failures);
    return 1;
  }

  printf("emit tests ok\n");
  return 0;
}
