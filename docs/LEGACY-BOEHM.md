# Historical Boehm backend

The previous implementation remains in `src/c` as historical source. Supported
Make, CMake, Nix, and CI entrypoints use the independent RC toolchain. The old
Boehm build configuration is available in Git history at `97aa8e1`; it is no
longer an optional dependency of the current build.

The pre-RC history contains the tagged-word runtime, mmap tail trampoline,
Yggdrasil/Bifrost overlays, typecheck cache, and native dictionary extensions.
Those extension interfaces belong to the legacy implementation.

## Historical measurements

The former README reported S42 134/0 for the legacy CLI and Yggdrasil AOT
application. It recorded 6.18 seconds for the CLI, 5.72 seconds for the AOT
application, and 4.84 seconds for Shen-Go. These are historical measurements,
not measurements of the current reference-counted backend or a controlled
comparison with it. See [AOT-GAP.md](AOT-GAP.md) and commit `97aa8e1`'s README
for the original context and commands.

The old region-only proposal in [rung2.md](rung2.md) is also distinct from the
current reference-counting compiler and exact cycle-reclamation runtime.
