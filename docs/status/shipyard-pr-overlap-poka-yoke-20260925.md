# Shipyard PR overlap poka-yoke

The WaveNet work exposed a queue inefficiency: several open PRs carried the
same shared commits and were admitted as separate PR-head validations. The
repository's pre-push advisor only inspected recent local branches, so it could
not see the complete set of open GitHub PR heads. The result was redundant
macOS gates and conflicting branches that should have been consolidated first.

The existing Shipyard merge steward already collects the authoritative open-PR
snapshot in `pull-request-open-heads.json`. A new read-only check,
`tools/scripts/shipyard_duplicate_pr_heads.py`, validates that snapshot and
groups heads by:

- head repository and commit;
- base repository and base ref.

It fails closed on malformed or incomplete census data and returns a duplicate
finding when two open PRs point at the same exact head identity. It also
collects each open PR's unmerged commit list and reports shared unmerged
commits, which is the case that actually occurred in the WaveNet set even
though the PR heads were different. The steward publishes
`duplicate-pr-heads.json` with its other evidence and marks its health check
unhealthy when a duplicate exists. Regression fixtures cover the WaveNet-shaped
collision, different base refs, different source repositories, and malformed
input. A live census on 2026-09-26 reproduced shared commits across #8848,
#8851, and #8858.

This is an early warning and admission hygiene check, not a second merge queue.
A REST census has a time-of-check/time-of-use window, so atomic exact-head
handoff in Shipyard remains the authoritative merge-time prevention. The
repository-side gap is now explicit and testable rather than relying on an
agent to notice overlap manually.
