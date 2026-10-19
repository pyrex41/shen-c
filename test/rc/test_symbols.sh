#!/bin/sh
# Generated static symbol descriptors must remain safe across live contexts.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
work=$(mktemp -d "${TMPDIR:-/tmp}/shen-rc-symbols.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/input"
: > "$work/input/kernel.kl"
cat > "$work/input/user.kl" <<'KL'
(defun cached-context () (value *cache-marker*))
(defun cached-identity () cached-symbol)
KL
SHEN_C_HOME="$root" "$builder" "$work/input" "$work/app"
cat > "$work/app/contexts.c" <<'C'
#define SHEN_RC_NO_MAIN
#include "app.c"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
static void check(RcContext *ctx, int marker) {
    RcValue *name=rc_symbol(ctx,"*cache-marker*"), *value=rc_integer(ctx,marker);
    assert(name && value && rc_global_set(ctx,name,value));
    rc_release(name); rc_release(value);
    name=rc_symbol(ctx,"cached-context"); assert(name);
    value=rc_apply(ctx,name,0,NULL); assert(value && rc_integer_value(value)==marker);
    rc_release(name); rc_release(value);
    name=rc_symbol(ctx,"cached-identity"); assert(name);
    value=rc_apply(ctx,name,0,NULL); assert(value);
    RcValue *same=rc_symbol(ctx,"cached-symbol"); assert(value==same);
    rc_release(name); rc_release(value); rc_release(same);
    rc_collect(ctx);
}
int main(void) {
    RcContext *anchor=rc_context_new(); assert(anchor);
    assert(rc_bootstrap_kernel(anchor) && rc_run_user(anchor));
    for(int i=0;i<64;i++) {
        RcContext *ctx=rc_context_new(); assert(ctx);
        assert(rc_bootstrap_kernel(ctx) && rc_run_user(ctx));
        check(ctx,i); check(anchor,1000+i); rc_context_free(ctx);
        check(anchor,2000+i);
    }
    rc_context_free(anchor);
    puts("RC emitted symbol context lifetime: PASS");
}
C
# Match the generated app's documented compiler overrides.
${CC:-cc} ${CPPFLAGS:-} ${CFLAGS:--O2} -std=c17 -I"$work/app" -I"$root/src/rc" \
    "$work/app/contexts.c" "$root/src/rc/rc.c" "$root/src/rc/primitives.c" \
    "$root/src/rc/eval.c" "$root/src/rc/read.c" "$root/src/rc/emit.c" "$root/src/rc/native_compile.c" $(case $(uname -s) in Darwin) echo -Wl,-export_dynamic;; *) echo -rdynamic -ldl;; esac) -lm -o "$work/app/contexts"
"$work/app/contexts"
