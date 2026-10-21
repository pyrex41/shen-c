# Shen-C port boot module

`shen-c-boot.kl` is the only kernel input that is not part of the pinned
ShenOSKernel release. The community kernel files are definitions only; this
module runs after all of them are installed and performs the initialisation
the release expects from a port:

- `shen.initialise` (from `init.kl`) builds the property dictionary, arity
  table, lambda forms and type signatures, and sets `*version*`;
- the bundled extensions are initialised as Shen/Scheme does, with
  `shen/c` as the port's feature symbol (`extension-launcher` and
  `extension-type-annotations` need no initialiser);
- `stlib.initialise` installs the standard library.

The native runtime sets `*language*`, `*implementation*`, `*port*`,
`*porters*`, `*os*`, the standard streams and `*home-directory*` before the
kernel is installed (`src/rc/primitives.c`).
