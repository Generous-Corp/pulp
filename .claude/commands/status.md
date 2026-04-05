---
name: status
description: Show Pulp project status, build state, and configuration
---

Show the current project status.

Run: `./build/tools/cli/pulp status`

If the CLI binary doesn't exist, fall back to showing:
1. `git status` — current branch and changes
2. `git log --oneline -5` — recent commits
3. `ls build/` — whether a build directory exists
4. `cat docs/status/support-matrix.yaml | head -40` — format support status
