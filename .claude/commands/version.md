---
name: version
description: Show, bump, or check the Pulp SDK / CLI + Claude plugin versions
---

Answer "what version of Pulp is this project using?" — always print both the Claude plugin version and the Pulp SDK / CLI version on one line, e.g.:

```
Claude plugin 0.5.0 · Pulp SDK/CLI 0.13.0
```

### 1. Plugin version
Read from `.claude-plugin/plugin.json` using a single `jq` command:
```bash
plugin_version=$(jq -r .version .claude-plugin/plugin.json 2>/dev/null)
```

### 2. Pulp SDK / CLI version
Canonical source is `project(Pulp VERSION X.Y.Z)` in the repo's `CMakeLists.txt`. Prefer the running CLI when available, fall back to parsing:

Canonical command surface for this slash-command: `pulp version`

```bash
if command -v pulp >/dev/null 2>&1; then
    sdk_version=$(pulp version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
elif [ -x ./build/pulp ]; then
    sdk_version=$(./build/pulp version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
else
    sdk_version=$(grep -E "project\(Pulp VERSION" CMakeLists.txt | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
fi
```

### 3. Print the canonical line
```bash
echo "Claude plugin ${plugin_version:-unknown} · Pulp SDK/CLI ${sdk_version:-unknown}"
```

### Subcommands via $ARGUMENTS
- `bump patch|minor|major` — run `python3 tools/scripts/version_bump_check.py --mode=apply` then remind the user to `pulp pr`.
- `check` — run `python3 tools/scripts/version_bump_check.py --mode=report` to verify both surfaces are in sync.

After bumping, remind the user to rebuild (the CLI constant is derived from CMake at configure time) and to land the change via `pulp pr` so `auto-release.yml` tags the new version.

See `docs/reference/versioning.md` for the full surface-by-surface explanation.
