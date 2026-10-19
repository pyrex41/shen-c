# Compile-first sanitizer completion, 2026-10-19

The 16-module kernel and all dynamic code passed the pinned 43.1 suite (136/0)
under fatal ASan/UBSan and SHEN_RC_VERIFY_OWNERSHIP. The packed run recorded zero
compiler calls and zero interpreter entries, then exited cleanly. Source and
binary hashes remained unchanged; see [receipt](result.json).

An earlier cold run completed 136/0 but failed thread teardown: the sanitizer
attempted to unmap the CLI's static alternate signal-stack buffer. That run
remains FAIL at local compile-first-bundle/evidence/rc/*192440Z. Commit 1096423
restores previous signal handlers and alternate stack before returning from the
worker, and uses the actual frame address when deriving the stack limit.
An 8 MiB stack control caught runaway recursion as an ordinary Shen error.

The successful executable rebuilds main with -O1 -g -fsanitize=address,undefined
-fno-sanitize-recover=all -fno-omit-frame-pointer and the ownership verifier.
Unchanged instrumented core, generated-kernel and tier objects are reused from
the preceding full instrumented build; this change only affects main.c. The
canonical module sources are rebuilt into a packed image with the same policy
flags. Full source/compiled artifact hashes and generated module hashes are in
the receipt. No release-performance result is inferred from sanitizer timings.
