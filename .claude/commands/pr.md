---
description: Push a PR via `pulp pr` — runs skill-sync + version-bump + gh pr create + shipyard ship.
---

Run the `pulp pr` subcommand. It is the single orchestrator for shipping a
branch to `main`:

1. Runs `tools/scripts/skill_sync_check.py` against `origin/main`. Hard-fails
   if a mapped skill path was touched without updating the corresponding
   `SKILL.md` (or a `Skill-Update: skip skill=<name> reason="..."` trailer).
2. Runs `tools/scripts/version_bump_check.py --mode=apply`, which bumps SDK
   (CMakeLists.txt), Claude plugin (`.claude-plugin/plugin.json`), and the
   marketplace entry (`.claude-plugin/marketplace.json`) as dictated by the
   heuristic + any `Version-Bump:` trailers on the branch.
3. Commits the bump as `chore: bump <surfaces>` if anything changed.
4. `gh pr create` against `main` with a generated body summarizing the
   bumps and the diff surface.
5. `shipyard ship` — cross-platform validate + merge on green.
6. Returns only after the PR merges, or after a clean failure.

Do NOT run `gh pr create` and `shipyard ship` separately. Do NOT run the
skill-sync or version-bump scripts by hand — `pulp pr` invokes them in the
right order with the right flags.

On completion, `.github/workflows/auto-release.yml` picks up the merged
version bump on `main` and publishes a GitHub Release.

```sh
pulp pr $ARGUMENTS
```
