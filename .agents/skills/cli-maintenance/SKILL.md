---
name: cli-maintenance
description: Checklist and decision tree for adding, modifying, or removing CLI commands. Keeps CLI source, slash commands, docs, and skills in sync.
requires:
  scripts:
    - tools/scripts/cli_sync_check.py
---

# CLI Maintenance

## When to use this skill

- Adding a new subcommand to the CLI
- Changing args/behavior of an existing command
- Removing or renaming a command
- Responding to a cli-plugin-sync hook reminder
- Auditing CLI / plugin / docs consistency

## Adding a CLI Command — Full Checklist

### 1. Implement in CLI source
- [ ] Create `tools/cli/cmd_<name>.cpp` with `int cmd_<name>(const std::vector<std::string>& args)`
- [ ] Add declaration to `tools/cli/cli_common.hpp` in the command forward declarations section
- [ ] Add entry to the command table in `tools/cli/pulp_cli.cpp` (Command, ScriptCommand, or BinaryCommand)
- [ ] Update `tools/cli/CMakeLists.txt` to compile the new file

### 2. Update the CLI commands manifest
- [ ] Add entry to `docs/status/cli-commands.yaml` with:
  - `name`, `status` (use status vocabulary: stable/usable/experimental), `summary`
  - `args` with name, kind (positional/option/flag/passthrough), description
  - `subcommands` if applicable

### 3. Decide: does this need a slash command?

**Create a slash command** (`.claude/commands/<name>.md`) if:
- The command is user-facing and interactive (not plumbing)
- An agent would benefit from a `/name` shortcut
- The command has a natural "ask the user then run" pattern

**Skip a slash command** if:
- It's a low-level plumbing command (e.g., `cache clean`)
- It's adequately covered by an existing skill
- It's a subcommand of something already covered

**Commands that intentionally don't have slash commands:**
audio, cache, clean, upgrade, export-tokens, ci-local, design-debug, help

### 4. Update docs
- [ ] Add/update section in `docs/reference/cli.md`
- [ ] If it changes capabilities: update `docs/reference/capabilities.md`
- [ ] If it's in the plugin: update `docs/guides/claude-code-plugin.md` command table

### 5. Update skills that reference CLI commands
- [ ] `grep -r "pulp <name>" .agents/skills/` — update any skill that calls this command

### 6. Update CLAUDE.md if needed
- [ ] If the command is referenced in the "CLI tool" code block
- [ ] If the command changes the build/test/validate workflow

### 7. Validate sync
```bash
python3 tools/scripts/cli_sync_check.py
```

## Modifying a CLI Command

Same as above, focus on steps 2, 4, 5, 6, 7. Key risks:
- Changed args not reflected in `cli-commands.yaml`
- Changed behavior not reflected in slash command `.md`
- Skills calling the old invocation syntax

## Removing a CLI Command

- [ ] Remove from `cmd_*.cpp` and command table in `pulp_cli.cpp`
- [ ] Remove from `cli_common.hpp` declarations
- [ ] Remove from `CMakeLists.txt`
- [ ] Remove from `cli-commands.yaml`
- [ ] Remove slash command if it exists
- [ ] Remove from `docs/reference/cli.md`
- [ ] Search all skills: `grep -r "pulp <name>" .agents/skills/`
- [ ] Search CLAUDE.md
- [ ] Run sync check

## `pulp pr` — shim over `shipyard pr`

By default `pulp pr` now delegates to `shipyard pr` (on PATH), forwarding
argv. Shipyard owns skill-sync, version-bump, PR creation, and cross-host
validation; `pulp pr` exists so the old invocation, the `/pr` slash command,
and the natural-language triggers in the `ci` skill all continue to work.

Invariants:

- When `shipyard` is on PATH, `pulp pr` execs `shipyard pr <args>` and exits
  with shipyard's status. Do NOT add pre/post-processing in `cmd_pr.cpp` —
  shipyard is the single source of truth.
- When `shipyard` is NOT on PATH, `pulp pr` prints an install hint pointing
  at `tools/install-shipyard.sh` and exits non-zero. Update the hint text
  in `cmd_pr.cpp::print_install_shipyard_hint()` when the install path
  changes.
- `--native` keeps the legacy in-process orchestrator (skill-sync →
  version-bump apply → commit → push → `gh pr create` → `shipyard ship`)
  for forensic/debugging use. Do not use it as the default path and do not
  document it as the primary surface — the shim IS the primary surface.
- `pulp pr` (shim or `--native`) still refuses to run on `main`.

Gotchas:

- **Changing `cmd_pr.cpp` triggers the cli-maintenance skill-sync gate.**
  If you're modifying the shim, the install hint, or `--native` logic,
  add a bullet here and you're covered. If the change is mechanical (e.g.
  renaming a helper) and genuinely doesn't need skill documentation, add a
  `Skill-Update: skip skill=cli-maintenance reason="..."` trailer on the
  tip commit.
- **Don't call the Python scripts (`skill_sync_check.py`,
  `version_bump_check.py`) by hand.** `shipyard pr` calls them with the
  right flags via the shim. Direct invocation skips the commit-trailer
  parsing and the PR-body rendering.
- **Bump level is per-surface.** A plugin-only `feat:` in a commit subject
  does not upgrade the SDK. The `Version-Bump: <surface>=<level>` trailer
  is authoritative and surface-scoped.
- **`pulp version check --with-bump-check`** is the fast sanity check to
  run *before* `pulp pr` if you want to see what the gate will say. Same
  script, `--mode=report`.

## `pulp upgrade` — self-update

Lives in `tools/cli/cmd_misc.cpp` and calls `pulp::cli::pulp_upgrade_url_for()`
from `tools/cli/upgrade_url.hpp`. The URL/asset-name convention is **pinned
by the release workflow** (`.github/workflows/release-cli.yml`) and guarded
by `test/test_cli_upgrade_url.cpp` — both need to agree.

Convention (don't drift):

```
Asset = "pulp-<platform>-<arch>.<ext>"  (NO version in the filename)
URL   = ".../releases/download/v<version>/<asset>"
arch  = "arm64" | "x64"                  (NOT "x86_64")
ext   = "zip" for windows, "tar.gz" otherwise
```

Gotchas:

- **Don't bake the version into the asset filename.** The version sits in
  the release *tag* (path segment `v<version>`), not in the file name.
  PR #377 / issue #352 was the bug where the filename contained the
  version and every upgrade 404'd. `test_cli_upgrade_url.cpp` explicitly
  fails if the version reappears in the filename.
- **Use `x64`, not `x86_64`.** The release workflow uploads under `x64`.
- **If you change `upgrade_url.hpp`, update the regression test in the
  same PR.** Both live at HEAD; drift between them is the whole reason
  the header exists.

## `pulp version check`

Validates consistency across the three version-bearing surfaces:

- SDK: `CMakeLists.txt` `project(... VERSION x.y.z ...)` ↔ compile-time `PULP_SDK_VERSION` constant.
- Claude plugin: top-level `"version"` in `.claude-plugin/plugin.json` (semver).
- Marketplace: top-level `"version"` in `.claude-plugin/marketplace.json` (must match `plugin.json`).

Gotcha: JSON files can have multiple `"version"` fields (e.g. `metadata.version`, `plugins[0].version`). The check anchors on the top-level field via a `^(?:  )?"version":` regex — don't introduce a JSON parser unless you genuinely need schema validation.


### Note: pulp scan / pulp host added

These commands live in `tools/cli/cmd_host.cpp` (scan + host share a
file). When changing scanner.scan() signatures, update cmd_host.cpp's
ScanOptions construction in lockstep — the cross-format loop builds an
options struct per iteration.

## Phase 0 host-contracts touchpoints

`tools/cli/cmd_host.cpp` calls `PluginSlot::process()` which now takes
a `ParameterEventQueue`. When adding new CLI hosting commands, include
`pulp/host/parameter_event_queue.hpp` and pass an empty queue if you
have no automation to deliver. See `docs/reference/host-thread-rules.md`
for the full contract.
