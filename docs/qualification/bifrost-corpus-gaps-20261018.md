# Shen-C compatibility gap closure

Work starts at `802e724` on `fix/bifrost-corpus-gaps`, with separate worktrees
for the script-output and numeric-hash fixes. Yggdrasil's dynamic-load closure
is developed separately from its `23a2f39` native RC integration checkpoint.

1. Fix script mode's implicit result echo without suppressing explicit language
   output, changing eval/REPL printing, or discarding errors and arguments.
2. Make numeric hashes consistent with equality, including signed zero and
   mixed integer/float edge cases, without weakening the corpus oracle.
3. Preserve reachable dependencies of statically available loaded Shen source
   in shaken artifacts. Verify the typechecked interpreter's actual result.
4. Review each isolated diff and focused controls; integrate the Shen-C fixes,
   then run default/boxed fatal sanitizer runtime checks, the unchanged complete
   43.1 suite, and the Bifrost corpus with exact executed/skipped counts.
5. Update README with the measured string-copy improvements and their limits.
   Preserve earlier receipts; append source/binary-bound follow-up evidence.
6. Push fixes and update the integration PRs. Keep any unqualified platforms or
   broader suite gaps explicit; a zero exit alone is not a semantic oracle.

## Verified candidate

The two Shen-C corpus failures are fixed on `fix/bifrost-corpus-gaps`.
Production sources were frozen at `5e44057`; `32db412` adds both-ABI test
coverage and matched CI tool selection; `97758d5` keeps assertions enabled
under Release `NDEBUG` builds, with an additional executed control. Vendor kernel, Shen sources, release
manifest, suite and Bifrost goldens remain unchanged. These are post-v0.3.0
changes; the v0.3.0 tag and installation pin have not moved.

Script mode installs an ordinary Shen evaluator helper that omits only the
outer return-value print. Calls resolve normally for every form; explicit
output, arguments, errors and nested loads remain observable. `eval -l` and
REPL printing are unchanged. Typechecked nested loads retain their existing
type/result diagnostics; the quiet helper handles ordinary untyped loading.

For the canonical `shen.hashkey` body, the compiler installs a private wrapper
before kernel initialization forms. Integral floats in the exact Int64 range
use their equal integer key, then call the original kernel implementation.
An alpha-safe structural guard rejects custom bodies. Captured dependency
identities provide a generic fallback on redefinition. Ordinary string/number
printing is unchanged, and nonnumeric keys avoid the dependency-check loop.
Owned and borrowed compiler paths retain their C ownership contracts.

| Check (Darwin arm64 / Apple Clang 21 unless stated) | Result |
|---|---|
| Complete unchanged Bifrost CLI corpus | **70 executed PASS, 0 FAIL, 2 capability skips, 2 kernel skips** |
| Fatal ASan/UBSan runtime tests, default and boxed | **25/25 each** |
| Complete 43.1 generated kernel and suite, release | **136/0**, all reports/redirect/audits pass |
| Fully instrumented generated kernel and suite | **136/0**, fatal ASan/UBSan and ownership verification |
| Fresh compiler/CLI, source compilation, native controls | **258 checks PASS**, canonical receipt verified |
| Hash boundaries, both callback ABIs, redefinitions | PASS, including Int64 bounds, signed zero, mixed-key put/get, custom kernel bodies |
| CMake hash regression, host SDK 27 | PASS under fatal ASan/UBSan |
| Pinned full-corpus Linux/macOS CI at `97758d5` | **PASS**, [run 39159931780](https://github.com/pyrex41/shen-c/actions/runs/39159931780) |
| Complete Linux Clang/GCC, macOS Clang and fatal sanitizer CI at `97758d5` | **PASS**, [run 39159931894](https://github.com/pyrex41/shen-c/actions/runs/39159931894) |
| Bifrost native adapter Linux/macOS CI at `9ddd2d0` | **PASS**, [run 39160171330](https://github.com/pyrex41/bifrost/actions/runs/39160171330); released v0.3.0 pin unchanged |

The first CMake attempt mixed the Nix 14.4 SDK with the host sanitizer runtime
and stalled in ASan initialization before `main`. Its stack sample and failed
receipt are retained. The successful CMake run uses `/usr/bin/clang` with the
host SDK and deployment target 27.0. The Make and complete instrumented-kernel
runs passed independently.

The original script loop was rejected because it cached function bindings
across forms. An unconditional hash override and modified vendor files were
also rejected. Final code delegates to normal evaluation and the guarded,
original hash implementation instead.

## Reproduction

```sh
make CC=clang -j2
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 make CC=clang sanitize
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  make -f Makefile.rc CC=clang RC_SANITIZE=1 \
  RC_CPPFLAGS='-DSHEN_RC_VERIFY_OWNERSHIP=1 -DSHEN_RC_IMMEDIATE_INTEGERS=0' \
  RC_BUILD=obj/boxed RC_BIN=bin/boxed test
CC=clang CFLAGS=-O2 scripts/certify-rc.sh
CC=clang CFLAGS='-O1 -g -DSHEN_RC_VERIFY_OWNERSHIP=1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 scripts/certify-rc.sh
CC=clang CFLAGS=-O2 scripts/test-rc-toolchain.sh --canonical-receipt PATH_TO_RELEASE_RECEIPT
BIFROST_SHEN_C="$PWD/bin/shen-c" /path/to/bifrost --impls shen-c --json
```

The corpus is pinned to Bifrost `def3047`; require 74 rows with exactly the
executed/skipped counts above. The CI lane checks those counts rather than
crediting the aggregate summary's inclusion of skips as passes. Linux Clang
uses matching `clang-18`/`llvm-ar-18` tools; mismatched Clang/archiver plugins
are not a runtime result.

[Portable receipts and original/published hashes](../evidence/corpus-gaps-20261018/publication.json)
include source, generated C, binaries, dependencies, suite and corpus input
identities. Original build trees remain in `evidence/rc/`. The companion
[Yggdrasil follow-up](https://github.com/pyrex41/yggdrasil/blob/fix/shen-c-native-rc/docs/dynamic-load-20261018.md)
closes the specific shaken typechecker/interpreter dependency gap.
No new performance measurement or release is claimed here.
