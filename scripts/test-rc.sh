#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

audit()
{
    artifact=$1
    if [ ! -f "$artifact" ]; then
        echo "RC audit: missing artifact $artifact" >&2
        return 1
    fi
    # Check imports and linked libraries; a statically embedded GC is caught by
    # checking all global symbols as well as just undefined imports.
    symbols=$("${NM:-nm}" -g "$artifact")
    if printf '%s\n' "$symbols" | LC_ALL=C grep -E '(^|[[:space:]_])GC_[[:alnum:]_]+'; then
        echo "RC audit: Boehm symbol in $artifact" >&2
        return 1
    fi
    case $(uname -s) in
        Darwin) dependencies=$("${OTOOL:-otool}" -L "$artifact") ;;
        Linux) dependencies=$(ldd "$artifact" 2>&1) || {
            case "$dependencies" in *'not a dynamic executable'*|*'statically linked'*) ;; *)
                printf '%s\n' "$dependencies" >&2; return 1 ;;
            esac
        } ;;
        *) echo "RC audit: unsupported dependency scanner" >&2; return 1 ;;
    esac
    if printf '%s\n' "$dependencies" | LC_ALL=C grep -Ei '(^|[/[:space:]])lib(gc|bdwgc)([. -]|$)'; then
        echo "RC audit: Boehm dependency in $artifact" >&2
        return 1
    fi
    printf 'RC artifact audit PASS: %s\n' "$artifact"
}

if [ "${1:-}" = --audit-only ]; then
    shift
    [ "$#" -gt 0 ] || { echo 'usage: test-rc.sh --audit-only ARTIFACT ...' >&2; exit 2; }
    for artifact in "$@"; do audit "$artifact"; done
    exit 0
fi

if [ "${1:-}" = --no-build ]; then
    shift
else
    make -f Makefile.rc test-binaries
fi
bin_dir=${RC_BIN:-bin/rc}
for test in test_cycles test_primitives test_eval test_env test_bool_tail test_apply_fast test_object_cache test_immediates test_collection test_reuse test_acyclic test_symbols test_primitive_fast test_equal test_canonical_primitives test_canonical_numeric test_callee; do
    "$bin_dir/$test"
    audit "$bin_dir/$test"
done
# Runtime-only checks can run independently of the native compiler.
# Passing --runtime-only selects that scope explicitly; no silent skipping.
if [ "${1:-}" = --runtime-only ]; then
    echo 'RC emitted semantics: NOT RUN (explicit --runtime-only scope)'
    exit 0
fi
audit "${1:-$root/bin/rc-build}"
"$root/test/rc/test_semantics.sh" "${1:-$root/bin/rc-build}"
"$root/test/rc/test_symbols.sh" "${1:-$root/bin/rc-build}"
"$root/test/rc/test_callee.sh" "${1:-$root/bin/rc-build}"
"$root/test/rc/test_frames.sh" "${1:-$root/bin/rc-build}"
"$root/test/rc/test_frame_compaction.sh" "${1:-$root/bin/rc-build}"
"$root/test/rc/test_null_cleanup.sh" "${1:-$root/bin/rc-build}"
"$root/test/rc/test_builder_paths.sh" "${1:-$root/bin/rc-build}"
"$root/test/rc/test_ownership.sh" "${1:-$root/bin/rc-build}"
"$root/test/rc/test_primitive_fast.sh" "${1:-$root/bin/rc-build}"
"$root/test/rc/test_canonical_emit.sh" "${1:-$root/bin/rc-build}"
