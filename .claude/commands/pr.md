---
description: Push a PR via `shipyard pr` — runs Pulp gates, creates/tracks the PR, validates, and merges on green.
---

Run `shipyard pr`. It is the single orchestrator for shipping a branch to
`main`:

1. Runs `tools/scripts/skill_sync_check.py` against `origin/main`. Hard-fails
   if a mapped skill path was touched without updating the corresponding
   `SKILL.md` (or a `Skill-Update: skip skill=<name> reason="..."` trailer).
2. Runs `tools/scripts/version_bump_check.py --mode=apply`, which bumps SDK
   (CMakeLists.txt), Claude plugin (`.claude-plugin/plugin.json`), and the
   marketplace entry (`.claude-plugin/marketplace.json`) as dictated by the
   heuristic + any `Version-Bump:` trailers on the branch.
3. Commits the bump as `chore: bump <surfaces>` if anything changed.
4. Pushes the branch, creates the PR, and records Shipyard tracking state.
5. Runs cross-platform validate + merge on green.
6. Returns only after the PR merges, or after a clean failure.

Do NOT run `gh pr create` and `shipyard ship` separately. Do NOT run the
skill-sync or version-bump scripts by hand — `shipyard pr` invokes them in the
right order with the right flags.

Direct `gh pr create` is an explicit emergency/manual bypass only. If used,
tell the user the PR may not appear in Shipyard-managed state until it is
reconciled or re-shipped through Shipyard.

`pulp pr` remains a compatibility wrapper that defaults to `shipyard pr`.
Humans may configure `pr.workflow=github` or `manual` for their own checkout,
but do not choose those paths unless the user explicitly asks for the bypass.

On completion, `.github/workflows/auto-release.yml` picks up the merged
version bump on `main` and publishes a GitHub Release.

```sh
shipyard pr $ARGUMENTS
```

Use `pulp status` when you need to confirm the effective local PR workflow.
