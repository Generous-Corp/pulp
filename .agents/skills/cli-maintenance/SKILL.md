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
audio, cache, clean, export-tokens, ci-local, design-debug, harness, help, projects, project

**Commands that DO have slash commands** (list for cross-reference, not exhaustive — `ls .claude/commands/` is authoritative):
build, test, run, validate, ship, version, doctor, create, docs, status, design, import-design, inspect, pr, ci, upgrade, prototype-loop, motion

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
python3 tools/scripts/check_cli_mcp_parity.py --mode=report
```

### 8. Decide: does this need an MCP tool? (pulp #1997)

Every top-level CLI command is checked for MCP parity by
`tools/scripts/check_cli_mcp_parity.py`. The check enforces an
invariant: a new CLI command must either land alongside a
`pulp_<command>` tool in `tools/mcp/pulp_mcp.cpp` (with both a
`tools_list_json()` entry AND a `handle_request()` dispatch arm),
OR it must be added to `tools/scripts/cli_mcp_parity_baseline.json`
under `cli_only` with a one-line reason.

Decision heuristics for "does this deserve MCP exposure":

- One-shot RPC-shaped command (build, test, validate) → **YES**, add the MCP tool.
- Long-running watch/loop (dev, loop, run, host) → **NO**, baseline it.
- Trivial Bash equivalent (clean, version, config) → **NO**, baseline it.
- Interactive surface (design, ship) → **NO**, baseline it.
- Subcommand under an umbrella tool already present (audio, docs, inspect) → **NO**, baseline it (the umbrella's sub-tools cover the surface).

The gate runs in three places, all pinned to the same script:

1. `hooks/scripts/cli-plugin-sync.sh` — advisory `--mode=hint` after Edit/Write of
   `pulp_cli.cpp`, `pulp_mcp.cpp`, or the baseline JSON.
2. `.github/workflows/version-skill-check.yml` — enforcing `--mode=report`
   on every PR. Hard-fails if a new CLI command lacks both an MCP tool and
   a baseline entry.
3. The shipping pre-push gate stack (skill-sync / version-bump / compat-sync)
   — the parity check rides alongside.

Naming convention: hyphenated CLI command `import-design` ↔ underscored
MCP tool name `pulp_import_design`. The script handles the conversion
automatically; baselines should use the form natural to each side
(`cli_only` is hyphenated, `mcp_only` carries the full `pulp_` prefix).

When promoting an entry off the `cli_only` list, add the matching
`pulp_<command>` tool to `tools/mcp/pulp_mcp.cpp` and the parity check
will auto-detect the new coverage; remove the baseline entry in the same PR.

## Modifying a CLI Command

Same as above, focus on steps 2, 4, 5, 6, 7. Key risks:
- Changed args not reflected in `cli-commands.yaml`
- Changed behavior not reflected in slash command `.md`
- Skills calling the old invocation syntax
- Output path flags should accept both nested paths and bare filenames; guard
  empty `std::filesystem::path::parent_path()` before creating directories and
  add shellout coverage for the bare-filename case.

### Rust CLI cutover path convention

After the v0.78.1 cutover, the user-facing CLI is Rust `pulp`. Release
archives install `pulp` plus sibling `pulp-cpp`, and source builds stage the
Rust binary at `./build/pulp`. Slash-command examples should prefer `pulp`
on PATH for installed users and `./build/pulp` for source-tree examples.
Do not point new docs at `./build/tools/cli/pulp`; that path was the old
C++ default. Use `pulp-cpp` only when documenting fallthrough, rollback, or
debug comparisons.

### Binary subcommand delegation

`BinaryCommand` entries in `tools/cli/pulp_cli.cpp` delegate to helper binaries
inside the active build tree, e.g. `pulp import-design` launches
`tools/import-design/pulp-import-design`. Do not hard-code `build/` for these
lookups: CI uses matrix-scoped directories such as `build-linux`,
`build-macos`, and `build-windows`, and self-hosted macOS can mask mistakes
with stale warm `build/` artifacts. Resolve helpers from the running CLI's
sibling build tree first, honor `PULP_BUILD_DIR` when present, and keep
`test/test_cli_shellout.cpp` subprocess-output `INFO(...)` diagnostics so
future failures show the delegated binary's stderr.

**Cwd independence (2026-05-14):** `delegate_to_build_binary` in
`tools/cli/cli_common.cpp` MUST NOT require `cwd` to be inside a Pulp
project to find a delegate. Sibling helpers live next to the CLI binary
itself, so the argv[0]-relative resolution path is authoritative and
project-root is a fallback. The original `require_project_root()` gate
broke `cd /tmp && pulp import-design --from claude --file ~/x.html` for
no good reason — a first-time user shouldn't need to `cd` into a pulp
checkout just to translate a design file with absolute paths. When the
delegate truly is missing, the error message lists every candidate path
that was tried and gives the exact `cmake --build` line to remediate;
do not regress to "Run `pulp build` first" — that's misleading if the
top-level target doesn't depend on the missing helper.

### Numeric CLI flags

For count-like flags such as `pulp run --frames`, parsers should accept only
plain non-negative decimal digits and reject a leading `+`. C++ `from_chars`
acceptance varies by implementation for signed prefixes, and the Rust-facing
CLI surface should stay deterministic across platforms. Add regression tests
for boundary spelling when changing these parsers.

### Import-design artifact flags

`pulp import-design --output <path>` is the destination for the primary
artifact, not an artifact-kind selector. Keep sidecar anchoring tied to
`--output` while using `--emit {js|ir-json|cpp}` for the primary artifact
vocabulary. Accepted import-design artifact or runtime values must stay aligned
across `pulp_import_design.cpp`, `docs/reference/cli.md`,
`docs/reference/design-import.md`, `docs/status/cli-commands.yaml`, and the
import-design skill.

For `--emit ir-json`, asset-manifest flags are part of that same synced surface:
`--allow-network-fetch`, `--asset-cache`, `--asset-timeout-ms`, and repeated
`--asset-hash <uri=sha256>`. Keep their help text, YAML manifest rows, reference
docs, and import-design skill text in lockstep. Network asset fetching must
remain explicit opt-in; default JS emission should not fail just because an IR
asset reference points at HTTP(S).

`--mode baked` is implemented for `--emit ir-json` and `--emit cpp`; `cpp`
requires baked mode. `--mode live` remains the default and `--from jsx --mode
live --emit js` writes the precompiled bundle verbatim. Because that path does
not parse or render the bundle, it must reject `--validate`, `--reference`,
`--diff`, and `--debug` with a clear usage error rather than silently bypassing
shared post-processing. When changing this lane, keep the CLI help,
`docs/status/cli-commands.yaml`,
`docs/reference/cli.md`, `docs/reference/design-import.md`, and the
import-design skill aligned. The JSX baked snapshot policy is
`--snapshot-semantics {fail|warn|accept}`: `fail` rejects dynamic APIs by
default, `warn` proceeds with a structured diagnostic, and `accept` proceeds
silently.

The shipped import-design default is `--mode live --emit js`. User preference
overrides belong in the existing config surface as
`import_design.default_mode` (`live|baked`) and `import_design.default_emit`
(`js|ir-json|cpp`), with `PULP_IMPORT_DESIGN_DEFAULT_MODE` /
`PULP_IMPORT_DESIGN_DEFAULT_EMIT` as one-environment overrides. Keep C++ and
Rust `pulp config`, `pulp status`, the import-design helper, docs, slash
commands, and the MCP status output aligned whenever these keys change. If
only `default_mode=baked` is configured, `ir-json` is implied.

**Sidecar output anchoring:** when a CLI command takes `--output
<path>/main.ext` and also emits sidecar artifacts (e.g.
`pulp import-design` writes `bridge_handlers.cpp`, `classnames.json`,
`tokens.json` alongside `ui.js`), the sidecars MUST default to the same
directory as `--output` — not cwd. Track an `_explicit` bool per sidecar
flag and only derive the anchored path when the user didn't set it.
Scattering sidecars to cwd is a first-user trap (we hit it 2026-05-14
during a `cd /tmp` reimport demo). Test the default-anchored path AND
the explicit-override path so regressions surface.

### Adding a new value to an enum-like flag (e.g., `--from <source>`)

When extending a flag that takes one of a fixed set of values
(`pulp import-design --from {figma,stitch,v0,pencil,claude,...}`,
`pulp validate --target {auval,clap-validator,pluginval,...}`, etc.),
all four surfaces have to land in the same PR or the skill-sync gate
trips:

1. The CLI source switch (`tools/import-design/pulp_import_design.cpp`,
   etc.) — accept the new value in the parser **and** the dispatch.
2. The slash command file under `.claude/commands/` — usage block,
   examples, and any "when to detect this" notes.
3. The matching topical `.agents/skills/*/SKILL.md` — same prose,
   kept in sync so the agent's pre-context matches what the slash
   command says.
4. The path map (`tools/scripts/skill_path_map.json`) — add the new
   header/source paths if the new source brings new C++ files.

Reason this gets called out: editing `.claude/commands/<name>.md` maps
to the **`cli-maintenance`** skill (because the path map owns
`.claude/commands/**`), not to the topical skill. So a diff that
updates the slash command + the topical skill but leaves
cli-maintenance untouched still trips skill-sync. Keep this section
present so the gate stays satisfiable by the topical edit alone.
pulp #709 / `--from claude` is the worked example.

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

## `pulp project pin / unpin / bump` shell helpers

`tools/cli/cmd_project.cpp` shells out to `git` / `cmake` from unit-tested
helper paths, including Windows CI. Keep output redirection platform-aware:
POSIX uses `/dev/null`, but Windows `cmd.exe` needs `NUL`. Do not add raw
`2>/dev/null` or `>/dev/null 2>&1` in this file; route new call sites through
the local null-redirection helpers instead. Otherwise Windows Namespace can
misreport clean/dirty git state or fail origin-main probes even though the
same tests pass on macOS/Linux.

### pin / unpin / floating SDK mode

`pin` is the primary command name; `bump` survives as a deprecated alias
through `cmd_project`'s dispatch (`if (sub == "pin" || sub == "bump")`).
When adding a new project subcommand, add the canonical name AND keep
any old alias for one minor release — existing scripts and skill examples
break otherwise.

`pulp project unpin` rewrites `pulp.toml`'s `sdk_version` to `"latest"`
in-place (single-line value swap, preserving surrounding TOML
structure and comments). Do NOT delete the field — downstream tooling
that greps for it loses a clear signal. The dispatch entry lives at
the same spot as `pin`/`bump`/`undo` and shares `find_bumpable_project_root_from`.

`sdk_version = "latest"` is the floating-SDK marker. Resolution happens
in `read_sdk_version()` (cli_common.cpp) — `"latest"` becomes the
newest installed version under `~/.pulp/sdk/<x.y.z>/`, falling back to
`PULP_SDK_VERSION` when none are installed. Callers that need to
distinguish the floating marker from a real semver use
`read_raw_sdk_version()` + `is_floating_sdk()`. Don't open-code the
"latest" comparison anywhere — both helpers are exported from
cli_common.hpp so the comparison stays in one place.

`pulp create` writes `sdk_version = "latest"` by default
(cmd_create.cpp). The `--pin` flag opts into exact-version pinning
at create time. Both code paths print a discoverable post-create
message about `pulp project pin` so users learn the opt-in command
without hunting.

## `pulp pr` — shim over `shipyard pr`

By default `pulp pr` delegates to `shipyard pr` (on PATH), forwarding argv.
Shipyard owns skill-sync, version-bump, PR creation, tracking state, and
cross-host validation; `pulp pr` exists so the old invocation, the `/pr`
slash command, and natural-language triggers in the `ci` skill all continue
to work. Users can explicitly opt out per checkout with
`pulp config set pr.workflow github` or `manual`, or for one command with
`--workflow` / `PULP_PR_WORKFLOW`.

Invariants:

- When `shipyard` is on PATH, `pulp pr` execs `shipyard pr <args>` and exits
  with shipyard's status. Do NOT add pre/post-processing in `cmd_pr.cpp` —
  shipyard is the single source of truth.
- When `shipyard` is NOT on PATH, `pulp pr` prints an install hint pointing
  at `tools/install-shipyard.sh` and exits non-zero. It must NOT silently
  fall back to `gh`; that is how Shipyard tracking gaps happen. Update the
  hint text in `cmd_pr.cpp::print_install_shipyard_hint()` when the install
  path changes.
- `pr.workflow=github` is the explicit GitHub CLI path. It requires `gh` on
  PATH, runs the native gate/bump/PR flow, and leaves Shipyard tracking
  disabled by design.
- `pr.workflow=manual` prints the suggested commands and exits before
  pushing or creating a PR.
- `pulp status` reports the effective PR workflow and selected tool health.
- `--native` keeps the legacy in-process orchestrator for forensic/debugging
  use. Do not use it as the default path and do not document it as the
  primary surface — Shipyard-managed `shipyard pr` is the primary surface.
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

## `pulp macos` — per-PR macOS-runner retargeting

`tools/cli/cmd_macos.cpp` plus `.github/workflows/build-macos.yml`.

`pulp macos retarget --pr N --to <local|namespace|github-hosted>` cancels
any in-flight macOS-bearing workflow_runs for PR N (from both `build.yml`'s
matrix and any prior `build-macos.yml` dispatch) and fires a fresh
`gh workflow run build-macos.yml --ref <pr-head> --field runner=<choice>`.

Why a separate workflow file (and not just a new `pulp pr --retarget-macos`
flag): the `build.yml` matrix couples Linux/Windows/macOS into one
workflow_run. Rerunning macOS via that matrix re-runs Linux/Windows too.
`build-macos.yml` is independent — it produces its own `macos`-named
check that supersedes the matrix's `macos` check by recency. Branch
protection's required-check name stays one stable token.

`pulp macos status --pr N` reads the latest `macos` check from the GitHub
check-runs API for the PR's head SHA. Useful for confirming a retarget
landed and which runner pool picked it up.

Repo variables the workflow reads (defined globally for `build.yml`'s
overflow logic; reused here unchanged):
- `PULP_LOCAL_MACOS_RUNS_ON_JSON` — `--to local`
- `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON` — `--to namespace`
- (`--to github-hosted` is always `"macos-15"`, no var needed)

When this is the right tool vs. `shipyard rescue`:

| Goal | Tool |
|------|------|
| Move one PR's macOS to a different pool without disturbing Linux/Windows | `pulp macos retarget` |
| Move multiple PRs' workflow_runs to a different provider (cancel + redispatch the WHOLE workflow) | `shipyard rescue` |

## `pulp overflow` — macOS overflow routing

`tools/cli/cmd_overflow.cpp`. Wraps three repo variables that
`.github/workflows/build.yml`'s resolve-provider reads:

| Var | Purpose |
|-----|---------|
| `PULP_LOCAL_MACOS_RUNS_ON_JSON` | Selector when local has capacity |
| `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON` | Selector when local is saturated (overflow target; despite the historical name, this is the generic overflow var) |
| `PULP_LOCAL_MAC_OVERFLOW_THRESHOLD` | BUSY count that trips overflow |

Surfaces:

- `pulp overflow status` — show current state, including which runner
  the local target points at, the overflow target, threshold value, and
  registered self-hosted runners (if the default token can list them).
- `pulp overflow enable [--to <selector>]` — set the overflow target.
  Default `--to "macos-15"` for free GH-hosted; pass
  `--to '"namespace-profile-generouscorp-macos"'` for paid Namespace.
- `pulp overflow disable` — delete the overflow var. Note this only
  changes future dispatches; in-flight cloud runs keep running.
- `pulp overflow threshold [N]` — get (no arg) or set the BUSY count.

The variable is named `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON` for
historical reasons (Plan B in `planning/2026-05-13-namespace-overflow-
implementation.md` originally targeted Namespace exclusively). It now
holds the overflow target regardless of provider — rename tracked as a
future cleanup.

## `pulp upgrade --check-only`

The sandbox E2E harness runs this command with
`PULP_UPDATE_CHECK_DISABLED=1` and expects a non-silent, network-free result.
If the update cache is empty in that mode, print the installed CLI version
and an explicit disabled/not-queried latest-version line instead of probing
GitHub Releases. Otherwise PR sandbox lanes can fail spuriously when GitHub
release fetches are blocked or rate-limited.

## `pulp run --headless / --screenshot / --frames / --watch` (#914)

`tools/cli/cmd_run.cpp` plus the shared parser in
`tools/cli/cmd_run_parse.cpp` (`parse_run_options` / `assemble_launch_args`)
expose four CI-friendly flags on top of the basic launch path:

- `--headless` — run the standalone offscreen (no window). Forwarded
  as `--headless` and as the `PULP_HEADLESS=1` env var so binaries
  that read either source pick it up.
- `--screenshot <path>` — save a PNG to `<path>` after rendering.
  Implies `--headless`. Forwarded as `--screenshot <path>` AND
  `PULP_SCREENSHOT=<path>`. If `--headless` is set without an
  explicit screenshot path, the CLI writes
  `build/<target>.png` so simple invocations still produce an
  artifact.
- `--frames <n>` — number of frames to render before capturing
  (default 1). Forwarded as `--frames <n>` AND `PULP_FRAMES=<n>`.
- `--watch` — re-launch the binary on file changes via the existing
  `watch_loop` plumbing. Composes with the headless flags so dev
  loops can render PNGs on every save.

The CLI parser is unit-tested in `test/test_cli_run_options.cpp`
(parse + forwarding contract) and end-to-end shell-out coverage lives
in `test/test_cli_shellout.cpp` (`[issue-914]` tag), which exercises
the discover-binary → launch-with-flags → PNG-on-disk path against the
fixture binary in `test/fixtures/cli_run_fixture.cpp`.

Gotchas:

- `--screenshot` without a path argument exits 2 with a diagnostic.
  Bad `--frames` (non-integer / <= 0) exits 2 too. Both are caught
  before project resolution so they fail fast in `--help`-adjacent
  contexts.
- Both argv AND env vars are set on every invocation. Do not remove
  the env-var fallback — older standalone binaries (and the
  `pulp-screenshot` flow) read env first.
- `--watch` is consumed by the CLI; it is NOT forwarded to the
  launched binary. The launched binary just sees the headless
  flags.

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
- **Validator-discovery preflight (#743).** Before launching any
  validator binary, `pulp validate` runs the same discovery used by
  `pulp doctor --validators` and aborts cleanly if any candidate has
  a broken code signature (the "ripped from .app bundle" pattern,
  where amfid SIGKILLs the binary at launch with exit 137 / zero
  stderr). If you add a new validator to `cmd_validate.cpp`, also
  add it to the priority list in `tools/cli/validator_discovery.cpp`
  so the preflight covers it.

### `pulp doctor --validators` (#743)

Discovers `auval` / `pluginval` / `clap-validator` across well-known
paths (system → cask app bundle → PATH → `~/.cargo/bin`) and verifies
each candidate's code signature is intact. The pure-logic core lives
in `tools/cli/validator_discovery.{hpp,cpp}` (no `cli_common` link
deps — same isolation pattern as `version_diag` / `projects_registry`),
so unit tests in `test/test_cli_validator_discovery.cpp` can stub
`path_exists` / `path_owner_uid` / `assess_signature` without shelling
out to `spctl`.

Signature assessment runs `codesign --verify` first (authoritative
integrity check) and only treats specific verdicts as Broken:

- "invalid resource directory" / "directory or signature have been modified"
- "invalid Info.plist" / "plist or signature have been modified"
- "a sealed resource is missing or invalid"
- "main executable failed strict validation"

Anything else — including unsigned `cargo` binaries and system CLI
tools that `spctl` rejects with "code is valid but does not seem to
be an app" — is treated as Healthy because amfid won't kill them at
launch. The naive `spctl --assess` check the issue spec proposed
catches the failure mode but ALSO false-positives on `auval` and
cargo-installed `clap-validator`; the layered check fixes that.

`--fix` removes broken **user-owned** copies (uid matches `getuid()`).
Broken **root-owned** copies print `sudo rm <path>` and never
auto-elevate — that's the safety boundary the spec is explicit about.

When adding a new validator: extend the `kValidators` array in
`validator_discovery.cpp`, add its priority paths to
`validator_priority_paths()`, and add a unit-test scenario covering
all four states (Healthy, user-owned Broken, root-owned Broken,
Missing).

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
- **Install sibling payloads before replacing `pulp`.** Phase 8+
  archives contain Rust `pulp`, `pulp-cpp`, and the runtime library.
  Pre-cutover C++ CLIs still run `cmd_upgrade.cpp` during that hop, so
  the helper in `upgrade_install.hpp` must copy `pulp-cpp` and other
  top-level payload files next to the current binary before self-replace
  (#1673).
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

#### `pulp scan --no-load` (#812)

Filesystem-only enumeration mode. The default `pulp scan` opens each
discovered bundle via `dlopen` to read entry-point metadata; one
malformed plugin throwing in static-init aborts the whole scan
(`libc++abi: terminating`). `--no-load` lists bundles by path/format
without dlopen — the safe escape hatch when a host plugin crashes
the scanner. `pulp scan --help` short-circuits before plugin
enumeration so users can discover the flag even when the underlying
scan path is broken.

#### Cross-binary `pulp project bump` ↔ `undo` parity (#244)

C++ and Rust both serialize `bump-undo-*.json` records but with
slightly different field sets — Rust writes a transient `notes:[...]`
array that the C++ writer never emits. The C++ JSON parser was
desync-prone on unknown ARRAY/OBJECT values; hardened in commit
`8f29b1fd` to skip them cleanly. When extending the undo schema,
keep both writers + the C++ parser in lockstep, and add a fixture-
level test in `test/test_cli_project_bump.cpp` that round-trips both
directions.

#### `pulp projects list --json` (#244)

The Rust binary always had a `--json` lane; the C++ port for parity
landed via `70e94dd7`. `pulp projects` is the only `projects`
subcommand with `--json` today — `add`/`remove` are write-only and
report success via exit code.

### `pulp coverage diff` — local diff-coverage gate

`pulp coverage diff` (in `tools/cli/cmd_coverage.cpp`) is a thin
shell-out to `tools/scripts/local_diff_cover.sh`. The script is the
single implementation; the CLI subcommand, the
`.claude/commands/coverage-diff.md` slash command, and the pre-push
hook all delegate there. Threshold + filters live in
`tools/scripts/coverage_config.json` — that JSON is the single source
of truth; `.github/workflows/coverage.yml` reads it via `jq` so the
CI gate stays in lockstep. When changing the threshold, edit the JSON
once and don't touch any of the four invocation surfaces. Anti-drift
test: `tools/scripts/test_local_diff_cover.py::WorkflowSourceOfTruthTests`
fails if a future edit hardcodes `--fail-under=NN` back into the
workflow.

## Phase 0 host-contracts touchpoints

`tools/cli/cmd_host.cpp` calls `PluginSlot::process()` which now takes
a `ParameterEventQueue`. When adding new CLI hosting commands, include
`pulp/host/parameter_event_queue.hpp` and pass an empty queue if you
have no automation to deliver. See `docs/reference/host-thread-rules.md`
for the full contract.

## `pulp projects` + registry wiring (#499 / #552 Slice 1b)

`pulp projects list/add/remove` live in `tools/cli/cmd_projects.cpp`.
The JSON file at `~/.pulp/projects.json` (or `$PULP_HOME/projects.json`)
is maintained by `tools/cli/projects_registry.{hpp,cpp}` and is
populated automatically from `cmd_create.cpp` on successful scaffold
via `pulp::cli::projects_registry::add_project(...)`.

Design decision: **registry is authoritative.**
`pulp create` writes to it; `pulp projects add/remove` is the user
surface; `pulp doctor --versions --scan-parents` walks CWD ancestors
as an opt-in diagnostic but never mutates the registry. No silent
disk scans.

Gotchas:

- **`projects_registry` is deliberately decoupled from `cli_common`.**
  Same rule as `version_diag` — the unit test (`pulp-test-cli-
  projects-registry`) links just `projects_registry.cpp` +
  Catch2 so the test binary stays small. Don't reach into
  `cli_common.hpp` from inside the registry module.
- **`add_project` dedupes by canonical path, not by string.** The
  canonicalish helper resolves symlinks via
  `fs::weakly_canonical`; `/tmp/...` and `/private/tmp/...` land at
  the same canonical form on macOS. Registry round-trip tests must
  canonicalise the `TempDir.path` up front or `REQUIRE(x == y)`
  comparisons will break.
- **Missing-on-disk entries are kept, not pruned.** Stale-entry
  policy (from the design doc): the entry is flagged with a
  `(missing)` line and a copy-paste `pulp projects remove <path>`
  hint — never auto-removed. Only explicit `pulp projects remove`
  (or a manual JSON edit) mutates the registry.
- **Nested parent + child both appear under `--scan-parents`.** The
  scan returns deepest-first. The caller (`cmd_doctor`) dedupes
  against the active project but keeps both ancestors if both
  contain a `pulp_add_*` macro. The user resolves the ambiguity —
  we surface both rather than picking arbitrarily.
- **`--scan-parents` reads `CMakeLists.txt` with a simple regex**
  (`\bpulp_add_[A-Za-z0-9_]+\s*\(`). Matches any `pulp_add_*` macro
  the SDK introduces without requiring a new entry here.

## `pulp project bump` / `pulp project undo` (#499 / #564 Slice 7)

`pulp project bump` and `pulp project undo` live in
`tools/cli/cmd_project.cpp` and delegate to the pure-logic core in
`tools/cli/project_bump.{hpp,cpp}`. Behavior summary:

- In standalone SDK-mode projects (`pulp.toml` without Pulp's `core/`
  source tree), bump treats `pulp.toml` `sdk_version` as the SDK pin,
  rewrites it together with the versioned `find_package(Pulp X.Y.Z ...)`
  call, and leaves `project(NAME VERSION ...)` alone as the app/plugin
  product version.
- In legacy source-embedded projects, bump reads `CMakeLists.txt`,
  locates the first Pulp pin (FetchContent GIT_TAG,
  `pulp_add_project(VERSION ...)`, or `project(NAME VERSION ...)`),
  rewrites it atomically, records an undo batch, and prints Slice 3
  (#548) migration notes for the hop.
- `--all` iterates `~/.pulp/projects.json` (Slice 1b #552).
- Undo reads `bump-undo-<timestamp>.json` and reverts each bumped
  entry's recorded edits. New undo files may contain multiple edits
  across `pulp.toml` and `CMakeLists.txt`; legacy one-edit files are
  still parsed.

Gotchas:

- **Singular `project` command, not `projects`.** Registry commands
  (`pulp projects list/add/remove`) are plural; per-project pin
  management is singular. Both exist side-by-side in the dispatch
  table — keep them separate in help text and docs.
- **Undo filenames replace `:` with `-`.** ISO-8601 stamps contain
  colons which are illegal on Windows. `undo_batch_path()` maps
  `2026-04-21T14:30:00Z` → `bump-undo-2026-04-21T14-30-00Z.json`.
  Tests pin this; don't "fix" it on POSIX without updating Windows.
- **Dynamic pins (branches, SHAs) are skipped, not failed.**
  `refuse_dynamic_pin()` returns true for anything that isn't
  semver-after-optional-`v`. Status ends up as `"skipped"` with a
  human-readable reason in `failure_reason`. Do not rewrite these.
- **Do not bump the Pulp source checkout with this command.**
  `pulp project bump` is for consumer projects. From the Pulp source
  tree, use `pulp version bump` and the normal release/PR workflow.
- **Standalone mode's source of truth is `pulp.toml` `sdk_version`.**
  If a standalone project has `project(NAME VERSION ...)`, that is the
  app/plugin version and must not be interpreted as the SDK version.
  Keep `pulp.toml` and `find_package(Pulp X.Y.Z ...)` in lockstep.
- **`sdk_path` is rewritten only when it points at a managed Pulp cache.**
  Custom paths are preserved and reported; `pulp build` verifies
  whether they satisfy the requested `sdk_version`.
- **`--allow-cli-skew` is the explicit escape hatch.** By default,
  target SDK versions newer than the installed CLI fail fast and tell
  the user to run `pulp upgrade` first.
- **`--allow-redundant` bypasses the origin/main guard.** `pulp project
  bump` fetches `origin main` best-effort and refuses by default when
  main already pins the target-or-newer SDK. Fetch failures fail open
  so offline users aren't blocked by stale refs. In standalone
  projects, still compare local `pulp.toml` / `find_package(Pulp ...)`
  lockstep before firing the guard: if `sdk_version` already matches
  the target but `find_package` is stale, the command should repair the
  local drift instead of forcing `--allow-redundant`.
- **Managed `sdk_path` rewrites are only real edits when the path
  changes.** If a standalone project already targets the requested SDK
  and `sdk_path` already points at that managed cache, the command must
  return "already at target version" rather than staging a no-op
  rewrite just because the path is managed.
- **`project_bump` is decoupled from `cli_common`.** Same rule as
  `projects_registry` / `update_mode` — the unit test binaries link
  just the module + Catch2. Don't reach into `cli_common.hpp` from
  `project_bump.cpp`.
- **Catch2 test names can't contain `--flag`.** The Catch2 runner
  parses `--foo` as a CLI flag even inside a string argument, so
  ctest's Catch2 invocation fails with "unrecognised token". Rename
  bump-all test cases to "bump all ..." instead of "--all ...".
- **Post-upgrade hook respects `update.bump_projects`.**
  `cmd_upgrade.cpp` reads the key (default `prompt`) and either
  prints the `pulp project bump --all` hint or stays quiet on
  `off`. The actual exec-into-new-binary lands in a follow-up; this
  slice just wires the nudge.
- **Git-clean gate uses `git -C <proj> status --porcelain`.** If
  git isn't on PATH, `cmake_is_dirty()` returns false — we refuse
  to block on a missing tool. `--force-dirty` is the explicit
  override for users who WANT to bump alongside other edits.
- **Migration notes print once per bump batch, using the minimum
  old pin as `from`.** That captures the widest set of applicable
  notes when different projects were on different versions.

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
- **Execution commands can be stricter than `doctor --versions`.**
  `pulp build` / `pulp dev` may reuse the same semver parsing for a
  hard preflight while `doctor --versions` itself remains advisory.
  Keep that split explicit: diagnostics stay 0-exit, execution paths
  decide whether to block.
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
- **Plugin `min_cli_version` mirrors the same semantics.** A plugin
  `plugin.json` with no `min_cli_version` skips the
  check silently; a newer `min_cli_version` emits the same advisory
  WARN finding in `analyze()` and an inline "needs CLI: >= vX.Y.Z"
  line under the Plugin entry in the human report. The JSON surface
  adds a `plugin_min_cli` top-level field alongside `cli` and
  `plugin` — do not rename it; `tools/scripts/cli_version_check.sh`
  and the `upgrade` skill parse it by key.

The diagnostic also supports `--scan-parents` and `--json` plus the
`~/.pulp/projects.json` registry — see the section above for registry
gotchas. The `--versions` core remains pure-logic and scoped to
`version_diag.{hpp,cpp}`.

Keep follow-up version-diagnostic work independently reviewable: update
checks, migration docs, `/upgrade` skill changes, mode enforcement, and
plugin ↔ CLI skew fields (`min_cli_version`, `plugin_min_cli`, and
`tools/scripts/cli_version_check.sh`) should land as focused issues and
PRs.

### `pulp doctor` — `pulp-mcp` advisory row



`run_doctor_checks()` in `tools/cli/cli_common.cpp` ends with an
**optional** check for the Claude Code plugin's MCP server binary. The
check is purely advisory (never gates the doctor exit code) and resolves
the binary in this order:

1. `pulp-mcp` on `$PATH` (the steady-state after `curl install.sh | sh`).
2. `~/.pulp/bin/pulp-mcp` (the install location, in case `$PATH` didn't
   reload yet in this shell).
3. `<repo_root>/build/tools/mcp/pulp-mcp` (source builds).

When found, the row prints the binary's own `--version` output alongside
`PULP_SDK_VERSION` so users see drift between an old installed
`pulp-mcp` and a newer CLI. **Never escalate this to a hard gate** — the
plugin must remain tolerant of older installed `pulp-mcp` so users on an
older project SDK aren't blocked. Cross-version compatibility belongs in
per-tool feature detection inside `pulp-mcp` itself.

`pulp-mcp` accepts `--version` / `-V` and `--help` / `-h` short-circuit
flags before entering its JSON-RPC stdin loop (`tools/mcp/pulp_mcp.cpp
main()`). Both flags exit cleanly without consuming stdin so the
release-CLI smoke gate and `pulp doctor` can probe the binary without
speaking MCP framing. **Anything new that needs stdin must NOT short-
circuit here** — fall through to the main loop instead.

`serverInfo.version` returned from the MCP `initialize` method is wired
to `PROJECT_VERSION` via `tools/mcp/pulp_mcp_version.h.in`. Hardcoding a
new version string there will be silently overridden by the configure
step; bump the project version instead.

## `pulp doctor --caches` — FetchContent cache health (#744)

Like `--versions`, `--caches` is a dedicated diagnostic that
short-circuits the doctor pipeline. It scans Pulp's shared FetchContent
source cache (the one `pulp_register_fetchcontent_source` populates) and
classifies each entry as healthy, dangling-symlink, stale-commit, or
root-owned. Lives in `tools/cli/fetchcontent_cache.{hpp,cpp}` with
`cmd_doctor` and the `cache_preflight_check` helper in
`cli_common.cpp` as the only callers.

Gotchas:

- **Same `cli_common`-decoupling rule as `version_diag`.**
  `fetchcontent_cache.cpp` reimplements `user_home_dir_local` and a
  small env-string helper so the unit-test binary
  (`test/test_cli_fetchcontent_cache.cpp`) can link only
  `fetchcontent_cache.cpp` and Catch2 — no pulp::runtime, no
  cli_common. If you add a helper, keep it local unless you check the
  test impact first.
- **DiscoveryEnv is the unit-test seam.** All filesystem access goes
  through `DiscoveryEnv::lstat` / `stat_follow` / `list_dir` callables
  set up by `make_real_env`. Tests construct their own env with
  deterministic mock callables — they never touch
  `~/Library/Caches/Pulp/`. Adding new classification logic? Put the
  syscalls behind the env so the new behaviour can be covered without
  prepping a fixture cache on the dev's machine.
- **FetchContent scratch dirs (`<dep>-src`/`-build`/`-subbuild`)**
  always coexist with the populated source dir we created. They are
  CMake's own working state, not entries we own. Discovery treats any
  trailing segment of `src`, `build`, or `subbuild` as scratch and
  classifies them Healthy regardless of declared REF — clobbering them
  defeats the configure-time cache. The
  `discover: FetchContent scratch dirs do not register as stale-commit`
  test pins this behaviour.
- **`--fix` only touches user-owned entries.** Root-owned classification
  is the safety gate: an entry whose POSIX uid is not `geteuid()` gets
  `RootOwned` even when the underlying state would otherwise be
  Dangling or StaleCommit, and `apply_fixes` skips it. Agents must
  never silently elevate; the user has to run `sudo rm` themselves.
  On Windows, ownership is treated as user-writable (POSIX uid is a
  no-op concept on the platform that hosts the cache under the user's
  profile by definition).
- **Stale-commit needs both a declared REF AND a cached REF.** When
  `pulp_register_fetchcontent_source` is called without a `REF`
  argument (some optional deps are floated by branch), the entry is
  classified Healthy regardless of directory contents. Adding a new
  cache classification? Mirror this guard so missing-data inputs
  don't fire false positives.
- **Comments in `CMakeLists.txt` are stripped before the regex pass.**
  The `parse_declared_refs_from_text` helper does a line-by-line
  first-`#` cut before scanning. A commented-out
  `pulp_register_fetchcontent_source(foo REF bar)` does NOT pollute
  the declared-refs map, which keeps stale-commit detection honest
  for example/documentation snippets in CMakeLists.
- **Preflight is gated on `needs_configure`.** `pulp build` only runs
  the preflight when CMake will reconfigure; incremental rebuilds
  skip it (the cache scan is cheap but the gate matters more — a
  healthy cache that fits a stale `CMakeCache.txt` would surface the
  bad path eventually but not on the cheap incremental loop).
  `pulp test` mirrors the rule: preflight only runs when a cold-start
  build is needed (no `CMakeCache.txt`).
- **`PULP_SKIP_CACHE_PREFLIGHT=1` bypasses the preflight, NOT the
  doctor command.** Set it in CI environments that intentionally
  curate the cache and would rather see CMake's native error than
  the early gate. `pulp doctor --caches` always runs its scan
  regardless — the diagnostic is the diagnostic.
- **JSON shape is committed.** `pulp doctor --caches --json` prints
  `{"cache_root", "healthy", "entries": [{"name", "path", "status",
  "is_symlink", "resolved_target", "declared_ref", "cached_ref",
  "dep_name", "reason", "remediation", "fixable"}]}`. `status`
  values are `healthy`, `dangling-symlink`, `stale-commit`,
  `root-owned`, `unknown`. Don't rename keys; scripts and the
  rust-cli port (#740 family) parse them by name.

Adjacent modules to coordinate with: `tools/cmake/PulpFetchContent.cmake`
(the cache-root and sanitize-suffix logic — `default_cache_root` and
`sanitize_ref` in `fetchcontent_cache.cpp` MUST mirror it), and the
build / test commands that call `cache_preflight_check`.

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

- **Anonymous GitHub API only.** 60 req/hr/IP. The 24h cache default
  keeps us well under that. Do NOT add authenticated fetches — the
  design is explicit about "no GitHub App, no auth".
- **Background refresh is a detached `std::thread`.** The original
  `std::async` + static-future pattern can block on destructor; the
  current `std::thread(...).detach()` is correct. Do NOT regress this
  back to `std::async` without understanding the Windows CRT
  finalization path — the process must be able to exit while the fetch
  thread is still in-flight.
- **Cache file is atomic via `.tmp` + rename.** A torn write just
  forces a re-fetch on the next invocation, not corruption. Cross-device
  rename falls back to `copy_file` + `remove`.
- **Commit trailer block must be contiguous.** Version-bump + skill
  trailers live on the tip commit. Do NOT split with blank lines —
  `git interpret-trailers --parse` treats them as non-trailers.

## Mode enforcement + pending-upgrade (#499 Slice 5 / #550)

Slice 5 wires all four `update.mode` values into the dispatch path in
`pulp_cli.cpp` and adds the auto-mode staging + Windows tombstone
cleanup. Key layout:

- **`tools/cli/update_mode.{hpp,cpp}`** — pure-logic core: `Mode` enum,
  snooze read/write, pending-upgrade JSON round-trip, tombstone path
  helpers, mode-specific banner composers, decision helpers
  (`decide_prompt_banner`, `should_stage_auto_download`). No
  `cli_common` link dep — same standalone-test pattern as
  `update_check`. Unit tests in
  `test/test_cli_update_mode.cpp` mock the filesystem via per-test
  tmpdirs and inject time via explicit epoch-seconds arguments.
- **`tools/cli/pulp_cli.cpp`** — `maybe_emit_update_banner_and_refresh`
  now consumes `update_mode`. Decision tree:
  - `off` → zero I/O, zero network, zero banner.
  - `prompt` → one-shot banner per new version (Slice 2 behavior,
    preserved); 24h snooze via `~/.pulp/update-snooze` when it's set
    (writes happen from `cmd_config` on mode-change and from the
    `/upgrade` Claude skill on user decline — the dispatch path
    never writes the snooze itself, it only reads it).
  - `manual` → one-liner per new version ("Run `pulp upgrade` when
    you're ready."), suppressed after `banner_shown_for_version`
    matches.
  - `auto` → writes `~/.pulp/pending-upgrade`, prints "downloaded,
    will complete on next invocation". The actual binary swap lives
    in `cmd_upgrade` — Slice 5 does NOT download in the background
    thread, only stages intent via the marker file. This preserves
    Section G's "no binary is ever replaced without the user's
    session touching `pulp` again".
- **`tools/cli/cmd_config.cpp`** — `pulp config set update.mode ...`
  now clears `~/.pulp/update-snooze` as a side effect. Reason: a mode
  change is itself an act of re-engagement with update management,
  so an existing 24h snooze would otherwise silence the new mode's
  behavior. Also adds the `update.bump_projects` allow-list entry
  (values: `prompt | auto | off`, default `prompt`) as a **reserved
  stub for Slice 7 (#564)** — accept it now, implement in Slice 7.
- **Banner shapes** (locked, tested verbatim in `test_cli_update_mode.cpp`):
  - manual: `Pulp vX.Y.Z available (you have vA.B.C). Run \`pulp upgrade\` when you're ready.`
  - auto staged: `Pulp vX.Y.Z downloaded. The upgrade will complete on your next \`pulp\` invocation.`
  - auto completed: `Pulp CLI upgraded to vX.Y.Z. Run \`pulp upgrade --notes\` to see what changed.`

Gotchas specific to Slice 5:

- **Windows tombstone pattern (`*.pulp.old`).** The swap in
  `cmd_upgrade` on Windows can't overwrite a file-locked running exe
  in place. The rustup/pip/Python pattern is: `MoveFileEx(exe,
  exe.pulp.old, REPLACE_EXISTING)` (rename-out the old bytes),
  then copy the new binary into the original path. On the NEXT
  invocation of the new binary, `cleanup_tombstone()` deletes the
  `.pulp.old` file. macOS/Linux overwrite the running inode fine —
  `cleanup_tombstone()` is a no-op there. Always call cleanup from
  the dispatch hook (top of `main`) so the sweep is universal.
- **Never replace the binary mid-command.** Design Section G is
  explicit: the auto-mode dispatch path only stages — it must not
  call `cmd_upgrade` directly or start a download that can race
  `main()`'s exit. The staging path here is intentionally a
  marker-write + user-facing notice, not a network fetch.
- **`decide_prompt_banner` is pure.** Never call it from the snooze
  write path — it'd double-count the banner against the
  `banner_shown_for_version` counter. The snooze file is written by
  (a) `cmd_config` on mode change (as a clear) and (b) the
  `/upgrade` Claude skill on explicit decline. Nowhere else.
- **`update.bump_projects` is an accept-only stub in Slice 5.**
  Don't wire behavior for it yet; Slice 7 (#564) owns that. If you're
  tempted to check the value here, stop — it should round-trip
  through `pulp config get/set` only.

## Migration notes + `pulp upgrade --notes` (#499 Slice 3 / #548)

Slice 3 extends `cmd_upgrade` with an embedded, per-release migration
index. The table is generated at CMake configure time from
`docs/migrations/*.md` and compiled into the binary so upgrade notes
are always in lock-step with the shipped version — no runtime
download, no filesystem scan.

Key layout:

- **`docs/migrations/vX.Y.Z.md`** — one file per release with notes
  worth surfacing. TOML frontmatter fields: `version` (required),
  `breaking` (bool), `applies_if` (expression string), `summary`.
  `docs/migrations/README.md` is the schema reference; the codegen
  skips it. Only write a note when a reasonable pro developer needs
  to change code / config / habits — not every PR.
- **`tools/scripts/build_migration_index.py`** — standalone Python
  codegen. Parses the TOML frontmatter, escapes the Markdown body for
  a C++ string literal, sorts entries by parsed semver, and writes
  `tools/cli/generated/migration_index.cpp`. Runs from
  `tools/cli/CMakeLists.txt` via `add_custom_command`; also invokable
  directly for iteration.
- **`tools/cli/migration_index.hpp`** — schema (`MigrationEntry`),
  `EvalContext`, `evaluate_applies_if`, `entries_for_hop`,
  `applicable_entries`, `render_notes_text`, `render_notes_json`.
  Link-free from `cli_common` (same pattern as `version_diag` /
  `update_check`).
- **`tools/cli/migration_runtime.cpp`** — evaluator + renderers. The
  generated `migration_index.cpp` defines only the data table so
  `migration_runtime.cpp` is the single TU unit tests build against
  (the test provides its own stub `kMigrationIndex`).
- **`pulp upgrade --notes [--json] [--from X --to Y]`** — new flag.
  No network, no binary swap. `--json` is stable-shape for Slice 4
  (`/upgrade` Claude skill); do NOT rename the keys (`from`, `to`,
  `entries[].version|breaking|summary|applies_if|body`) without
  bumping the skill.

`applies_if` grammar:

```
expr    := or
or      := and ("||" and)*
and     := cmp ("&&" cmp)*
cmp     := ident op version | "(" expr ")"
ident   := "cli_version_from" | "cli_version_to"
op      := "<" | "<=" | ">" | ">=" | "==" | "!="
```

Fail-closed semantics: unknown idents, malformed expressions, or
unparseable context versions all evaluate to false (so a bad note
doesn't noise the output). Empty expression matches every hop.

Gotchas:

- **Version literals in `applies_if` have optional `v` prefix, but
  the lexer requires `v` to be followed by a digit.** `vnull` is
  NOT a version; it's an identifier (and fails closed). If you need
  a plain `v`-prefixed literal, make sure the next char is `0-9`.
- **`from` is exclusive, `to` is inclusive.** `entries_for_hop(A, B)`
  returns entries with version strictly `> A` and `<= B`. Stepping
  `--from 0.27.0 --to 0.27.0` returns zero entries. This is
  intentional — a no-op hop prints "No migration notes apply."
- **`docs/migrations/README.md` is excluded from the index.** It's
  the schema reference, not a migration note. Don't rename it without
  updating `build_migration_index.py`.
- **Generated `.cpp` lives under `${CMAKE_BINARY_DIR}/` — it is NOT
  checked in.** The source of truth is `docs/migrations/*.md`.
- **Duplicate `version = "X.Y.Z"` across two files is a hard error.**
  The codegen exits 2 and CMake fails to configure. Rename one.
- **MSVC transitive-include hygiene.** `migration_runtime.cpp`
  explicitly `#include <cstddef>`, `<sstream>`, `<string>`, `<vector>`,
  `<tuple>`. Don't rely on libc++ giving you those transitively.

## SDK cache filenames — version pin (pulp #1814)

`pulp install` (and the underlying `pulp cache fetch skia`) used to write
the downloaded SDK tarball to `~/.pulp/cache/pulp-sdk-<platform>.tar.gz` —
unversioned. The cache-hit path checked only `fs::exists()`, so a stale
tarball from a prior release would silently shadow a fresh download and
the CLI would "install" yesterday's SDK without warning. The fix pins the
SDK version into the filename:

```
~/.pulp/cache/pulp-sdk-v0.92.0-darwin-arm64.tar.gz
```

Plumbing:

- `tools/cli/sdk_cache_paths.{hpp,cpp}` — two helpers,
  `sdk_tarball_filename` and `legacy_unversioned_sdk_tarball_filename`.
  Standalone TU (no `cli_common` deps) so the unit test
  `test/test_cli_sdk_tarball_filename.cpp` can link them without dragging
  in pulp::runtime / pulp::platform. Same pattern as `version_diag` and
  `fetchcontent_cache`.
- `tools/cli/cmd_misc.cpp` `cmd_cache` `fetch skia` branch — calls
  `pulp::cli::sdk_tarball_filename(PULP_SDK_VERSION, platform)` for the
  local cache filename. GitHub release assets are still uploaded with the
  unversioned filename (the version lives in the URL path segment), so
  the remote URL uses `legacy_unversioned_sdk_tarball_filename` only as
  a *filename component of the URL*, not as a local cache key.
- Best-effort legacy cleanup — if `~/.pulp/cache/pulp-sdk-<platform>.tar.gz`
  exists from a pre-#1814 install, the fetch path removes it before
  resolving the versioned filename, so stale tarballs don't sit in
  `~/.pulp/cache/` forever. `std::error_code` is used to skip silently
  on permission / mount errors.
- User-facing output — both "fresh download" and "cache hit" paths now
  print the SDK version explicitly: `SDK v0.92.0 (includes Skia) already
  cached at …` / `Downloading SDK v0.92.0 (Skia binaries for …)`.

Gotchas:

- **Don't compare the local filename against the URL filename.** They're
  intentionally different — local pins the version (so cache lookup
  misses on stale tarballs), URL doesn't (because that's the GitHub
  release-asset shape). The version-pin contract is between cache lookup
  and cache write, not against the remote.
- **`pulp install` is the user-facing alias.** It calls
  `cmd_cache({"fetch", "skia"})` internally — fixing `cache fetch` fixes
  `install` automatically.
- **`pulp sdk install` is *not* the same path.** That's the Rust CLI's
  fall-through to `pulp-cpp sdk install` (see `pulp-rs/src/cmd/sdk.rs`);
  the C++ `cmd_sdk` is a different code path with its own per-version
  `~/.pulp/sdk/<version>/` layout. #1814 fix lives only in `cmd_cache`.
- **If you bump `PULP_SDK_VERSION`, you do NOT need to manually
  invalidate `~/.pulp/cache`.** The new version means the filename
  misses on the old cache and the user gets a fresh download
  automatically.
- **`pulp create` defaults to `CMAKE_BUILD_TYPE=Release`; `--debug` opts in** (2026-05). Was Debug-default historically, which silently produced ~5-10× slower binaries because `pulp build`'s incremental configure doesn't re-pass `-DCMAKE_BUILD_TYPE` (so the create-time value sticks for the project's lifetime). Two call sites in `cmd_create.cpp` both honor the flag — keep them in lockstep when adding new configure paths. Intentionally NOT mirrored:
    - `cmd_project.cpp`'s `bump --verify-build` keeps Debug — throwaway verify build, faster compile, binary isn't run.
    - `validate-build.sh` keeps Debug — that's the validator's job (catches debug-only assertion failures).
    - `cli_common.cpp`'s SDK install path already used Release.
  Follow-up scope (not done yet): `pulp build --debug` / `--release` that force a reconfigure of an existing `build/` directory.
- **`pulp sdk available` + newer-SDK banner cache contract** (2026-05, #22/#23). `pulp sdk available` shells out to curl for the GitHub `/releases?per_page=30` endpoint and parses `tag_name` entries — no JSON dep. `maybe_print_newer_sdk_banner(installed)` caches the latest release at `~/.pulp/cache/latest_release.txt` (line 1 = version, line 2 = Unix timestamp) with a 24h TTL and a 2s curl timeout. The cache is best-effort — if curl fails the banner just doesn't print this run. Wired into `pulp sdk status` only; `pulp build` and `pulp create` deliberately stay quiet on the hot path. To invalidate the cache: `rm ~/.pulp/cache/latest_release.txt`.
- **`tools/cli/cli_doctor_helpers.cpp` owns doctor check bodies** (2026-05, R2-4). The 971-line doctor block moved out of `cli_common.cpp` into its own TU. Public API (`DoctorCheck` struct + `run_doctor_*` functions) stays in `cli_common.hpp`; no new public header was added (Codex risk callout: don't replace one catch-all surface with another). When adding a new doctor check, edit `cli_doctor_helpers.cpp` only.
- **`pulp doctor list` + `--only <name>`** (2026-05, R2-8). `pulp doctor list` enumerates available checks; `pulp doctor --only <substring>` case-insensitive filter runs a subset. Works across modes (`pulp doctor android list`, `pulp doctor --only emulator`). Filter logic lives in `cmd_doctor.cpp`, not in the helpers TU — the `DoctorCheck` vector returned by `run_doctor_checks` already IS the registry. No new struct or registration step needed when adding a check.
- **Validator commands must suppress editor hosts.** Any CLI path that shells out to `auval`, `pluginval`, `clap-validator`, or `vstvalidator` must run the command with `PULP_DISABLE_PLUGIN_EDITOR=1 PULP_HEADLESS=1 PULP_TEST_MODE=1`. This is part of the launch-safety contract: validation should never open a native plugin editor or OS window on a user/agent machine.

## Build type — repo/example builds default to Release (perf)

`ensure_repo_build_configured` (`cli_common.cpp`, the `pulp build` configure
path for repo/example plugins) now passes `-DCMAKE_BUILD_TYPE=Release` on a
fresh configure. Previously it passed NO build type, so CMake configured with
no optimization (no `-O`, no `NDEBUG`) — an unoptimized binary whose plugin
editor/DSP feels sluggish in a DAW for the same reason a Debug build does
(confirmed 2026-05-22: ChainerSynth AU laggy unoptimized, "super snappy" in
Release). `pulp create` already defaults Release with a `--debug` opt-in.

Notes for CLI maintenance:
- Override per-build with `PULP_BUILD_TYPE=Debug pulp build` (read in
  `ensure_repo_build_configured`); it only applies on a FRESH configure — an
  existing `CMakeCache.txt` build type is left untouched (re-configure or wipe
  `build/` to change it). `pulp build` prints `Build type: <type>`.
- The `PULP_BUILD_TYPE` value is `shell_quote`'d before it goes into the cmake
  command (the configure runs via the shell), like every other interpolated
  value there. Never concatenate a raw env value into a shell command string —
  a value like `Release; rm -rf ~` would execute.
- Don't reintroduce an empty/Debug default for plugin builds — plugins are
  perf-tested in a host, so unoptimized is the wrong default.
- Companion runtime signal: `decide_gpu_host` (core/format/gpu_host_select.hpp)
  logs `build=debug|release` on the `[plugin-gpu-host]` adapter line and warns
  once on Debug, so a host log immediately shows which build was loaded.

## `pulp build --install` + `--skip-validation` (PR #2932, items 7.4 / 7.4b / 7.5)

The documented `build → validate → install` pipeline now exists as
real CLI flags rather than implied tooling. `tools/cli/cmd_build.cpp`
gained:

- `--install` — after a successful build, copy each format bundle to
  its per-format system folder (`~/Library/Audio/Plug-Ins/Components`
  for AU, `~/Library/Audio/Plug-Ins/VST3/` for VST3,
  `~/Library/Audio/Plug-Ins/CLAP/` for CLAP). The install runs the
  `cmd_validate(["--strict"])` gate first; a failing validator
  refuses to copy anything to the user's system tree.
- `--skip-validation` — debug-only escape hatch for adapter work. It
  is **only valid when paired with `--install`** (using it alone is
  rejected at parse time so users can't accidentally disable
  validation on a normal build). `--install + --watch` is also
  rejected — repeated installs from a watch loop would race the host.

Implementation lives in `tools/cli/install_paths_mac.{hpp,cpp}` and is
mediated through an `InstallEnv` interface so the 14 Catch2 cases in
`pulp-test-cli-install-paths-mac` can verify the mkdir → rm → cp
ordering, idempotency, and failure cases without writing to a real
`~/Library` tree. CLI-surface contract is pinned by 2 shellout cases
in `pulp-test-cli-ship-shellout`.

Gotchas:

- **rm comes before cp**, not the other way around. A leftover bundle
  with stale Info.plist can confuse the AU registrar even after a
  fresh copy. Tests pin this order.
- **Relative `$HOME` is rejected.** The destination resolver refuses
  any path that's not absolute under `$HOME/Library/Audio/Plug-Ins/`.
- **Plugin Install Policy applies.** See CLAUDE.md "Plugin Install
  Policy" — a plugin that crashes a DAW during scan is worse than no
  plugin at all. The validation gate is non-optional in normal use.

## `pulp ship auv3-xcodeproj` (PR #2938, item 3.10)

Thin wrapper over `cmake -G Xcode` that generates an Xcode project
for an AUv3 target without disturbing the user's regular Ninja /
Makefile build dir. Surface:

```bash
pulp ship auv3-xcodeproj <target>                    # iphonesimulator (default)
pulp ship auv3-xcodeproj <target> --sdk iphoneos     # device
pulp ship auv3-xcodeproj <target> --sdk macosx       # macOS lane
pulp ship auv3-xcodeproj <target> --output build/xcode/MyPlugin
pulp ship auv3-xcodeproj <target> --open             # open in Xcode after gen
pulp ship auv3-xcodeproj <target> --dry-run          # print cmake invocation only
```

Default output dir is `build/xcode/<target>-<sdk>`. iOS SDKs include
`tools/cmake/ios.toolchain.cmake` with the correct `IOS_PLATFORM`
(OS for device, SIMULATOR64 for simulator). The full Xcode-project
generation flow plus device entitlement templates live in the `auv3`
skill — this section just pins the CLI surface so the slash command,
docs, and skill stay in lockstep.

## `pulp identity` — committed plugin identity lockfile

Track 3.12 (macOS plugin-authoring plan) introduced a Rust-side surface
for managing `.pulp/identity.lock`, the committed pin of each plugin's
host-visible identity (AU 4CC + manufacturer code, AAX product code,
optional VST3 FUID, optional CLAP id, version). Lives in
`experimental/pulp-rs/src/cmd/identity.rs`; schema doc at
`docs/reference/identity-lock.md`.

Surface:

- `pulp identity record [--allow-identity-change] [--dry-run]` —
  refresh the lockfile. Refuses to overwrite drifted entries without
  `--allow-identity-change` (the audit-trail flag a reviewer can grep
  in the commit message).
- `pulp identity check [--allow-identity-change]` — pure read; exit
  1 on drift, 0 with `--allow-identity-change`. CI-friendly.
- `pulp build --check-identity [--allow-identity-change]` — runs the
  check before the configure step so a drifted PR fails the whole
  build, not just a downstream gate. Wired in
  `cmd::orchestrate::build_with`.

Gotchas (lessons from the slice):

- **Empty recorded fields are NOT drift.** `vst3_fuid` and
  `clap_plugin_id` are optional today because the recorder doesn't yet
  scrape `Steinberg::FUID(...)` literals out of source files. The diff
  treats `recorded == ""` as "not yet pinned" — only non-empty
  recorded values that no longer match the current source trigger a
  failure. A future slice can teach `parse_plugins_from_text` to walk
  source-side identity declarations once the regex shape is locked in.
- **Single-file CMake scan.** `parse_plugins_from_cmake` reads the
  project's top-level `CMakeLists.txt`. Nested `pulp_add_plugin(...)`
  invocations under `add_subdirectory(...)` are out of scope today —
  add a CMake-tree-walker if real projects start hiding plugin
  declarations in nested files.
- **Lock file is reviewable.** TOML output is sorted by `target` and
  uses `to_string_pretty`, so `git diff .pulp/identity.lock` is
  always linear. Don't switch to `to_string` (compact) — the on-disk
  layout being readable is the whole point.
- **Two flag sites, same semantics.** `--allow-identity-change` works
  on both `pulp identity {record,check}` and `pulp build
  --check-identity`. Keep them in lockstep — the orchestrate path
  re-uses `cmd::identity::run(Check)` rather than re-implementing the
  comparison.

## Standalone host transport + MIDI + persistence (item 3.5)

`StandaloneApp` is the user-facing surface behind `pulp run`. As of
the 3.5 wiring it consumes three Pulp subsystems the way a DAW would
consume the equivalent host APIs:

- **`pulp::midi::MidiMessageCollector` (item 1.9)** — `ui_midi_collector()`
  is the lock-free SPSC path for UI / virtual-keyboard / scripting
  MIDI. The audio callback drains it into each block's `MidiBuffer`
  at the correct sample offsets via `drain_into(block_start_seconds,
  ctx.buffer_size, ctx.sample_rate)`. Hardware MIDI still goes
  through the mutex-guarded `pending_midi_` accumulator on the
  input-thread callback.
- **Built-in transport (item 1.3)** — `StandaloneConfig` carries
  `tempo_bpm`, `time_sig_numerator`, `time_sig_denominator`, and
  `transport_playing`. The audio callback populates
  `ProcessContext::tempo_bpm`, `position_beats`, `bar`, and
  `is_playing` from those values; `position_samples` advances on a
  rolling atomic counter so plugins read a monotonic timeline.
- **`pulp::state::ApplicationProperties` (item 1.2)** —
  `StandaloneApp::{save,load}_persisted_config(app_name)` round-trip
  the entire `StandaloneConfig` through the platform user-properties
  file. Keys are namespaced under `standalone.*` so they don't collide
  with plugin-owned state. Empty `app_name` is the documented
  "persistence disabled" sentinel — callers can opt out by passing
  `""`.

When extending the standalone surface (new `StandaloneConfig` field,
new transport field, new persisted key), update **both**:

1. The audio callback's `ProcessContext` population in
   `core/format/src/standalone.cpp` (so plugins see the new value
   immediately).
2. `save_persisted_config` / `load_persisted_config` (so the value
   survives a `pulp run` restart).

Test coverage lives in `test/test_standalone_transport_midi.cpp` —
add a case there when you extend either surface.
