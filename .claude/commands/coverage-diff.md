---
name: coverage-diff
description: Run the local diff-coverage check that mirrors CI's `Diff coverage required` gate
---

Run the local diff-coverage check before pushing. Mirrors CI's `Diff coverage required (75%)` gate so coverage-only failures don't cost a 20-min CI roundtrip.

Threshold + filters live in `tools/scripts/coverage_config.json` — edit there once and CI + local stay in sync.

If $ARGUMENTS is provided, treat it as a space-separated list of test targets to build (faster path):

```bash
tools/scripts/local_diff_cover.sh $ARGUMENTS
```

Otherwise build everything (slow, matches CI):

```bash
tools/scripts/local_diff_cover.sh
```

Bypass on doc-only / workflow-only PRs:

```bash
PULP_SKIP_DIFF_COVER=1 tools/scripts/local_diff_cover.sh
```

If the script fails:

- Read the HTML report for the per-file uncovered-line breakdown. The script
  prints its path at the end — it is written under this worktree's `build-cov/`,
  so read the path it printed rather than assuming a shared location.
- Add a test that exercises the new lines, or refactor to remove the dead branch.
- The pre-push hook enforces this check by default when relevant code changed; `PULP_DISABLE_PREPUSH_DIFF_COVER=1` demotes only diff-cover to advisory.

If the script can't run because `diff-cover` isn't installed:

```bash
pip install --user 'diff-cover>=9'
```
