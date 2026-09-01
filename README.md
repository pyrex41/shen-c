# Shen-C

A [Shen](https://shenlanguage.org/) port in C17. This repository
([`pyrex41/shen-c`](https://github.com/pyrex41/shen-c), default branch
`main`) is a fork of [otabat/shen-c](https://github.com/otabat/shen-c).
It boots Mark Tarver's **Shen S42** kernel (`*version*` is `"42"`) and
passes the upstream suite **passed 134 / failed 0** both as
`bin/shen-c` (`make certify`) and as a Yggdrasil-generated AOT app
(`bin/runme-aot-app`).

Shen is a portable functional language by [Mark Tarver](https://marktarver.com/)
with pattern matching, macros, optional sequent-calculus types, Prolog, and
Shen-YACC.

**Runtime.** Boehm GC (`bdw-gc`) via Nix `pkg-config` (Homebrew paths are
rejected). Values are tagged words: fixnum / bool / empty-list are non-pointer
immediates; cons, symbol, and string pointers use
`GC_register_displacement`. After a KL boot, `bin/shen-c` overwrites kernel
defuns with generated `NativeFunction`s (rust-style `install_all`; 686 names)
and re-applies C port overwrites. Loaded test files still typecheck as boxed
KL through AOT `t*`. Tails hop on a 16 MiB mmap trampoline (Darwin Boehm
workers). Overlay-after-load is wired in `abi.c` / `emit.c` and **unplugged**
for runme (`overlay_wrap=0`).

**Bars (do not regress).**

| Bar | How | Status |
|-----|-----|--------|
| Interpreter certify | `nix develop -c make CC=clang certify` | **134 / 0**, Shen **6.18 s** |
| AOT runme | `yes \| bin/runme-aot-app/app` from `shen/test/s42` | **134 / 0**, **5.72 s** real |
| Same suite, shen-go | `script runme.shen` | **134 / 0**, **4.84 s** real |

L interpreter typecheck is **4.244 s** / 1 177 672 inferences (Go **2.141 s** /
1 178 236). Cold AOT is **~1.18×** Go wall, not a match. Warm
`SHEN_C_TC_CACHE` can match Go on L-interp; it is off by default. Details:
`docs/AOT-GAP.md`.

Tested on macOS (Apple Silicon) with Clang. Ubuntu (x86_64 / AArch64) is a
minor supported OS. Upstream also ships an
[iOS App Store build](https://chatolab.wordpress.com/2017/07/10/shen-programming-language-for-ios/)
and [Shen for Android](https://chatolab.wordpress.com/2017/12/26/shen-programming-language-for-android/)
(Tatsuya Tsuda / otabat), plus [Shen-JVM](https://github.com/otabat/shen-jvm).

## Installation

Pre-S42 release tarballs are still on
[otabat/shen-c releases](https://github.com/otabat/shen-c/releases). S42 work
is this fork: clone and build from source (Nix).

```
git clone https://github.com/pyrex41/shen-c.git
cd shen-c
nix develop -c make CC=clang
```

Upstream tarball (older kernel):

```
tar xvf shen-c-{VERSION}-{OS}-{ARCH}.tar.gz
```

## Usage

```
shen-c --version
shen-c                 # REPL (stdin EOF exits)
shen-c eval -e EXPR
shen-c script FILE
```

Quit the REPL with `(exit 0)` or stdin EOF. `(value *version*)` is `"42"`.

## Build from source

C17. `bdw-gc` must come from the Nix dev shell. KL graphs go through
`shen_context` / `GC_malloc`.

```
nix develop -c make
nix develop -c make test
nix develop -c make certify
nix develop -c cmake -G Ninja -B build && nix develop -c cmake --build build
```

`make certify` runs Tarver S42 `shen/test/s42/runme.shen` on `bin/shen-c` and
writes `evidence/certify.log`. Certified means that log contains
`passed ... 134` and `failed ... 0`.

AOT of the same suite (shaken kernel + runtime load of test sidecars):

```
nix develop -c make CC=clang bin/runme-aot-app
cd shen/test/s42 && yes | /usr/bin/time -l ../../bin/runme-aot-app/app
```

Yggdrasil C (rung 1) includes `src/c/abi.h`, links `bin/libshenc.a`, and
registers shaken `defun`s as `NativeFunction`s via `shen_register_defun`.
`eval-kl` stays for needs-eval shakes. `bin/yggdrasil-build` reads a shake dir
(`kernel.kl` + user `.kl` + `yggdrasil.manifest.txt`) and writes `app.c`
(one C function per shaken `defun`), `Makefile`, and `CMakeLists.txt`.
Generated `main` is `shen_boot` (never `load_kl_file` of `shen/src/kl/*.kl`),
install NativeFunctions, re-apply port overwrites, `(shen.initialise)` if the
manifest names it, then user toplevels.

`make test` builds `bin/test_abi`, `bin/test_emit`, `test/fixtures/sum`
(kernel `id` + user `sum3` prints 42), `test/fixtures/fib-small` (self-tail
`goto`, other tails `shen_apply`, prints 6765), Yggdrasil shakes
`hello-ygg` / `fib-ygg` (`hello from shaken shen`, `fib 20 = 6765`), and
`tc-ygg` (needs-eval slice: 568 kernel NativeFunctions including `types.kl` /
`t-star`; declare tables print `inferences = 2778`). First-class AOT lambdas
go through `shen_native_closure` / `shen_apply` (macroexpand / `map` / wrap).
`declare` still uses `eval-kl` for signature thunks. Rung 2 is documentation
only: `docs/option5-rung2.md` and `docs/rung2.md`.

Shake a Shen program (stage 1 is the Yggdrasil host, not this interpreter):

```
# from the yggdrasil checkout; reference host shen-cl, fallback shen-go
yggdrasil shake tests/fib.shen out/
# or:
../shen-cl/bin/sbcl/shen eval -q -l yggdrasil.shen -e '(yggdrasil.shake ["tests/fib.shen"] "out")'
../shen-go/bin/shen eval -q -l yggdrasil.shen -e '(yggdrasil.shake ["tests/fib.shen"] "out")'
```

`shen-c eval -l` / `-e` exist (`*hush*` is stdout-only) but `yggdrasil.shake`
on shen-c is unverified. Stage-1 host is shen-cl (fallback shen-go). Stage-2
is C.

```
nix develop -c make CC=clang bin/libshenc.a bin/yggdrasil-build
# from the yggdrasil checkout:
yggdrasil shake tests/fib.shen out/
SHEN_C_HOME=/path/to/shen-c nix develop /path/to/shen-c -c \
  ./bin/yggdrasil-build out out/app-c
./out/app-c/app
# or: yggdrasil build tests/fib.shen out/ --target c
# expected: fib 20 = 6765

# typecheck-heavy kernel slice (needs-eval; stage-1 host shen-cl/shen-go):
yggdrasil shake tests/tc-interp.shen out-tc/
SHEN_C_HOME=/path/to/shen-c nix develop /path/to/shen-c -c \
  ./bin/yggdrasil-build out-tc out-tc/app-c
# run from the dir that contains interpreter.shen:
(cd tests && ../out-tc/app-c/app)
# kernel declare tables print inferences = 2778; loading interpreter.shen
# typechecks through AOT t* (full runme is bin/runme-aot-app, 134/0).
```

AddressSanitizer and UndefinedBehaviorSanitizer:

```
nix develop -c make SANITIZE=1 test
# Linux: ASan+UBSan. Darwin: UBSan only (ASan deadlocks Boehm GC_init).
```

A binary named `shen-c` is written to `bin/`.

```
./bin/shen-c --version
./bin/shen-c
./bin/shen-c eval -e '(+ 1 1)'
./bin/shen-c script FILE
```

Stdin EOF exits the REPL.

To build a release archive:

```
nix develop -c make release
```


## Run non-release build

There are several ways to run a non-release build.

* Run the shell script under the project root.
```
./shen-c
```

* Run Shen-C REPL by using the make command
```
make repl
```
or if rlwrap is installed
```
make rrepl
```

* Run Shen-C REPL from a compiled binary

`bin/shen-c` finds the repo tree from its own path (`home_from_executable()`
looks for `shen/src/kl/sys.kl`). `$SHEN_C_HOME` is an optional override, not
required for Bifrost or for a binary that still sits in `bin/`.

```
./bin/shen-c
env -u SHEN_C_HOME ./bin/shen-c eval -e '(version)'
```

To force a tree (or if the binary was copied away from the repo):

```
export SHEN_C_HOME=/path/to/shen-c
alias shen-c='$SHEN_C_HOME/bin/shen-c'
```
or if rlwrap is installed
```
alias shen-c='rlwrap $SHEN_C_HOME/bin/shen-c'
```
and then
```
shen-c
```


## Learn Shen
* [Official website of Shen](http://shenlanguage.org/)
* [The Shen OS Kernel Manual](http://shenlanguage.org/learn-shen/index.html)
* [The Official Shen Standard](http://www.shenlanguage.org/learn-shen/shendoc.htm)
* [Shen Community Wiki](https://github.com/Shen-Language/wiki/wiki)
* [The Book of Shen: third edition](https://www.amazon.co.uk/Book-Shen-Third-Mark-Tarver/dp/1784562130)


## License

#### Shen
Copyright (c) 2010-2022, Mark Tarver  
Shen is released under the [BSD License](shen/LICENSE.txt).

#### Shen-C
Copyright (c) 2022, Tatsuya Tsuda; S42 / AOT work on this fork.
Shen-C is released under the [MIT License](http://www.opensource.org/licenses/MIT).
