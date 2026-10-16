# TartCI and Shipyard isolated scorecard receipt (2026-10-16)

This receipt records the fresh isolated scorecards captured at
`2026-10-16T14:37:36Z`. It is observational evidence only. Neither scorecard
authorizes promotion, routing changes, merge, or a safe-merge claim.

## Reproduction

The scorecards were read from the local Shipyard metrics database:

`/Users/danielraffel/Library/Application Support/shipyard-dev/metrics/metrics.db`

Executable used:

`/Users/danielraffel/Code/Shipyard-fleet-baseline-scorecard/target/debug/shipyard`

Commands (one per project):

```text
shipyard metrics scorecard --mode isolated --fail-on-stale --project danielraffel/tartci --since 3d --json
shipyard metrics scorecard --mode isolated --fail-on-stale --project danielraffel/Shipyard --since 3d --json
```

The command's freshness threshold was 24 hours (`86400` seconds). Both
sources were fresh at capture time:

| Project | Newest sample | Age at checked_at | Freshness |
| --- | --- | ---: | --- |
| `danielraffel/tartci` | `2026-10-16T14:36:09Z` | 87 seconds | `fresh` |
| `danielraffel/Shipyard` | `2026-10-16T14:03:36Z` | 1,440 seconds | `fresh` |

## Captured metrics

Values below are copied from the JSON emitted at the checked time. Percentages
are percentages of the scorecard's reported job population; durations are
minutes unless marked seconds.

| Metric | TartCI | Shipyard |
| --- | ---: | ---: |
| Job samples | 300 | 498 |
| Successful jobs | 204 | 324 |
| Failed jobs | 6 | 12 |
| Other jobs | 90 | 162 |
| Worker minutes | 1,532.4167 | 1,286.9167 |
| Failure rate | 2.8571% | 3.5714% |
| Cancelled share | 19.3333% | 28.7149% |
| Starvation share | 6.6667% | 18.6747% |
| Queue p50 / p90 | 3s / 3s | 3s / 4s |
| Wasted compute | 362.1667 | 436.5167 |
| Coordination | 11.9167 | 439.8667 |
| Rework compute | 107.3333 | 59.2 |
| Coordination + rework | 119.25 | 499.0667 |
| Distinct pull requests | 7 | unavailable (no PR identity) |
| Comparison verdict | `insufficient_sample` | `insufficient_sample` |

## Safe-merge and promotion boundary

The following fields were unavailable for both projects because the metrics
store had no durable merge receipt joining a PR head to a safely merged change:

- `safely_merged_changes`
- `wasted_compute_per_safely_merged_change`

The scorecard verdict is `insufficient_sample` for both projects. Therefore the
record supports no promotion, safe-merge, routing, or release decision. A later
decision must regenerate both scorecards against the same command and a newly
checked database state, then record its own `checked_at` and exact-head
provenance.

## Fleet observation recorded with this receipt

The step-1 `m5s` reachability check was attempted at approximately
`2026-10-16T14:38Z` and `2026-10-16T14:44Z` through `ssh m5s`, `m5s-lan`, and
`m5s-wifi`. Each attempt timed out during banner exchange with
`UNKNOWN:65535`. No restart or promotion was attempted, and the existing m5s
shadow status remains `NOT ESTIMABLE` because the host is unreachable.

The m1 `PASS DONE` result at `2026-10-16T14:27:38Z` was verified. This host
observation is recorded for provenance only and does not change either
scorecard verdict.
