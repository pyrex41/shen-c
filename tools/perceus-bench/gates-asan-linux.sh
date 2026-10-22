#!/bin/sh
# ASan/UBSan gates in a Linux container (ASan hangs at init on the Darwin 27
# host). usage: gates-asan-linux.sh REPO COMMIT LOGDIR
set -u
repo=$1; commit=$2; logs=$3; mkdir -p "$logs"
src="$logs/src"; rm -rf "$src"
git clone -q "$repo" "$src" && git -C "$src" checkout -q "$commit" || exit 2
docker run --rm -v "$src":/work -w /work ptight-asan:1 sh -c '
  set -u; export ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 RC_SUITE_TIMEOUT=14400 RC_BUILD_TIMEOUT=14400
  git config --global --add safe.directory /work
  make CC=clang AR=llvm-ar sanitize > /work/asan-unit.log 2>&1; echo "asan-unit rc=$?"
  CC=clang AR=llvm-ar CFLAGS="-O1 -g -DSHEN_RC_VERIFY_OWNERSHIP=1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    scripts/certify-rc.sh > /work/asan-certify.log 2>&1; echo "asan-certify rc=$?"
  r=$(ls -td evidence/rc/certify-*/result.json | head -1); grep -h "\"last_passed\"\|\"suite\":" "$r"
' > "$logs/asan-summary.txt" 2>&1
echo "commit $(git -C "$src" rev-parse HEAD)" >> "$logs/asan-summary.txt"
cat "$logs/asan-summary.txt"
