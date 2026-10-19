# Compile-first runtime: packed-cache checkpoint, 2026-10-19

Status: **OPEN**. This is a functional and exploratory performance checkpoint,
not repeated performance acceptance against Boehm.

The default backend compiles loaded source, eval-kl, REPL forms and generated
rules before execution. Native lazy closures defer compilation until first call;
no body executes through the interpreter. Canonical C separates machine-code
shape from live literal bindings, preserving mutable value identity across calls
and contexts. Code remains resident until process exit.

The initial 14-module a813870 checkpoint passed the complete 136-test suite in
cold and cached native configurations, with zero interpreter entries. Its fully
instrumented generated-kernel ASan/UBSan run also passed 136/0. Cold compilation
is measured separately; cold-start parity is not a gate.

## Why pack cached code

The initial cached run loaded 362 individual shared objects. It reported
262.82 seconds in loading and 310.89 seconds wall time on a shared Darwin host.
Those no-LTO timings identified a removable cost; they are not a matched speedup
baseline for the subsequent LTO build.

The bounded candidate packs already generated expression modules into one shared
image. Lookup compares both the ABI key and complete current canonical C bytes.
Live literal vectors still come from the current execution. Missing, damaged,
or mismatched packs use the ordinary native cache/compiler path. Packs never
select an interpreter. The explicit preparation command limits each image to
512 modules and 32 MiB of canonical C; compiled images remain resident. Pack
artifacts are separate from the per-module disk-cache budget.

The latest exploratory run includes main's standard library and initialization
modules (16 boot modules), script-load fixes and recursion safeguards. It ran
on Darwin arm64 with Apple clang, host -O3/thin LTO and dynamic modules -O1.
The initial pack was rebuilt for the launcher's recorded SDK environment;
the mismatched-ABI exploratory run was stopped and retained as FAIL.

| Matched diagnostic configuration | User CPU | Wall | Peak RSS |
| --- | ---: | ---: | ---: |
| Explicit interpreter | 5.182s | 5.471s | 38.48 MiB |
| Packed native cache | 5.221s | 6.779s | 79.95 MiB |

Both configurations passed 136/0 and produced identical output after masking
only upstream elapsed-time reports. Native execution recorded 363 pack hits,
zero compiler calls, zero interpreter entries and 0.929 seconds loading. This
single pair establishes behavior and removes the observed loader bottleneck;
it does not establish a CPU or memory advantage over Boehm.

Private local evidence (not committed caches or binaries):

- `shen-c-compile-first/evidence/rc/compile-first-20261019T160745Z`: initial cold/warm qualification.
- `shen-c-compile-first/evidence/rc/compile-first-20261019T163329Z`: complete fatal ASan/UBSan native suite.
- `shen-c-compile-first-bundle/evidence/rc/compile-first-20261019T175500Z`: stopped wrong-pack-ABI exploratory run, retained FAIL.
- `shen-c-compile-first-bundle/evidence/rc/compile-first-20261019T180215Z`: packed suite PASS; source and binary hashes unchanged.

## Reproduce

Build release tools and create one native cache configuration:

```sh
make -f Makefile.rc CC=clang RC_LTO=1 all bin/rc/test_native_compile
python3 scripts/test-compile-first.py bin/shen-rc --configs interpret,native-cold --cache "$PWD/evidence/native-cache" --timeout 3600
```

Pack the cache using the ABI in its `compile-*.config` file. If more than one
configuration exists, pass `--abi HEX` explicitly. Use the same SDK/compiler
environment as the suite launcher; a pack from another configuration safely
misses. Unseen code shapes compile normally, so warm the complete workload
before the final pack and freeze it before timing:

```sh
python3 scripts/bundle-native-cache.py --cache evidence/native-cache --abi HEX
python3 scripts/test-compile-first.py bin/shen-rc --configs interpret,native-warm --cache evidence/native-cache
python3 scripts/test-compile-cache.py bin/rc/test_native_compile
```

`--source-receipt` optionally seeds canonical C from a previous qualified cache.
The script verifies those source hashes, rebuilds with current headers/flags,
and the loader still requires complete current-emission equality. It never
reuses old machine code. Older canonical modules can acquire the current
function-entry stack guard before recompilation.

## Remaining gates

Finish current packed-code default/boxed and generated-kernel sanitizer
qualification, source/compiler/metadata controls, Make/CMake and macOS CI.
Linux clang/GCC, ASan/UBSan and Linux/macOS Bifrost CI passed the earlier
0af960e checkpoint; a cancelled macOS CI job remains unqualified.

Rebuild the historical Boehm comparator with the same complete module order,
compiler, SDK and comparable release settings. Freeze both implementations and
cache artifacts, run two five-pair alternating batches on a quiet host and keep
every sample. Require median paired user CPU <=0.90 in each batch, eight CPU
wins, wall regression <=3%, the agreed RSS bounds and complete unchanged suite
success. If CPU remains short, profile the packed release build separately
before choosing the next ownership or dispatch change.

The later [16-module generated-kernel sanitizer receipt](../evidence/compile-first-sanitizer-20261019/README.md) passes 136/0 and clean process exit after the CLI alternate-stack restoration fix. This supersedes the earlier shutdown failure; platform CI and repeated Boehm acceptance remain separate gates.

The [native optimization checkpoint](native-optimization-20261019.md) records the next ownership candidate, noisy whole-suite results, rejected oversized preparation and the measured generated-C compiler bottleneck.
