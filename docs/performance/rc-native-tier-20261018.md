# Native tier for hot runtime-defined defuns (issue #8, first slice)

**Status: opt-in and experimental. It is off unless `SHEN_RC_NATIVE` is
set.** With a warm cache the tier cuts complete-suite user CPU by about 10%
and the in-suite L-interpreter load by about 15%. Without a warm cache it is
a large net loss: compiling the hot defuns costs roughly 4x the CPU of the
whole suite. Timing was taken on a shared, loaded host, so it is labeled
UNKNOWN until it is requalified.

## Design

- **Counting.** `eval.c` exposes a per-context observer (`RcTierHooks` in
  `rc_internal.h`). The runtime and generated applications never attach one.
  The CLI attaches `src/rc/native_tier.c`, which registers each interpreted
  `defun` together with its retained syntax, closure and name, and counts
  calls at both interpreted entry points (`dynamic_call` and
  `prepared_call`). Plan promotion replaces a closure's body, so the syntax
  is kept at definition time.
- **Promotion.** A defun becomes pending when it reaches
  `SHEN_RC_NATIVE_THRESHOLD` calls. The default is 1000; 0 promotes at
  definition. At the next `eval-kl` entry, every pending defun goes into one
  module-mode translation unit, with at most 512 per unit. A binding change
  at that point is exactly as safe as an ordinary runtime redefinition.
- **Emission.** `RcEmitOptions.module_entry` emits defuns only. It installs
  no primitives and offers each native closure to a host callback, which
  publishes the closure only if the symbol is still bound to the exact
  interpreted closure that was promoted. A redefinition retires the old
  registration, so later definitions always win. Generated code follows the
  same ownership protocol as `rc-build` output. That includes
  `SHEN_RC_VERIFY_OWNERSHIP` and the sanitizer flags of the host build,
  which are passed to the module compile.
- **Compiling and loading.** The tier runs `$SHEN_RC_NATIVE_CC` (default:
  the build compiler) with `SHEN_RC_NATIVE_CFLAGS` (default `-O1`) plus
  `-shared -fPIC -fvisibility=hidden` (and `-undefined dynamic_lookup` on
  Darwin) through `posix_spawnp`. Compiler output goes to a log file and
  never to the inherited descriptors. The resulting object is loaded with
  `dlopen(RTLD_NOW|RTLD_LOCAL)`, which resolves the runtime from the host,
  so there is never a second copy of the runtime. The CLI links with
  `-Wl,-export_dynamic` (Darwin) or `-rdynamic -ldl` (ELF); this also stops
  LTO from internalizing runtime symbols the kernel itself does not use.
  The runtime headers are embedded in the CLI because it can run outside
  the source tree. Objects are never `dlclose`d, because compiled symbol
  tables and closures point into them.
- **Cache.** `SHEN_RC_NATIVE_CACHE` sets the cache directory. The default is
  `$XDG_CACHE_HOME/shen-c/native` or `~/.cache/shen-c/native`; `off` uses a
  private temporary directory. Each object's key is an injective, type-tagged
  serialization of the batch's KL. The key also includes digests of the
  embedded headers and the emitter sources, the compiler, its flags and the
  host build flags. The key text is stored beside the object, and a hit
  requires byte equality, so a digest collision cannot load the wrong code.
  A warm hit skips both emission and compilation. Objects are written
  atomically, and loading happens only from a directory that the user owns
  and that no one else can write to.
- **Fallback.** A missing compiler (spawn failure or exit 127) disables the
  tier for the rest of the process. A failed compile or an object that will
  not load leaves that batch interpreted. A defun the emitter rejects is
  excluded permanently, and the rest of its batch is retried under its own
  key. The tier never leaves an error set.
- **Statistics.** `SHEN_RC_NATIVE_STATS=1` prints counters and a histogram of
  call counts on stderr at exit. `SHEN_RC_NATIVE=count` collects the
  histogram without compiling anything.

Two fixes that were already needed made the tier viable:

- `frame_code`'s peephole `sscanf` could measure the whole remaining
  function text on every line, which made emission quadratic. Emitting the
  suite's defuns took 3.9 s before the fix and 0.5 s after; the kernel C is
  byte-identical.
- `rc_symbol_table`, which runs at the entry of every generated function,
  searched a linked list. With 64 loaded modules it was the second-largest
  self-time entry; it now uses a 64-entry direct-mapped cache.

## Call distribution (step 1)

The 43.1 suite registers 454 interpreted defuns. Of these, 161 are never
called and 51 are redefined. There are 2,258,119 counted calls:

| Threshold | Defuns at or above it | Share of calls |
|---:|---:|---:|
| 1 | 293 | 100% |
| 64 | 118 | 99.96% |
| 1024 | 92 | 99.77% |
| 4096 | 72 | 97.45% |
| 16384 | 46 | 86.95% |

Threshold 1000 promotes 92 defuns in 21 batches. The cost of compiling is
driven by code size, not by the number of compiler invocations: 292 forced
defuns emit about 12.6 MB of C, one batch alone is 4.3 MB (738 functions
including lambdas), and the `cc` start-up floor is about 0.1 s.

## Measurements

Host: Darwin arm64, macOS 27.0.1, Apple clang 21.0.0. Load average was 7–8
during the six alternating rounds and 10–25 earlier, from other agents, so
**timing is UNKNOWN until requalified on a quiet host.** Base is `a2c4d7e`.
The candidate binary is built from `5145d26`
(sha256 `c13bc128…10c5`; base `48d4f115…0f95`). Raw data is in
[native-tier-measurements-20261018.json](native-tier-measurements-20261018.json),
produced by `scripts/measure-native-tier.py BASE bin/shen-c --rounds 6`.
"L load" is the in-process `clock()` time that the harness prints for
`(load "interpreter.shen")` inside the complete suite; most of it is
typechecking.

| Configuration | Median user CPU | Paired ratio vs base | Median L load | Paired ratio |
|---|---:|---:|---:|---:|
| base `a2c4d7e` | 3.82 s | — | 1.815 s | — |
| tier off | 3.81 s | 0.997 | 1.807 s | 0.996 |
| on, warm cache (92 defuns) | 3.44 s | **0.899** | 1.542 s | **0.846** |
| forced (threshold 1), warm (292) | 3.435 s | 0.901 | 1.521 s | 0.837 |

All 24 runs passed 136/0.

Cold runs, where compiler child CPU is included in user CPU:

| Configuration | User CPU | Compile (wall, in process) | Emit |
|---|---:|---:|---:|
| on, cold | 19.0 s | 15.5 s | 0.30 s |
| forced, cold | 25.95 s | 22.7 s | 0.40 s |

So at -O1 the one-time compile is about 40x the per-run saving. An earlier
loaded-host run found -O0 compiles about 3x faster, but its warm runtime was
not distinguishable from base at that noise level. With ASan, -O0 modules
also overflowed the stack (see below).

## Validation

- `scripts/test-native-tier.py bin/shen-c` PASS. Every configuration passed
  136/0 with output identical after masking only the elapsed seconds, so the
  typechecking inference counts match too. Statistics were asserted for
  each configuration:
  - off and count;
  - on, cold then warm (warm: 0 compiles, 21/21 hits);
  - forced, cold then warm (292 promoted, 0 failed; warm 64/64 hits);
  - a warm cache with truncated objects (recompiled);
  - `SHEN_RC_NATIVE_CC=false` (292 interpreted fallbacks, tier stays on);
  - a missing absolute compiler path, and `cc` with an empty `PATH` (both
    disable the tier).

  Seven focused programs give identical output with the tier off and forced,
  cold and warm: redefinition after promotion; redefinition of a registered
  cold defun and of a pending defun within one form (each asserts that the
  old registration retires, `redefined >= 1`, and that the new definition
  wins); a 1,000,000-iteration tail loop; partial application and closures;
  errors under `trap-error`; and mutual recursion. The define callback's
  identity-guard miss requires a binding change that bypasses interpreted
  `defun` between snapshot and publication, which Shen code cannot cause, so
  that path is UNKNOWN (not exercised).
- With ASan, UBSan and `SHEN_RC_VERIFY_OWNERSHIP=1` in both the CLI and the
  modules (`RC_SANITIZE=1`, modules at `-O1 -g`), the forced cold and warm
  suites both pass 136/0 with identical output. With modules at `-O0 -g`,
  ASan reported a stack overflow in deep non-tail recursion through promoted
  code, because unoptimized instrumented frames are much larger than the
  interpreter's. The CI step therefore uses `-O1 -g`.
- `make test`, `make sanitize`, `scripts/test-rc.sh --no-build bin/rc-build`
  and `scripts/certify-rc.sh` (136/0, audits PASS) all pass on Darwin. CMake
  builds and runs a promoted module.

## Remaining work

- Make the cold path cheaper. Options include compiling in the background
  and installing at a later safepoint (this hides latency but not CPU),
  capping the C size of a promoted unit, and per-function code-size
  reduction in the emitter. libtcc is another option, with a fast but weak
  compile.
- Promote at safepoints other than `eval-kl` entry. A long computation that
  never re-enters `eval-kl` stays interpreted.
- Evict stale cache entries; the cache is currently never pruned.
- Decide whether to turn the tier on by default. With the costs above, it
  should stay off unless a warm cache is expected.
- Measure tier-off overhead in `rc-build` applications, which compile the
  `tier_call` check without LTO. Unmeasured; the CLI's tier-off ratio is
  0.997.
- Requalify timing on a quiet host, on Linux, and through the existing
  phase-profile protocol. `build-phase-profile.py` instruments
  `rc-build` application C, not the CLI, so it was not used here.
