#!/bin/sh
# Vendor the pinned community ShenOSKernel release byte for byte.
#
# USAGE
#   scripts/fetch-kernel.sh [--check] [ShenOSKernel-43.1.tar.gz]
#
# With no archive the release asset is downloaded from GitHub. Its SHA-256 is
# verified before extraction. Without --check the vendored trees are replaced
# by the archive's files; with --check they are compared and the script fails
# on any difference. The port boot module (shen/src/port) is not part of the
# release and is never touched.
#
#   ShenOSKernel-43.1/klambda/*.kl  -> shen/src/kl/
#   ShenOSKernel-43.1/LICENSE.txt   -> shen/src/kl/LICENSE.txt
#   ShenOSKernel-43.1/sources/      -> shen/src/shen/
#   ShenOSKernel-43.1/tests/        -> shen/test/43.1/
set -eu

KERNEL_VERSION=43.1
KERNEL_TAG=shen-$KERNEL_VERSION
KERNEL_COMMIT=715824a52b55e9c81a9501df8548ff3edd8411f5
KERNEL_SHA256=900eedef44f440af78f9395ed06e689ce1604546fc309f5bc7c8bc84cc651920
KERNEL_FOLDER=ShenOSKernel-$KERNEL_VERSION
KERNEL_URL=https://github.com/Shen-Language/shen-sources/releases/download/$KERNEL_TAG/$KERNEL_FOLDER.tar.gz

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
check=0
if [ "${1:-}" = --check ]; then check=1; shift; fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

archive=${1:-}
if [ -z "$archive" ]; then
  echo "Downloading $KERNEL_URL"
  curl -fsSL --max-time 300 "$KERNEL_URL" -o "$work/$KERNEL_FOLDER.tar.gz"
  archive=$work/$KERNEL_FOLDER.tar.gz
fi
if command -v sha256sum >/dev/null 2>&1; then
  got=$(sha256sum "$archive" | awk '{print $1}')
else
  got=$(shasum -a 256 "$archive" | awk '{print $1}')
fi
if [ "$got" != "$KERNEL_SHA256" ]; then
  echo "error: $KERNEL_FOLDER.tar.gz SHA-256 mismatch" >&2
  echo "  expected $KERNEL_SHA256" >&2
  echo "  got      $got" >&2
  exit 1
fi
echo "Verified $KERNEL_FOLDER.tar.gz (sha256 $KERNEL_SHA256; tag $KERNEL_TAG, commit $KERNEL_COMMIT)"

mkdir "$work/x"
tar -xzf "$archive" -C "$work/x"
release=$work/x/$KERNEL_FOLDER
if [ "$(cat "$release/VERSION")" != "$KERNEL_VERSION" ]; then
  echo "error: archive VERSION is not $KERNEL_VERSION" >&2
  exit 1
fi

# Assemble the expected vendored layout once, then copy or compare it.
expected=$work/expected
mkdir -p "$expected/shen/src/kl" "$expected/shen/src" "$expected/shen/test"
cp "$release"/klambda/*.kl "$expected/shen/src/kl/"
cp "$release/LICENSE.txt" "$expected/shen/src/kl/LICENSE.txt"
cp -R "$release/sources" "$expected/shen/src/shen"
cp -R "$release/tests" "$expected/shen/test/$KERNEL_VERSION"

status=0
for tree in shen/src/kl shen/src/shen shen/test/$KERNEL_VERSION; do
  if [ $check = 1 ]; then
    if ! diff -r "$expected/$tree" "$root/$tree" >/dev/null 2>&1; then
      echo "error: $tree differs from $KERNEL_FOLDER" >&2
      diff -rq "$expected/$tree" "$root/$tree" >&2 || true
      status=1
    else
      echo "$tree matches $KERNEL_FOLDER"
    fi
  else
    rm -rf "${root:?}/$tree"
    mkdir -p "$(dirname "$root/$tree")"
    cp -R "$expected/$tree" "$root/$tree"
    echo "Vendored $tree from $KERNEL_FOLDER"
  fi
done
exit $status
