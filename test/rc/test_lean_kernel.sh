#!/bin/sh
# Lean C kernel against the generic emitter, on the same pinned kernel.
# The two generated kernels may differ only where the lean backend is
# entitled to: the lean prelude (runtime header and guard symbol table) and
# the bootstrap's rc_lean_install, which runs after every install and before
# the kernel's top-level forms. Every generated function must be identical
# unless it is a lean function (none yet).
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/rc-lean-kernel.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
cd "$root"
"$builder" --emit-kernel . "$tmp/generic.c" 2> "$tmp/generic.log"
"$builder" --lean --emit-kernel . "$tmp/lean.c" 2> "$tmp/lean.log"
python3 - "$tmp/generic.c" "$tmp/lean.c" <<'PY'
import difflib, re, sys
generic = open(sys.argv[1]).read().splitlines()
lean = open(sys.argv[2]).read().splitlines()
added, removed = [], []
for op, a0, a1, b0, b1 in difflib.SequenceMatcher(None, generic, lean, autojunk=False).get_opcodes():
    if op in ('replace', 'delete'): removed += generic[a0:a1]
    if op in ('replace', 'insert'): added += lean[b0:b1]
assert not removed, ('generic lines missing from the lean kernel', removed[:5])
# Allowed additions: the prelude block and the install statement.
start = lean.index('/* Lean C (RC_LEAN=1): runtime support and the guard symbol table. */')
end = lean.index([l for l in lean[start:] if l.startswith('static const RcSymbolTable rc_lean_symbols=')][0], start)
prelude = lean[start:end + 1]
install = ['  if(!rc_lean_install(ctx,&rc_lean_symbols)) return false;']
assert added == prelude + install, ('unexpected lean additions', [l for l in added if l not in prelude + install][:5])
assert lean.count(install[0]) == 1
boot = lean.index('bool rc_bootstrap_kernel(RcContext *ctx) {')
at = lean.index(install[0])
assert boot < at and 'rc_install_overwrites' in '\n'.join(lean[boot:at]), 'install precedes the bootstrap installs'
after = '\n'.join(lean[at:])
assert 'run_top' in after, 'install after the top-level forms'
names = re.findall(r'^  "([^"]+)"', '\n'.join(prelude), re.M)
for required in ['=', 'hd', 'tl', 'cons?', 'absvector?', '<-address', 'address->', 'set', 'value', 'shen.pvar?', 'do']:
    assert required in names, required
print('RC lean kernel: generic and lean kernels identical apart from the prelude (%d guard symbols) and rc_lean_install PASS' % len(names))
PY
