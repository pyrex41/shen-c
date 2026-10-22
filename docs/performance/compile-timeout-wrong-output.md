# A load-time compile timeout aborts the program (main 8c94154)

Status: reproduced on main 8c94154, the baseline binary
(`perf/tight-perceus` measurement copy `main8c94154`). No issue has been filed.

## Summary

When a load-time native compile takes longer than `SHEN_RC_COMPILE_TIMEOUT`
(default 120 s), the definition being loaded fails. The program stops with
exit status 1 and prints one diagnostic line to stderr. Its stdout stops at
that point, so the workload's result line is missing.

This is **not silent wrong output**. The earlier progress note was wrong to
call it that. The failure is loud: a non-zero exit plus a stderr diagnostic.
The real problem is that a slow C compiler, for example on a loaded host, turns
a correct program into a failed run. The interpreter is never used as a
fallback, which compile-first deliberately does not do (`native mode never
falls back to interpretation`, CHANGELOG 0.4.0).

During the warm-up runs on 2026-10-20, at load average 30–60 with the
default 120 s timeout, b-typed and d-g5 on main produced a payload that did not
match the go-1f587de reference. That harness did not record the exit codes or
stderr of warm-up runs, so whether those runs exited 1 is UNKNOWN. The forced
repro below shows the mechanism.

## Reproduction

```sh
cd shenbench/workloads/d-g5/00-run-093w2n
SHEN_RC_NATIVE_CACHE=$(mktemp -d)/cache SHEN_RC_COMPILE_TIMEOUT=1 SHEN_RC_NATIVE_STATS=1 \
  shen-c script driver.shen > timeout.out 2> timeout.err; echo $?        # 1
SHEN_RC_NATIVE_CACHE=$(mktemp -d)/cache SHEN_RC_COMPILE_TIMEOUT=1200 \
  shen-c script driver.shen > ok.out 2> ok.err; echo $?                  # 0
diff ok.out timeout.out
```

- An empty cache is required. A warm cache skips the compiler.
- `SHEN_RC_COMPILE_TIMEOUT=1` forces the timeout on an idle machine.

stderr contains:

    shen-rc: native compilation failed (status 124); diagnostics: <cache>/<key>.log
    RC_NATIVE ... compiles=10 ... failures=2 ...

stdout is identical except for timing lines, up to the failing load. It then
lacks the result line, which is the whole payload for this driver:

    < CAPCOV-OPERATION-RESULT [[[["job"],["job"],[]],["result",[["row","job","j",...

## Code path

- `src/rc/native_compile.c:128-136` (`run_compiler`): when the deadline
  passes, it kills the compiler's process group and returns 124.
- `src/rc/native_compile.c:347` (`compile_execute`): a non-zero status
  becomes `rc_error("native compilation failed (status %d) ...")`.
  `compile_execute` returns NULL and counts `failures++` (line 352).
- That error unwinds like any Shen error to the CLI's top level, which exits 1.
  Code that runs the failing form under `trap-error` would see an ordinary Shen
  error instead.

Options for the user to decide (nothing is changed on this branch):
- a more generous default, or no default timeout;
- retrying once without a timeout;
- an explicit, documented interpreter fallback for timeouts only;
- or keep the error and document `SHEN_RC_COMPILE_TIMEOUT` for loaded hosts.
