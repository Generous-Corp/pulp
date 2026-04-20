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
audio, cache, clean, upgrade, config, export-tokens, ci-local, design-debug, help

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

## `pulp validate` — plugin-format validators

Runs `clap-validator` / `pluginval` / `auval` / optional AAX validator
on built plugins in `build/{CLAP,VST3,AU,AAX}`. Lives in
`tools/cli/cmd_validate.cpp`.

Modes:

- Default — best-effort. Missing validators skip gracefully but emit a
  loud WARNING at the end listing each missing tool + install hint.
- `--strict` — CI enforced tier. Any "skipped because tool not
  installed" upgrades to exit 1. Use in CI gates.
- `--all` — also run optional `vstvalidator` + full AAX validation.
- `--json` / `--report <path>` — emit structured report
  (`validation-report-v1.schema.json`).
- `--screenshot` — capture plugin editor PNGs under
  `artifacts/screenshots/`.

Gotchas:

- **Missing-tool skip is NOT a silent pass.** The advisory at the end
  of the run enumerates absent tools; `--strict` gates CI on it. A
  green run without all four validators is *not* the same as a run
  where all four passed — the advisory makes that visible.
- **Each missing tool is reported once** even across many plugins.
  The `note_missing` helper de-duplicates by tool name.
- **Install hints are embedded in the code** — if you add a new
  format lane, wire `note_missing("<tool>", "<format>", "<hint>")`
  alongside the `++skipped_missing_tool` bump in the skip branch.
  Otherwise `--strict` won't know about it.
- **Exit code still follows `failed > 0` first.** `--strict` only
  adds "OR any skipped-missing-tool" on top. Genuine validator
  failures still fail without `--strict`.

## `pulp upgrade` — self-update

Lives in `tools/cli/cmd_upgrade.cpp` (moved out of `cmd_misc.cpp` in
#547 Slice 2 so the update-check surface can grow independently) and
calls `pulp::cli::pulp_upgrade_url_for()` from
`tools/cli/upgrade_url.hpp`. The URL/asset-name convention is **pinned
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

## `pulp doctor --versions` — version diagnostics (#499 Slice 1)

The first slice of the release-discovery UX (issue #499) is a pure
diagnostic that short-circuits the doctor pipeline: it prints
CLI/SDK/Plugin versions side-by-side plus advisory skew warnings and
always exits 0. Lives in `tools/cli/version_diag.{hpp,cpp}` with
`cmd_doctor` as the only caller.

Gotchas:

- **`version_diag` is deliberately decoupled from `cli_common`.** It
  re-implements its own tiny `read_toml_scalar` / `user_home_dir_local`
  helpers so the unit-test binary can link just `version_diag.cpp` —
  no pulp::runtime link surface. If you add a new helper, keep it
  local unless you've also evaluated the test impact.
- **Always exit 0 even on WARN.** Skew is advisory. Making this
  command gate on skew would break scripts that invoke
  `pulp doctor --versions` as a routine health check in a pipeline.
  This is a design choice, not an oversight.
- **Untagged builds are silently skipped.** Anything that doesn't
  parse as `M.N.P` (e.g. `0.24.0-dev`, a git SHA) has
  `Semver{.comparable = false}`. Skew analysis short-circuits on
  non-comparable inputs per the design doc.
- **Plugin lookup prefers the repo's `.claude-plugin/plugin.json`.**
  The installed-plugin layout inside `~/.claude/plugins/pulp/` or
  `~/.claude-plugin/pulp/` is an open question in the design doc —
  the lookup is best-effort and deliberately forgiving; when in
  doubt it reports "(not found)" instead of failing.
- **`cli_min_version` is optional and additive.** The field only
  becomes meaningful from the first release that needs it; before
  then it's silently absent and skew analysis skips. Don't
  retroactively add it to every `pulp.toml` in the repo.

Follow-up slices (2-6) are tracked against #499: update-check,
migration docs, `/upgrade` skill, mode enforcement, and plugin ↔ CLI
skew detection. Do not land them piecemeal under Slice 1's PR; file
new issues and PRs.

## `pulp config` + update-check (#499 Slice 2 / #547)

Slice 2 wires a 24h update-check cache plus a config surface for
`~/.pulp/config.toml`. Key layout:

- **`tools/cli/update_check.{hpp,cpp}`** — pure-logic core. No
  `cli_common` link dep so the unit tests in
  `test/test_cli_update_check.cpp` can compile it standalone (same
  pattern as `version_diag`). Exposes `CacheEntry`, `parse_cache_json`,
  `serialize_cache_json`, `read_cache_file`, `write_cache_file`,
  `is_cache_stale`, `is_newer`, `compose_banner`,
  `write_toml_key_in_section`, and the `Fetcher` interface with a
  real `GitHubReleasesFetcher` (curl/PowerShell) so tests can inject
  a fake and never hit the network.
- **`tools/cli/cmd_upgrade.cpp`** — moved out of `cmd_misc.cpp`. Adds
  `--check-only` reading the cache. Writes
  `banner_shown_for_version` after a successful upgrade so the
  next-invocation banner stays quiet for the version we just installed.
- **`tools/cli/cmd_config.cpp`** — `pulp config get|set|list` with an
  allow-list of keys (`update.mode`, `update.check_interval_hours`,
  `update.channel`). Allow-list prevents typos silently inflating the
  config surface.
- **`tools/cli/pulp_cli.cpp`** — `maybe_emit_update_banner_and_refresh()`
  runs before dispatch. `PULP_UPDATE_CHECK_DISABLED` env short-circuits
  it (used by CI). `banner_blocked_commands` = `config`, `version`,
  `help` so machine-parseable output stays clean.
- **Banner shape** (locked, tested verbatim):
  `Pulp vX.Y.Z available (you have vA.B.C). Run \`pulp upgrade\` or \`pulp config set update.mode manual\` to silence.`
  Emitted on **stderr**, not stdout — never corrupts piped output.

Gotchas:

- **Slice 2 does NOT implement the interactive y/N prompt.** Full
  auto/prompt/manual/off enforcement lands in Slice 5. Today's `prompt`
  mode prints the one-shot informational banner only. Don't
  retroactively add interactive blocking to `prompt` mode without
  reading the design doc Section A first.
- **`update.channel = "beta"` is accepted by the allow-list but
  ignored.** Reserved for Slice 5. Accepting it now means users can
  set it ahead of time without the config reader errors; don't surface
  it in the `pulp config --help` Examples until Slice 5 honours it.
- **Anonymous GitHub API only.** 60 req/hr/IP. The 24h cache default
  keeps us well under that. Do NOT add authenticated fetches — the
  design is explicit about "no GitHub App, no auth".
- **`std::async(std::launch::async, ...)` is stored in a static
  future** so the destructor doesn't block `main` on Windows. The
  refresh thread finishes in < 2s normally and the process exits
  either way — don't replace with `std::thread::detach` without
  thinking about signal delivery and CRT finalization on Windows.
- **Cache file is atomic via `.tmp` + rename.** A torn write just
  forces a re-fetch on the next invocation, not corruption. Cross-device
  rename falls back to `copy_file` + `remove`.
- **Commit trailer block must be contiguous.** Version-bump + skill
  trailers live on the tip commit. Do NOT split with blank lines —
  `git interpret-trailers --parse` treats them as non-trailers.
