#ifndef SHEN_RC_READ_H
#define SHEN_RC_READ_H
#include "rc.h"
/* Read one KLambda form, advancing *source. The returned value is owned.
 * NULL with no context error means clean EOF. Strings preserve bytes literally,
 * including newlines, as required by KLambda (no C escape interpretation). */
RcValue *rc_read(RcContext *ctx, const char **source);
/* Explicit compiler AST format escaped-v1: strings use \\, \" and
 * \xHH byte escapes; #sym"..." represents arbitrary symbol tokens.
 * This mode is separate from standard KLambda's literal string protocol. */
RcValue *rc_read_escaped(RcContext *ctx, const char **source);
#endif
