# Exact sixth-wave diagnostic

Immutable CLI `aa8ee0ced2225a2b0c04b070e12092baa937879a822d856f957b6ef4fe87a3ce`, source freeze3b055330. FullS42 completed exit0 with final134 passed/0 failed and every intermediate failure count0. Binary and complete suite hashes unchanged. This sampled run is diagnostic, not a timing result.

Three complete one-second samples at10ms intervals started offsets1.028/5.040/9.040 seconds; sample exited0 each. Raw captures5.92/24.44/20.44MB all below64MiB; gzip hashes and exact commands retained in result.json.

| Window | Context at start | Thread samples | Reported leading flat samples |
|---|---|---:|---|
| 1 | Einstein riddle result german, moving through loads/typechecks | 89 | equality8, evaluate6, apply_request5 |
| 2 | L interpreter load and function/type declarations | 90 | evaluate17, apply_request11, application7, apply_small5 |
| 3 | L interpreter function/type declarations | 90 | evaluate17, rc_env_frame7, apply_small6, edges6, apply_request5 |

The sample flat report omits symbols below5; these values are lower bounds when aggregated. evaluate40/269 thread samples (~14.9%) remains the leading visible interpreter cost; apply_request21 (~7.8%) follows. apply_small11, application7, rc_env_frame7 and equality8 remain concrete targets. These windows have different phase coverage than fifthwave, so counts do not establish a before/after speedup. Assoc is absent among reported flat leaders after the guarded helper landed.

Mapped inclusive stacks early pass load/map-h/eval-and-print into declare/variancy, system-S/search-user-datatypes and bind/cut. Mid/late windows spend more time in interpreted evaluation/application during interpreter source load/typecheck. Native callback/body mappings to installed Shen defuns and anonymous enclosing defuns are in mapped-profile.json. Inclusive maxima are per stack occurrence, not total costs; recursive occurrences repeat, and maxima must not be summed or compared directly with aggregated flat counts.

Next decisions: inspect evaluate expression classification/environment lookup and apply_request argument/tail dispatch; profile rc_env_frame allocation/retains specifically before further environment changes. The late edges6 samples justify inspecting edge traversal callers, but do not alone establish cycle collection as dominant. No collector overhaul is supported by this sparse sample. Equality remains an early-phase target; more assoc work is not suggested by these sampled flat leaders.
