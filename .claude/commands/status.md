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

Then run: `./build/tools/cli/pulp status`

If the CLI binary doesn't exist, fall back to showing:
1. `git status` — current branch and changes
2. `git log --oneline -5` — recent commits
3. `ls build/` — whether a build directory exists
4. `cat docs/status/support-matrix.yaml | head -40` — format support status
