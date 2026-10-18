# Shen-C 0.3.0 release plan

1. Integrate main's ancestry in the isolated `release/v0.3.0` worktree, retaining
   the complete qualified 43.1 tree from `e853f4a`. Main's older 42.2 kernel and
   Boehm build changes are superseded; all history remains reachable.
2. Update active CLI/Nix version metadata to 0.3.0, current README and changelog.
   Preserve historical Boehm sources and immutable qualification receipts.
3. Push a reviewable pull request to main. Require all four existing CI jobs on
   the exact release candidate, including full generated-kernel sanitizers,
   compiler/platform checks and the complete unchanged 136-test suite.
4. Merge the passing candidate into main without rewriting either history.
   Verify main contains the qualified checkpoint and the release changes.
5. Tag main as `v0.3.0`, publish a GitHub source release with the changelog,
   source archive and checksums, and verify tag, main and release identities.

The measured implementation remains `8e01579`; release version metadata is
distinct from the frozen performance artifact identity. CI on the integrated
candidate is required before publication. Host-specific timing evidence is not
a cross-platform performance claim.
