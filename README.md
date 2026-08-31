# Shen-C

Shen-C is a C port of the [Shen](http://shenlanguage.org/) programming language.  

Shen is a portable functional programming language developed by [Mark Tarver](http://marktarver.com/) that offers
* Pattern matching
* Lambda calculus consistency
* Macros for defining domain specific languages
* Optional lazy evaluation
* Optional static type checking based on [Sequent calculus](https://en.wikipedia.org/wiki/Sequent_calculus)
* An integrated fully functional Prolog
* An inbuilt compiler-compiler, Shen-YACC

Shen-C is implemented as an interpreter, mainly tested on macOS (Apple Silicon) using Clang.

Besides macOS (Apple Silicon), Ubuntu 20.04 LTS (x86_64/AArch64) is a minor supported OS.

The [iOS version of Shen-C](https://chatolab.wordpress.com/2017/07/10/shen-programming-language-for-ios/) is available on the App Store, which is a full featured Shen REPL with a customized keyboard for both iPhone and iPad.

Other ports of Shen by the Shen-C author includes
* [Shen-JVM](https://github.com/otabat/shen-jvm)
* [Shen for Android](https://chatolab.wordpress.com/2017/12/26/shen-programming-language-for-android/), which is a full featured Shen REPL with a customized keyboard for Android on Google Play


## Installation
1. Download a release build from [releases](https://github.com/otabat/shen-c/releases)
2. Unarchive a release build
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

Quit the REPL with `(exit 0)` or stdin EOF.


## Build from source

Shen-C is C17. Boehm GC (`bdw-gc`) is resolved with pkg-config from the Nix
dev shell. Homebrew include paths are not used. KLObject graphs are allocated
through `shen_context` (`GC_malloc`); they are not libc `malloc`/`free` or
refcounted. Trampoline frame chains are a later stage.

```
nix develop -c make
nix develop -c make test
nix develop -c make certify
nix develop -c cmake -G Ninja -B build && nix develop -c cmake --build build
```

`make certify` runs Mark Tarver's S42 `shen/test/s42/runme.shen` and writes
`evidence/certify.log`. Certified means that log contains `passed ... 134` and
`failed ... 0`. Shen-C is a C17 tree-walker on S42, not an AOT compiler.

Generated C (option 5 rung 1) includes `src/c/abi.h`, links `bin/libshenc.a`,
and calls intern/cons/hd/tl/apply on `shen_context` / Boehm. Defuns register as
`NativeFunction`s via `shen_register_defun`. `eval-kl` stays for needs-eval
shakes. `bin/yggdrasil-build` reads a Yggdrasil shake dir (`kernel.kl` + user
`.kl` + `yggdrasil.manifest.txt`) and writes a project: `app.c` (one C
function per shaken `defun`, not `eval_kl_object` of the source string),
`Makefile`, and `CMakeLists.txt`, then `make`s the executable. Generated
`main` is `shen_boot` (runtime + C overwrites only; never `load_kl_file` of
`shen/src/kl/*.kl`), install NativeFunctions, re-apply port overwrites,
`(shen.initialise)` if the manifest names it, then user toplevels.
`make test` builds and runs `bin/test_abi`, `bin/test_emit`, the
`test/fixtures/sum` app (kernel `id` + user `sum3` prints 42),
`test/fixtures/fib-small` (self-tail `goto`, other tails `shen_apply`,
prints 6765), real Yggdrasil shakes `test/fixtures/hello-ygg` /
`test/fixtures/fib-ygg` (`hello from shaken shen`, `fib 20 = 6765`),
and `test/fixtures/tc-ygg` (needs-eval slice: 568 kernel NativeFunctions
including `types.kl` / `t-star`; `shen.initialise` declare tables run as C;
prints `inferences = 2778`). `load interpreter.shen` is still trapped
(`macroexpand`/`walk` apply). `declare` still uses `eval-kl` for signature
thunks. Rung 2 is documentation only: `docs/option5-rung2.md`
(proof list) and the short pointer `docs/rung2.md`.

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
# expected: inferences = 2778 (kernel declare tables). load of
# interpreter.shen currently traps in macroexpand/walk.
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
Shen is released under the [BSD License](https://github.com/otabat/shen-c/tree/master/shen/LICENSE.txt).  

#### Shen-C
Copyright (c) 2022, Tatsuya Tsuda
Shen-C is released under the [MIT License](http://www.opensource.org/licenses/MIT).
