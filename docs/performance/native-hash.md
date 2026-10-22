# Native hash (Track A1)

`shen.dict` places every property through `(hash Key Capacity)`. The kernel
computes it by:

1. printing the key with `shen.app`;
2. exploding the string into unit strings;
3. mapping `string->n` over them;
4. folding with `shen.prodbutzero`;
5. reducing with `shen.mod`'s repeated doubling.

That path was 19–28% of b-typed's profile (`whyfast/REPORT.md`).
`src/rc/primitives.c` replaces `hash` with a bit-identical C port.

## Exactness

- **Inputs.** The port handles symbols, strings and exact integers, with a
  positive exact-integer modulus. It uses the same bytes the kernel prints:
  a symbol's name, the string itself, or the integer's decimal digits.
  `shen.fail!` prints as `...`.
- **Fold.** Zero bytes are skipped. The product is multiplied while it is at
  most 10000000000, then bytes are added. The result is taken modulo the
  capacity, with 0 mapped to 1.
- **Fallback.** Every other value runs the original kernel closure. That
  covers lists, vectors, floats (which go through the numeric
  `shen.hashkey` wrapper), closures, streams, and any other modulus.
- **Install.** The emitter installs the port only when the kernel module
  defines all 20 ported definitions with exactly the 43.1 text, up to the
  renaming of bound variables, and `shen.hashkey` has its certified shape.
- **Redefinition.** The 37 functions the kernel path reaches are captured at
  install. After any function publication they are checked again, using a
  per-context certificate keyed by the function generation. If any of them
  changed, `hash` runs the kernel closure.
- **Tests.** `test/rc/test_kernel_overrides.py` compares native `hash`
  with the kernel's own body on 63 keys and 10 moduli. It also redefines
  `shen.prodbutzero`, `string->n` and `shen.mod` and checks that `hash`
  observes each change.

## Measurements

Instructions retired, from `/usr/bin/time -l`: main 112de06 against this
branch (6e5f3cd). Two interleaved rounds were run with load-time compile off
(`SHEN_RC_EXECUTION=interpret`, `SHEN_RC_NATIVE_CACHE=off`), and the table
shows their mean. In that mode the kernel and the runtime are the
ahead-of-time compiled code being measured.
`big` is cvs's `btyped-x10`. Every run produced the reference payload or
output.

| Workload | main 112de06 instructions | A1 instructions | Change | main 112de06 peak RSS | A1 peak RSS |
|---|---|---|---|---|---|
| suite | 102.52G | 98.14G | -4.3% | 20 MB | 20 MB |
| mb-terms | 209.22G | 208.68G | -0.3% | 11 MB | 11 MB |
| mb-defs | 273.51G | 256.45G | -6.2% | 24 MB | 24 MB |
| a2-libs-typed | 118.80G | 111.80G | -5.9% | 28 MB | 28 MB |
| b-typed | 930.33G | 689.97G | -25.8% | 446 MB | 456 MB |
| c-untyped | 885.52G | 161.81G | -81.7% | 31 MB | 32 MB |
| d-g5 | 435.22G | 383.63G | -11.9% | 41 MB | 43 MB |
| big | 9303.72G | 6903.91G | -25.8% | 514 MB | 523 MB |

200000 hashes of a symbol take 58.8G instructions before and 1.50G after.

The generated kernel C grows by one line (49 bytes). Single-job clang `-O3`
compile time of `kernel.o` is unchanged within noise: 62.2 s user on main and
56.1 s on this branch, on a loaded host.
