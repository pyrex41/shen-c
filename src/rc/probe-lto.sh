#!/bin/sh
# Print usable LTO flags, or nothing on unsupported compiler/archive/link tools.
# Deliberate word splitting supports CC/AR wrappers and ordinary flag lists.
set -eu
probe=$(mktemp -d "${TMPDIR:-/tmp}/shen-rc-lto.XXXXXXXX") || exit 0
trap 'rm -rf "$probe"' EXIT HUP INT TERM
printf '%s\n' 'int shen_lto_probe(int x) { return x + 1; }' > "$probe/helper.c"
printf '%s\n' 'extern int shen_lto_probe(int); int main(void) { return shen_lto_probe(41) != 42; }' > "$probe/main.c"
flags=${RC_PROBE_FLAGS:-}
if [ -z "$flags" ]; then
  compiler=$(${CC:-cc} --version 2>/dev/null) || exit 0
  case "$compiler" in
    *clang*|*Clang*) flags=-flto=thin ;;
    *GCC*|*gcc*|*'Free Software Foundation'*) flags=-flto ;;
    *) exit 0 ;;
  esac
fi
${CC:-cc} ${RC_PROBE_CFLAGS:-} $flags -c "$probe/helper.c" -o "$probe/helper.o" >/dev/null 2>&1 || exit 0
${AR:-ar} rcs "$probe/helper.a" "$probe/helper.o" >/dev/null 2>&1 || exit 0
${CC:-cc} ${RC_PROBE_CFLAGS:-} $flags "$probe/main.c" "$probe/helper.a" ${RC_PROBE_LDFLAGS:-} ${RC_PROBE_LDLIBS:-} -o "$probe/test" >/dev/null 2>&1 || exit 0
printf '%s\n' "$flags"
