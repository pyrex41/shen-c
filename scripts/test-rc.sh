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
for test in test_cycles test_primitives test_eval test_bool_tail test_collection; do
    "$bin_dir/$test"
    audit "$bin_dir/$test"
done
# The standalone heap/evaluator suite can run without the legacy compiler host.
# Passing --runtime-only selects that scope explicitly; no silent skipping.
if [ "${1:-}" = --runtime-only ]; then
    echo 'RC emitted semantics: NOT RUN (explicit --runtime-only scope)'
    exit 0
fi
"$root/test/rc/test_semantics.sh" "${1:-$root/bin/rc-build}"
"$root/test/rc/test_frames.sh" "${1:-$root/bin/rc-build}"
"$root/test/rc/test_builder_paths.sh" "${1:-$root/bin/rc-build}"
