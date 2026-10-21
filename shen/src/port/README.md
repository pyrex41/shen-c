# Shen-C port boot module

`shen-c-boot.kl` is the only kernel input that is not part of the pinned
ShenOSKernel release. The community kernel files are definitions only; this
module runs after all of them are installed and performs the initialisation
the release expects from a port:

- `shen.initialise` (from `init.kl`) builds the property dictionary, arity
  table, lambda forms and type signatures, and sets `*version*`;
- the bundled extensions that have initialisers are initialised:
  features (with `shen/c` as the port's feature symbol), programmable
  pattern matching and namespaces, as Shen/Scheme does, plus
  `shen.x.expand-dynamic.initialise`, which Shen/Scheme omits but
  `doc/extensions/expand-dynamic.md` says must run before that extension's
  functions are used (it only sets `shen.x.expand-dynamic.*arities*` to
  `()`). `extension-launcher` and `extension-type-annotations` need no
  initialiser;
- `stlib.initialise` installs the standard library.

The native runtime sets `*language*`, `*implementation*`, `*port*`,
`*porters*`, `*os*`, the standard streams and `*home-directory*` before the
kernel is installed (`src/rc/primitives.c`).
