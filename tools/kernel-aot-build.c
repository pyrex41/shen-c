#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "emit.h"

/*
 * Always-AOT kernel codegen (rust install_all analog).
 *   kernel-aot-build <kl-dir> <out-dir>
 *
 * Emits one C TU per boot KL file (defuns only) plus install_all.c.
 * Does not read interpreter.shen / prologinterp.shen / backend.kl.
 */

static const char* const KERNEL_FILES[] = {
  "sys.kl",
  "writer.kl",
  "core.kl",
  "reader.kl",
  "declarations.kl",
  "toplevel.kl",
  "macros.kl",
  "load.kl",
  "prolog.kl",
  "sequent.kl",
  "track.kl",
  "t-star.kl",
  "yacc.kl",
  "types.kl",
  NULL
};

static void ident_from_kl (const char* filename, char* out, size_t cap)
{
  size_t n = 0;
  const char* p;

  if (cap == 0)
    return;

  for (p = filename; *p != '\0' && *p != '.' && n + 1 < cap; ++p) {
    char c = *p;

    if (c == '-')
      c = '_';

    out[n++] = c;
  }

  out[n] = '\0';
}

static int mkdir_p (const char* path)
{
  char buf[4096];
  size_t n;
  size_t i;
  struct stat status;

  n = strlen(path);

  if (n == 0 || n >= sizeof(buf))
    return -1;

  memcpy(buf, path, n + 1);

  for (i = 1; i < n; ++i) {
    if (buf[i] == '/') {
      buf[i] = '\0';

      if (buf[0] != '\0' && mkdir(buf, 0755) != 0 && errno != EEXIST)
        return -1;

      buf[i] = '/';
    }
  }

  if (stat(path, &status) == 0) {
    if (S_ISDIR(status.st_mode))
      return 0;

    if (unlink(path) != 0)
      return -1;
  }

  if (mkdir(path, 0755) != 0 && errno != EEXIST)
    return -1;

  return 0;
}

int main (int argc, char** argv)
{
  const char* kl_dir;
  const char* out_dir;
  const char* home;
  char home_abs[4096];
  char install_path[4096];
  FILE* install_out;
  int i;
  long total_defuns = 0;

  if (argc != 3) {
    fprintf(stderr, "Usage: kernel-aot-build <kl-dir> <out-dir>\n");
    return 2;
  }

  kl_dir = argv[1];
  out_dir = argv[2];
  home = getenv("SHEN_C_HOME");

  if (home == NULL || home[0] == '\0')
    home = ".";

  if (realpath(home, home_abs) != NULL)
    home = home_abs;

  if (mkdir_p(out_dir) != 0) {
    fprintf(stderr, "kernel-aot-build: cannot mkdir %s\n", out_dir);
    return 1;
  }

  shen_boot(&shen_root_context, home);

  snprintf(install_path, sizeof(install_path), "%s/install_all.c", out_dir);
  install_out = fopen(install_path, "w");

  if (install_out == NULL) {
    fprintf(stderr, "kernel-aot-build: cannot write %s\n", install_path);
    return 1;
  }

  fprintf(install_out,
          "/* Generated always-AOT kernel install_all (rust analog).\n"
          " * Call after load_shen_kl_files. Not overlay-after-load.\n"
          " * Sidecar tests stay on the tree-walker.\n"
          " */\n"
          "#include \"abi.h\"\n\n");

  for (i = 0; KERNEL_FILES[i] != NULL; ++i) {
    char ident[64];

    ident_from_kl(KERNEL_FILES[i], ident, sizeof(ident));
    fprintf(install_out,
            "void shen_kernel_aot_install_%s (shen_context* ctx);\n", ident);
  }

  fprintf(install_out,
          "\nvoid shen_kernel_aot_install_all (void)\n{\n"
          "  shen_context* ctx = &shen_root_context;\n");

  for (i = 0; KERNEL_FILES[i] != NULL; ++i) {
    char ident[64];
    char kl_path[4096];
    char c_path[4096];
    KLObject** forms = NULL;
    long n = 0;
    FILE* out;
    ShenEmitReport report = {0};

    ident_from_kl(KERNEL_FILES[i], ident, sizeof(ident));
    snprintf(kl_path, sizeof(kl_path), "%s/%s", kl_dir, KERNEL_FILES[i]);
    snprintf(c_path, sizeof(c_path), "%s/%s.c", out_dir, ident);

    if (shen_read_kl_path(kl_path, &forms, &n) != 0) {
      fprintf(stderr, "kernel-aot-build: cannot read %s\n", kl_path);
      fclose(install_out);
      return 1;
    }

    out = fopen(c_path, "w");

    if (out == NULL) {
      fprintf(stderr, "kernel-aot-build: cannot write %s\n", c_path);
      fclose(install_out);
      return 1;
    }

    if (shen_emit_kernel_module(out, forms, n, ident, KERNEL_FILES[i],
                                &report) != 0) {
      fprintf(stderr, "kernel-aot-build: emit failed for %s\n", kl_path);
      fclose(out);
      fclose(install_out);
      return 1;
    }

    fclose(out);
    total_defuns += report.defuns;
    fprintf(install_out, "  shen_kernel_aot_install_%s(ctx);\n", ident);
    fprintf(stderr, "kernel-aot-build: %s -> %s (%ld defuns)\n",
            KERNEL_FILES[i], c_path, report.defuns);
  }

  fprintf(install_out, "}\n");
  fclose(install_out);
  fprintf(stderr, "kernel-aot-build: install_all %ld defuns\n", total_defuns);

  return 0;
}
