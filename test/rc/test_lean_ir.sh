#!/bin/sh
# Lean C IR (src/rc/lean.c): goldens of the Core and LIR of two worked
# examples (DESIGN B1 §1.1-1.2), lowering and lir_check of every kernel defun,
# and the explicit seal list.
#   test_lean_ir.sh [RC_BUILD]          check
#   test_lean_ir.sh --regen [RC_BUILD]  rewrite the goldens
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
regen=0
if [ "${1:-}" = --regen ]; then regen=1; shift; fi
builder=${1:-"$root/bin/rc-build"}
dir=$root/test/rc/lean
tmp=$(mktemp -d "${TMPDIR:-/tmp}/rc-lean-ir.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
cd "$root"

golden() { # name output-file [extra.kl]
    name=$1 out=$2; shift 2
    "$builder" --dump-lir "$name" . "$@" > "$tmp/$out"
    if [ $regen = 1 ]; then cp "$tmp/$out" "$dir/$out"; echo "regenerated $dir/$out"
    elif ! cmp -s "$tmp/$out" "$dir/$out"; then
        diff -u "$dir/$out" "$tmp/$out" >&2 || true
        echo "lean IR golden mismatch: $out (rerun with --regen after review)" >&2; exit 1
    fi
}
golden 'shen.unlocked?' unlocked.lir
golden lean-golden.system-S-h-clause-8 system-S-h-clause-8.lir "$dir/system-S-h-clause-8.kl"
[ $regen = 1 ] && exit 0

# Every kernel defun lowers and passes lir_check; nothing is rejected.
"$builder" --check-lir . > "$tmp/check.txt"
tail -n 1 "$tmp/check.txt"
grep -q ' 0 rejected, 0 failed;' "$tmp/check.txt" || { cat "$tmp/check.txt" >&2; exit 1; }

# Seal list: a never-sealed name cannot join the lean set ...
for name in shen.hashkey hash 'symbol?' not eval-kl put shen.record-kl 'shen.store-arity'; do
    printf '%s\n' "$name" > "$tmp/set.txt"
    if "$builder" --lean-set "$tmp/set.txt" --check-lir . > /dev/null 2> "$tmp/err.txt"; then
        echo "seal list: $name accepted as a lean target" >&2; exit 1
    fi
    grep -q "never-seal" "$tmp/err.txt" || { cat "$tmp/err.txt" >&2; exit 1; }
done
# ... calls to never-sealed names go through their binding or a guarded
# primitive, never a direct call ...
"$builder" --dump-lir shen.f-error . > "$tmp/f-error.txt"
"$builder" --dump-lir hash . > "$tmp/hash.txt"
"$builder" --dump-lir 'shen.<-dict' . > "$tmp/dict.txt"
grep -q 'gcall hash(' "$tmp/dict.txt" || { cat "$tmp/dict.txt" >&2; echo "hash not called through its binding" >&2; exit 1; }
grep -q 'gcall shen.hashkey(' "$tmp/hash.txt" || { cat "$tmp/hash.txt" >&2; echo "hashkey not called through its binding" >&2; exit 1; }
if grep -E '(^| )call (shen.hashkey|hash|not|symbol\?|eval-kl|put)\(' "$tmp/hash.txt" "$tmp/f-error.txt" "$tmp/dict.txt"; then echo "never-sealed name called directly" >&2; exit 1; fi
# ... shen.pvar? is called only under a guard, and outside the lean set a
# kernel defun is called through its binding.
printf 'shen.lazyderef\n' > "$tmp/set.txt"
"$builder" --lean-set "$tmp/set.txt" --dump-lir shen.lazyderef . > "$tmp/lazy.txt"
grep -q 'gcall shen.pvar?(V2111)' "$tmp/lazy.txt" || { cat "$tmp/lazy.txt" >&2; exit 1; }
grep -q 'call shen.lazyderef(' "$tmp/lazy.txt"
printf 'shen.lazyderef\nshen.pvar?\n' > "$tmp/set.txt"
"$builder" --lean-set "$tmp/set.txt" --dump-lir shen.lazyderef . > "$tmp/lazy.txt"
grep -q '(guarded shen.pvar? V2111)' "$tmp/lazy.txt" || { cat "$tmp/lazy.txt" >&2; exit 1; }
# Unknown names are refused.
printf 'lean-test.no-such-defun\n' > "$tmp/set.txt"
if "$builder" --lean-set "$tmp/set.txt" --check-lir . > /dev/null 2>&1; then echo "unknown lean set name accepted" >&2; exit 1; fi
echo "RC lean IR: goldens, lowering and lir_check of every kernel defun, seal list PASS"
