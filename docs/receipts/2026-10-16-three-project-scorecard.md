# Three-project isolated scorecard receipt (2026-10-16)

This receipt records a fresh read of the Pulp, TartCI, and Shipyard project
scorecards from an isolated Shipyard checkout at exact protected heads. It is
observational evidence only. It authorizes no promotion, routing change,
merge, release, or safe-merge claim.

## Provenance

- Pulp protected head: `3c2334da9151e68e3040cd54de78c8da59d776e5`
- TartCI protected head: `0ed9da98c044eea117ddc390f247c586f6e5b637`
- Shipyard protected head: `100d3bbabbe784fdbfd381350d401b2aad9fd916`
- Shipyard checkout: `/Users/danielraffel/Code/Shipyard-baseline-refresh`
- Shipyard executable: `/Users/danielraffel/Code/Shipyard-baseline-refresh/target/debug/shipyard`
- Metrics database: `/Users/danielraffel/Library/Application Support/shipyard-dev/metrics/metrics.db`
- Database size at capture: `1,105,920` bytes
- Database mtime at capture: `2026-10-16 07:39:28 -0700`
- Raw JSON: [`2026-10-16-three-project-scorecard.json`](2026-10-16-three-project-scorecard.json)

The exact command was run once per project:

```text
shipyard metrics scorecard --mode isolated --fail-on-stale --project <project> --since 3d --json
```

The project values were `pulp`, `danielraffel/tartci`, and
`danielraffel/Shipyard`. All commands exited zero and reported `fresh` against
the 24-hour (`86400` second) threshold.

## Captured metrics

Percentages are the scorecard's reported fractions rendered as percentages;
durations are minutes unless marked milliseconds.

| Metric | Pulp | TartCI | Shipyard |
| --- | ---: | ---: | ---: |
| Job samples | 222 | 460 | 800 |
| Successful jobs | 144 | 301 | 501 |
| Failed jobs | 3 | 16 | 15 |
| Other jobs | 75 | 143 | 284 |
| Worker minutes | 480.1167 | 2,668.1833 | 2,551.3333 |
| Failure rate | 2.0408% | 5.0473% | 2.9070% |
| Cancelled share | 7.2072% | 19.1304% | 31.3750% |
| Starvation share | 3.1532% | 6.7391% | 22.6250% |
| Queue p50 / p90 | 2s / 2s | 3s / 3s | 3s / 4s |
| Wasted compute | 49.4167 | 694.2500 | 1,110.0833 |
| Coordination | 23.5667 | 17.7333 | 1,427.0167 |
| Rework compute | 1.3500 | 277.1167 | 72.3833 |
| Coordination + rework | 24.9167 | 294.8500 | 1,499.4000 |
| Distinct pull requests | 3 | 7 | 0 |
| Comparison verdict | `insufficient_sample` | `insufficient_sample` | `insufficient_sample` |

Freshness at each command's `checked_at`:

| Project | `checked_at` | Newest sample | Age | Status |
| --- | --- | --- | ---: | --- |
| Pulp | `2026-10-16T14:49:27.543900Z` | `2026-10-16T14:21:54Z` | 1,653s | `fresh` |
| TartCI | `2026-10-16T14:49:27.622476Z` | `2026-10-16T14:36:09Z` | 798s | `fresh` |
| Shipyard | `2026-10-16T14:49:27.754217Z` | `2026-10-16T14:03:36Z` | 2,751s | `fresh` |

## Data-quality and decision boundary

For all three projects, these fields remain explicitly unavailable because no
durable merge receipt joins a PR head to a safely merged change:

- `safely_merged_changes`
- `wasted_compute_per_safely_merged_change`

Other gaps are preserved in the raw JSON: duration coverage is partial (Pulp
191/222, TartCI 423/460, Shipyard 760/800), cache samples are zero for all
three, submission-to-receipt timestamps and model/token counters are
unavailable, and Shipyard has no completed samples with PR identity. Pulp and
TartCI PR identity coverage is partial (200/222 and 58/460 respectively).

The scorecard comparison is `insufficient_sample` for every project because
there is no before window in this isolated three-day read. This receipt supports
no promotion, routing, release, or safe-merge decision.
