#!/bin/sh
# ASan/UBSan gates in a Linux container (ASan hangs at init on the Darwin 27
# host). usage: gates-asan-linux.sh CHECKOUT LOGDIR
set -u
wt=$1; logs=$2; mkdir -p "$logs"
docker run --rm -v "$wt":/work -w /work ptight-asan:1 sh -c '
  set -u; export ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1
  rm -rf obj bin evidence
  make CC=clang AR=llvm-ar sanitize > /work/asan-unit.log 2>&1; echo "asan-unit rc=$?"
  CC=clang AR=llvm-ar CFLAGS="-O1 -g -DSHEN_RC_VERIFY_OWNERSHIP=1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    scripts/certify-rc.sh > /work/asan-certify.log 2>&1; echo "asan-certify rc=$?"
  r=$(ls -td evidence/rc/certify-*/result.json | head -1); grep -h "\"last_passed\"\|\"suite\":" "$r"
' > "$logs/asan-summary.txt" 2>&1
cp "$wt"/asan-unit.log "$wt"/asan-certify.log "$logs"/ 2>/dev/null
cat "$logs/asan-summary.txt"
