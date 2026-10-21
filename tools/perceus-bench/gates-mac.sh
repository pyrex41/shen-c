#!/bin/sh
# Run every stage gate against a checkout: unit tests, canonical suite
# (release certify), toolchain receipt, Bifrost CLI corpus.
# usage: gates.sh CHECKOUT LOGDIR BIFROST_BIN
set -u
wt=$1; logs=$2; bifrost=$3
mkdir -p "$logs"; cd "$wt" || exit 2
export SHEN_RC_NATIVE_CACHE="$logs/native-cache"
# The host load swings between 25 and 170: give certify room.
export RC_SUITE_TIMEOUT=7200 RC_BUILD_TIMEOUT=7200 SHEN_RC_COMPILE_TIMEOUT=1200
status=0
step() { name=$1; shift; start=$(date +%s); "$@" > "$logs/$name.log" 2>&1; rc=$?
  echo "$name rc=$rc $(( $(date +%s) - start ))s" | tee -a "$logs/summary.txt"; [ $rc -eq 0 ] || status=1; }
echo "commit $(git rev-parse HEAD) $(date -u +%FT%TZ)" > "$logs/summary.txt"
step build make CC=clang -j4
step unit make CC=clang test
step certify env CC=clang CFLAGS=-O2 scripts/certify-rc.sh
receipt=$(ls -td evidence/rc/certify-*/result.json | head -1)
echo "release receipt $receipt" >> "$logs/summary.txt"
grep -h '"last_passed"\|"suite":' "$receipt" >> "$logs/summary.txt"
step toolchain env CC=clang CFLAGS=-O2 scripts/test-rc-toolchain.sh --canonical-receipt "$receipt"
# ASan/UBSan lanes run in gates-asan-linux.sh: ASan hangs at init on this host.
step bifrost env BIFROST_SHEN_C="$wt/bin/shen-c" "$bifrost" --impls shen-c --json
python3 - "$logs/bifrost.log" >> "$logs/summary.txt" <<'PY'
import re, sys
t = open(sys.argv[1]).read()
summary = re.findall(r"SUMMARY: .*", t)
cap = t.count("SKIPPED: every implementation reported an unavailable capability")
kern = t.count("skipped: no matching kernels")
print("bifrost", summary[-1] if summary else "SUMMARY UNKNOWN", "| capability skips", cap, "| kernel skips", kern)
PY
echo "status $status" >> "$logs/summary.txt"
exit $status
