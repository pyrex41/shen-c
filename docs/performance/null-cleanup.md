# Remove drops of slots proven NULL

After logical-to-physical frame rewriting, a conservative generated-code pass
tracks physical slots proven NULL. Exact cleanup statements are omitted only
when their slot is known NULL; range drops are omitted only when every covered
slot is known NULL. Retained cleanup establishes NULL facts. Simple assignments
invalidate their destination; exact moves transfer facts. Unknown lines reset
all facts, including labels, braces, compound address-taking reuse calls and
control-flow joins. Generated checks and argument copies preserve facts.

The frame array remains private. Ordinary callbacks receive values and scratch
argument storage, not the frame array. Address-taking compound forms clear facts.
The existing string-aware rewrite and exact full-line matches preserve user
strings resembling cleanup syntax. The range helper is emitted only when an actual retained range call needs it.

## Evidence

Emitting the unchanged KL kernel (686 defuns, 291 lambdas, 198 top-level forms)
with baseline and candidate hosts produced:

| Metric | Baseline | Candidate |
| --- | ---: | ---: |
| Generated C bytes | 8,084,361 | 6,818,972 |
| Conditional release sites | 65,231 | 36,686 |
| Range drop calls | 5,582 | 2,518 |

This is code-size evidence, not a runtime performance claim.
No full kernel C compilation or suite benchmark was run in this lane.

Fresh fatal ASan/UBSan validation passed 72 semantic cases, 20 ownership cases,
large-frame recursion/captures/traps/tail calls and source metadata in both
ownership modes. `test_null_cleanup.sh` additionally passed both ownership modes
with branches, traps, pair reuse, recycled slots and strings containing fake
cleanup syntax and an embedded newline. Local logs are in
`evidence/rc/null-cleanup/`.
