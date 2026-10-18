#!/bin/sh
# Build one frozen runtime variant with the common native kernel/main objects.
# In a portable publication, decompress .c.gz sources to .c before this build.
# Worktrees must be preserved separately: baseline 0753780, A ae24940, B 770c39b.
# The baseline uses the four-warmup probe driver from this experiment, not the
# original one-warmup driver. Source hashes in freeze.json specify exact bytes.
# Usage: sh reproduce.sh RUNTIME_ROOT SHARED_RELEASE_ROOT FRESH_OUTPUT [0|1]
set -eu
[ "$#" -ge 3 ] && [ "$#" -le 4 ] || { echo 'usage: reproduce.sh RUNTIME_ROOT SHARED_RELEASE_ROOT FRESH_OUTPUT [0|1]' >&2; exit 2; }
fusion_runtime_root=$1
fusion_release_root=$2
fusion_output=$3
fusion_views=${4:-0}
case "$fusion_views" in 0|1) ;; *) exit 2;; esac
fusion_driver_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir "$fusion_output"
fusion_output=$(CDPATH= cd -- "$fusion_output" && pwd)
fusion_release_root=$(CDPATH= cd -- "$fusion_release_root" && pwd)
cd "$fusion_runtime_root"
# For the pre-view baseline/A the value=0 definition is harmless and absent
# from recorded build commands; use no extra macro for those exact builds.
fusion_cppflags=
if [ "$fusion_views" = 1 ]; then fusion_cppflags=-DSHEN_RC_STRING_SLICES=1; fi
make -f Makefile.rc CC=/usr/bin/cc RC_BUILD="obj/fusion-reproduce-$fusion_views" RC_BIN="bin/fusion-reproduce-$fusion_views" RC_CFLAGS='-O3 -g' RC_SANITIZE=0 RC_LTO=1 RC_LTO_FLAGS=-flto=thin RC_CPPFLAGS="$fusion_cppflags" runtime
/usr/bin/cc -O3 -g -std=c17 -flto=thin $fusion_cppflags -Isrc/rc "$fusion_driver_root/bench_string_vector.c" "$fusion_release_root/obj/rc/kernel.o" "bin/fusion-reproduce-$fusion_views/libshenrc.a" -lm -o "$fusion_output/probe"
/usr/bin/cc -O3 -g -std=c17 -flto=thin "$fusion_release_root/obj/rc/main.o" "$fusion_release_root/obj/rc/kernel.o" "bin/fusion-reproduce-$fusion_views/libshenrc.a" -lm -o "$fusion_output/shen-c"
# For diagnostic builds repeat in DIFFERENT runtime/output directories and add
# -DSHEN_RC_FUSION_DIAGNOSTICS=1 to BOTH runtime and probe compilation.
# CLI diagnostics are emitted on context destruction with SHEN_RC_FUSION_STATS=1.
# Freeze binary/source hashes before measurement. Published freeze receipts
# record the original shared objects; reconstructing them may change hashes.
#
# Actual matched commands, from the experiment checkout:
# python3 scripts/benchmark-string-vector.py --baseline evidence/rc/fusion-20261018/baseline/probe-warm --candidate evidence/rc/fusion-20261018/candidate-a/probe --forms evidence/rc/fusion-20261018/probes.kl --out evidence/rc/fusion-20261018/micro-five-pairs --repetitions 5
# python3 scripts/benchmark-rc.py --config evidence/rc/fusion-20261018/matched-config.json --out evidence/rc/fusion-20261018/suite-five-pairs --timeout 60
# python3 scripts/benchmark-string-vector.py --baseline evidence/rc/fusion-20261018/candidate-a/probe --candidate evidence/rc/fusion-20261018/candidate-b/probe --forms evidence/rc/fusion-20261018/probes.kl --out evidence/rc/fusion-20261018/micro-b-five-pairs --repetitions 5
# python3 scripts/benchmark-rc.py --config evidence/rc/fusion-20261018/matched-b-config.json --out evidence/rc/fusion-20261018/suite-b-five-pairs --timeout 60
# Commands require fresh output dirs. The A config binds A's old source hashes;
# use its source checkout and rebuild the configuration when reconstructing.
# Published paths contain placeholders and must be resolved to the local roots.
#
# Fatal runtime qualification, from the appropriate source checkout:
# ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make -f Makefile.rc CC=/usr/bin/cc RC_LTO=0 RC_SANITIZE=1 RC_BUILD=obj/fusion-san RC_BIN=bin/fusion-san RC_CPPFLAGS='-DSHEN_RC_STRING_SLICES=1 -DSHEN_RC_FUSION_DIAGNOSTICS=1' test
# Repeat in fresh dirs with -DSHEN_RC_IMMEDIATE_INTEGERS=0 added for boxed mode.
# Repeat current source in fresh dirs without either macro for default mode.
#
# Full regenerated native kernel, compiler and untouched suite:
# CC=/usr/bin/cc CFLAGS='-O1 -g -DSHEN_RC_STRING_SLICES=1 -DSHEN_RC_VERIFY_OWNERSHIP=1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 scripts/certify-rc.sh
# A used the same command without -DSHEN_RC_STRING_SLICES=1.
#
# Focused CMake qualification (runtime tests, not the full suite):
# cmake -S . -B build-fusion -DCMAKE_BUILD_TYPE=Debug -DSHEN_RC_SANITIZE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_C_FLAGS=-DSHEN_RC_STRING_SLICES=1
# cmake --build build-fusion --target test_rc_string_copy test_rc_string_views
# ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-fusion -R 'rc_string_(copy|views)' --output-on-failure
#
# Native escaped-view/closure-cycle host control (generated app.c is local):
# /usr/bin/cc -O1 -g -DSHEN_RC_STRING_SLICES=1 -DSHEN_RC_FUSION_DIAGNOSTICS=1 -DSHEN_RC_VERIFY_OWNERSHIP=1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -std=c17 -Isrc/rc evidence/rc/fusion-20261018/candidate-b/native-smoke-host.c src/rc/rc.c src/rc/primitives.c src/rc/eval.c src/rc/read.c -lm -o evidence/rc/fusion-20261018/candidate-b/native-smoke-host
# ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 SHEN_RC_FUSION_STATS=1 evidence/rc/fusion-20261018/candidate-b/native-smoke-host
