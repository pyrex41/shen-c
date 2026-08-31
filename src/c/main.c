#include <limits.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "init.h"
#include "repl.h"
#include "variable.h"
#include "version.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int streq (const char* a, const char* b)
{
  return strcmp(a, b) == 0;
}

static void print_version (void)
{
  printf("Shen-C %s\n", SHEN_C_VERSION);
}

static void print_usage (FILE* out)
{
  fprintf(out,
          "Usage: shen-c [--version] [--help] <COMMAND> [<ARGS>]\n\n"
          "commands:\n"
          "    repl\n"
          "        Launches the interactive REPL.\n"
          "        Default action if no command is supplied.\n\n"
          "    script <FILE> [<ARGS>]\n"
          "        Runs the script in FILE. *argv* is set to [FILE | ARGS].\n\n"
          "    eval <ARGS>\n"
          "        Evaluates expressions. ARGS can include:\n"
          "            -e, --eval <EXPR>\n"
          "                Evaluates EXPR and prints result.\n"
          "            -l, --load <FILE>\n"
          "                Reads and evaluates FILE via kernel load.\n"
          "            -q, --quiet\n"
          "                Sets *hush* (stdout only; file streams still write).\n");
}

static char* copy_c_string (const char* string)
{
  size_t size = strlen(string) + 1;
  char* copy = malloc(size);

  memcpy(copy, string, size);

  return copy;
}

static int directory_contains_kernel (const char* directory)
{
  char marker[PATH_MAX];
  struct stat status;
  int n = snprintf(marker, sizeof(marker), "%s/shen/src/kl/sys.kl", directory);

  if (n < 0 || (size_t)n >= sizeof(marker))
    return 0;

  return stat(marker, &status) == 0 && S_ISREG(status.st_mode);
}

static char* read_executable_path (void)
{
  char resolved[PATH_MAX];
  char* raw = NULL;

#ifdef __APPLE__
  {
    uint32_t size = PATH_MAX;
    char stack[PATH_MAX];

    if (_NSGetExecutablePath(stack, &size) == 0) {
      raw = copy_c_string(stack);
    } else {
      raw = malloc(size);

      if (is_null(raw) || _NSGetExecutablePath(raw, &size) != 0)
        return NULL;
    }
  }
#else
  {
    char stack[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", stack, sizeof(stack) - 1);

    if (n <= 0)
      return NULL;

    stack[n] = '\0';
    raw = copy_c_string(stack);
  }
#endif

  if (is_null(raw) || is_null(realpath(raw, resolved)))
    return NULL;

  return copy_c_string(resolved);
}

static char* home_from_executable (void)
{
  static const char* const suffixes[] = {
    "/..",
    "/../share/shen-c",
    "",
    "/share/shen-c"
  };
  char* executable = read_executable_path();
  char* dir_copy;
  char* directory;
  char candidate[PATH_MAX];
  char resolved[PATH_MAX];
  size_t i;

  if (is_null(executable))
    return NULL;

  dir_copy = copy_c_string(executable);
  directory = dirname(dir_copy);

  for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
    int n = snprintf(candidate, sizeof(candidate), "%s%s",
                     directory, suffixes[i]);

    if (n < 0 || (size_t)n >= sizeof(candidate))
      continue;

    if (!directory_contains_kernel(candidate))
      continue;

    if (is_not_null(realpath(candidate, resolved)))
      return copy_c_string(resolved);

    return copy_c_string(candidate);
  }

  return NULL;
}

static void fail_missing_home (void)
{
  fprintf(stderr,
          "SHEN_C_HOME is unset and the Shen-C home directory could not "
          "be derived from the executable\n"
          "Set SHEN_C_HOME to the Shen-C root directory\n"
          "ex) export SHEN_C_HOME=/home/user/shen-c\n");
  exit(EXIT_FAILURE);
}

static char* require_home (void)
{
  char* home_path = getenv("SHEN_C_HOME");

  if (is_not_null(home_path) && home_path[0] != '\0')
    return home_path;

  home_path = home_from_executable();

  if (is_null(home_path))
    fail_missing_home();

  return home_path;
}

static void boot (int argc, char** argv)
{
  shen_context_init(&shen_root_context);
  initialize_runtime(require_home());
  set_command_line_arguments(argc, argv);
  initialize_shen_kernel();
}

static int run_eval_args (int argc, char** argv, int start)
{
  int i = start;
  int saw_work = 0;

  while (i < argc) {
    if (streq(argv[i], "-e") || streq(argv[i], "--eval")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "eval requires an expression\n");
        return 1;
      }

      eval_print_expression(argv[i + 1]);
      saw_work = 1;
      i += 2;
    } else if (streq(argv[i], "-l") || streq(argv[i], "--load")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "eval -l requires a file\n");
        return 1;
      }

      eval_load_file(argv[i + 1]);
      saw_work = 1;
      i += 2;
    } else if (streq(argv[i], "-q") || streq(argv[i], "--quiet")) {
      set_shen_hush(1);
      i += 1;
    } else {
      fprintf(stderr, "Invalid eval argument: %s\n", argv[i]);
      return 1;
    }
  }

  if (!saw_work) {
    fprintf(stderr, "eval requires -e <EXPR> or -l <FILE>\n");
    return 1;
  }

  return 0;
}

int main (int argc, char** argv)
{
  if (argc == 2 && (streq(argv[1], "--version") || streq(argv[1], "-v"))) {
    print_version();
    return 0;
  }

  if (argc >= 2 && (streq(argv[1], "--help") || streq(argv[1], "-h"))) {
    print_usage(stdout);
    return 0;
  }

  if (argc == 1 || (argc >= 2 && streq(argv[1], "repl"))) {
    boot(argc, argv);
    run_shen_repl();
    return 0;
  }

  if (streq(argv[1], "script")) {
    if (argc < 3) {
      fprintf(stderr, "script requires a FILE\n");
      print_usage(stderr);
      return 1;
    }

    boot(argc, argv);
    run_script(argv[2], argc - 3, argv + 3);
    return 0;
  }

  if (streq(argv[1], "eval")) {
    boot(argc, argv);
    return run_eval_args(argc, argv, 2);
  }

  if (streq(argv[1], "-e") || streq(argv[1], "--eval")) {
    boot(argc, argv);
    return run_eval_args(argc, argv, 1);
  }

  fprintf(stderr,
          "ERROR: Invalid argument: %s\n"
          "Try `shen-c --help' for more information.\n",
          argv[1]);
  return 1;
}
