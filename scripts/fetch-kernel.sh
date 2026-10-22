#!/bin/sh
# Vendor a pinned ShenOSKernel release byte for byte.
#
# USAGE
#   scripts/fetch-kernel.sh [--check] [--version 43.1-ec16a9f|43.1] [ShenOSKernel-VERSION.tar.gz]
#
# With no archive the release asset is downloaded. Its SHA-256 is verified
# before extraction. Without --check the vendored trees are replaced by the
# archive's files; with --check they are compared and the script fails on any
# difference. The port boot module (shen/src/port) is not part of a release
# and is never touched.
#
# 43.1-ec16a9f (default): an unmodified build of Shen-Language/shen-sources
# commit ec16a9f, tizoc's merge of #117 (published as pyrex41/shen-sources
# release upstream-ec16a9f; (version) is "43.1"):
#   klambda/*.kl and LICENSE.txt -> shen/src/kl/
#   sources/                     -> shen/src/shen/
#   tests/                       -> shen/test/43.1/
# 43.1 (Shen-Language/shen-sources):
#   klambda/*.kl and LICENSE.txt -> shen/releases/43.1/kl/
#   tests/                       -> shen/test/43.1/
# The two releases ship byte-identical tests/, so they share shen/test/43.1.
set -eu

check=0
KERNEL_VERSION=43.1-ec16a9f
while [ $# -gt 0 ]; do
  case $1 in
    --check) check=1; shift ;;
    --version) KERNEL_VERSION=$2; shift 2 ;;
    *) break ;;
  esac
done

case $KERNEL_VERSION in
  43.1-ec16a9f)
    KERNEL_REPO=pyrex41/shen-sources KERNEL_TAG=upstream-ec16a9f
    KERNEL_COMMIT=ec16a9fa4fc2e87e2aa336fb1ae30e8ac2266394
    KERNEL_SHA256=060066a8f84d9a2f7d41cb3b37b986a8d4653279752b739b5063cd648d9baacd
    KL_DIR=shen/src/kl SOURCES_DIR=shen/src/shen ;;
  43.1)
    KERNEL_REPO=Shen-Language/shen-sources KERNEL_TAG=shen-43.1
    KERNEL_COMMIT=715824a52b55e9c81a9501df8548ff3edd8411f5
    KERNEL_SHA256=900eedef44f440af78f9395ed06e689ce1604546fc309f5bc7c8bc84cc651920
    KL_DIR=shen/releases/43.1/kl SOURCES_DIR= ;;
  *) echo "error: unknown kernel version $KERNEL_VERSION" >&2; exit 2 ;;
esac
TESTS_DIR=shen/test/43.1
KERNEL_FOLDER=ShenOSKernel-$KERNEL_VERSION
KERNEL_URL=https://github.com/$KERNEL_REPO/releases/download/$KERNEL_TAG/$KERNEL_FOLDER.tar.gz

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
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
echo "Verified $KERNEL_FOLDER.tar.gz (sha256 $KERNEL_SHA256; $KERNEL_REPO tag $KERNEL_TAG, commit $KERNEL_COMMIT)"

mkdir "$work/x"
tar -xzf "$archive" -C "$work/x"
release=$work/x/$KERNEL_FOLDER
if [ "$(cat "$release/VERSION")" != "$KERNEL_VERSION" ]; then
  echo "error: archive VERSION is not $KERNEL_VERSION" >&2
  exit 1
fi

# Assemble the expected vendored layout once, then copy or compare it.
expected=$work/expected
mkdir -p "$expected/$KL_DIR" "$(dirname "$expected/$TESTS_DIR")"
cp "$release"/klambda/*.kl "$expected/$KL_DIR/"
cp "$release/LICENSE.txt" "$expected/$KL_DIR/LICENSE.txt"
cp -R "$release/tests" "$expected/$TESTS_DIR"
trees="$KL_DIR $TESTS_DIR"
if [ -n "$SOURCES_DIR" ]; then
  mkdir -p "$(dirname "$expected/$SOURCES_DIR")"
  cp -R "$release/sources" "$expected/$SOURCES_DIR"
  trees="$trees $SOURCES_DIR"
fi

status=0
for tree in $trees; do
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
