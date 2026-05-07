---
name: status
description: Show Pulp project status, build state, and configuration
---

Show the current project status.

Always start by printing the canonical version line so "what Pulp am I using?" is answered on every `/status` invocation:

```
Claude plugin <plugin_version> · Pulp SDK/CLI <sdk_version>
```

See `.claude/commands/version.md` for the exact parsing recipe. Reuse the same logic — do not reinvent it.

Then run: `pulp status` when the CLI is on PATH, or `./build/pulp status` from a source build.
In a Pulp source checkout, include the PR workflow lines from `pulp status`
when reporting status; they show whether this checkout is using Shipyard,
direct `gh`, or manual PR handling.

If the CLI binary doesn't exist, fall back to showing:
1. `git status` — current branch and changes
2. `git log --oneline -5` — recent commits
3. `ls build/` — whether a build directory exists
4. `cat docs/status/support-matrix.yaml | head -40` — format support status
