# source_patches — the four Phase B commits

Branch `paper/edbt_v4`, base `0f182ad07` (pre-Phase-B tip), HEAD
`fab61b2fee237323833be3fa333054ce669fbff5`.

| order | commit | patch | content |
|---|---|---|---|
| ① | `799a668da` | `0001-…B1-observation-only…` | `--placement_trace` decision/operator trace (default off) |
| ② | `ff5554039` | `0002-…B2-shared-deterministic-strict-fit…` | one strict-fit mapper for all migration policies |
| ③ | `fa3c61172` | `0003-…B3-cost_c3_greedy…` | C3 priority provider + calibration loader + BASELINE line |
| ④ | `fab61b2fe` | `0004-…B4-replayable-query-submission…` | `--schedule_seed` + plan/manifest sidecars |

## Reproducibility proof (executed 2026-09-14)

```
git worktree add /tmp/patch_verify_wt 0f182ad07
cd /tmp/patch_verify_wt
git apply 0001*.patch && git apply 0002*.patch && git apply 0003*.patch && git apply 0004*.patch
git add -A && git diff --cached fab61b2fe | wc -l   # -> 0
```

Applying the four patches **in order** onto the base commit reproduces HEAD
exactly (0 diff lines). Patches are chained: 0002/0003 do not apply to the
pristine base without 0001.

Smoke-run binaries correspond to these commits:
`build_logs/final_binaries_B4.sha256` (binary `a3a2844d…`, plugin `41ee80ed…`);
the plugin was rebuilt after the B4 commit so its embedded `GIT_HEAD_SHA1`
matches HEAD (the first build attempt embedded the stale `fa3c61172…`;
corrected 2026-09-14 01:31, see `smoke_runs/preflight_c3/`).
