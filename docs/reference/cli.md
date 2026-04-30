# CLI Reference

The `pulp` CLI wraps common build, test, validation, and shipping operations.

Source: `tools/cli/pulp_cli.cpp`

## Commands

### create

**Status**: usable

Create a new plugin project from templates. Checks environment, scaffolds source files, configures the project, builds the generated test plus the default platform outputs, and runs tests.

Default behavior is product-first:
- **SDK mode** (default for external projects): uses `find_package(Pulp)`, generates `pulp.toml`, and either reuses a checkout-built local SDK or downloads a cached SDK release when no local checkout hint is available
- **Source-tree mode** (`--in-tree` / `--example`): uses local source and adds the project to `examples/`

```bash
pulp create "My Gain"                              # effect plugin (default)
pulp create "My Synth" --type instrument           # instrument plugin
pulp create "My App" --type app                    # standalone audio application
pulp create "My Project" --type bare               # minimal skeleton
pulp create "My FX" --manufacturer "Acme Audio"    # custom manufacturer
pulp create "My FX" --output ~/projects/my-fx      # custom output directory
pulp create "Debug Knob" --in-tree                 # add an example under examples/
pulp create "My FX" --no-build                     # scaffold only, skip build
pulp create "My FX" --no-interactive               # CI/scripting mode (no prompts)
```

Available types: `effect` (default), `instrument`, `app`, `bare`. Templates are in `tools/templates/<type>/`.

Default output location resolution:
1. `--output <dir>`
2. `PULP_PROJECTS_DIR`
3. `~/.pulp/config.toml` with:

```toml
[create]
projects_dir = "~/Code/PulpProjects"
```

4. If run inside the Pulp repo: create the project next to the repo root
5. Otherwise: create the project in `./<name>`

Set `PULP_HOME` to move the default `~/.pulp/` home used for SDK/cache/config storage.

This flow is meant to behave the same from a normal terminal, CI, or agent-driven workflows. The CLI prints which mode it selected and why, while `--no-interactive`, `PULP_PROJECTS_DIR`, and `PULP_HOME` make project creation predictable for automation.

Mode truth:
- **SDK mode** means you are in an external project building against a pinned installed Pulp SDK artifact.
- **Source-tree mode** means you are inside the Pulp checkout building the repo or its examples against live source.
- `pulp create` defaults to SDK mode unless you explicitly ask for an in-tree example with `--in-tree`.

What it does:
1. Runs `pulp doctor` checks (fails fast if environment is broken)
2. In standalone product mode: if run from inside a Pulp checkout, prepares pinned dependencies from that checkout and caches a local SDK install; otherwise downloads and caches the SDK release
3. Scaffolds source files from templates (processor, format entries, test, CMakeLists.txt)
4. In in-tree mode: adds the project to `examples/CMakeLists.txt`
5. In standalone product mode: generates `pulp.toml` with pinned SDK version and local SDK hints when created from a checkout
6. Configures, builds the generated test target plus the default platform outputs, and runs tests
7. Leaves the project ready for `pulp build`, which you use for rebuilds, explicit targets, or optional deliverables after changing configuration

Notes:
- `pulp create` is meant to prove that a fresh machine and fresh project can scaffold, configure, build the default native outputs, and pass the generated tests.
- Use `pulp build` after `pulp create` when you want to rebuild, target a specific format/app target, or materialize optional deliverables after changing configuration.
- Browser targets such as WAM/WebCLAP are separate lanes and are not emitted by default by `pulp create`.

Default formats are platform-gated:
- **macOS**: VST3, AU, CLAP, Standalone
- **Linux**: VST3, CLAP, LV2, Standalone
- **Windows**: VST3, CLAP, Standalone
- **app/bare**: Standalone only

On macOS and Windows, AAX is optional. `pulp create` only scaffolds `aax_entry.cpp`
and includes the AAX target when an AAX SDK is already configured via
`PULP_AAX_SDK_DIR`. Linux and Ubuntu do not support AAX.

### build

**Status**: usable

Configure and build the project. Auto-detects when CMake reconfiguration is needed. Works with both repo-based and standalone projects.

```bash
pulp build                    # Build all targets
pulp build --target PulpGain_VST3  # Build specific target
pulp build -j8                # Parallel jobs
pulp build --watch            # Build and watch for changes
pulp build --watch --test     # Build, watch, run tests on change
pulp build --allow-unsupported-sdk # Bypass the CLI-vs-project SDK guard (unsupported)
```

Extra arguments are passed through to `cmake --build`.

The `--watch` flag enters a file-watching loop after the initial build. It polls source files every 500ms and rebuilds on changes. Combine with `--test` to run tests after each successful rebuild and `--validate` to run quick dlopen checks.

For standalone projects (detected via `pulp.toml`), automatically sets `CMAKE_PREFIX_PATH` to the hinted local SDK when available, otherwise to the cached SDK release.
Before configure/build, `pulp build` also compares the active project's pinned `sdk_version` / `cli_min_version` against the running CLI. If the project is ahead, it fails fast and points at `pulp upgrade`; use `--allow-unsupported-sdk` only as an explicit unsupported escape hatch.

When `pulp build` decides a CMake reconfigure is required, it also runs the FetchContent cache preflight from `pulp doctor --caches` first. If the shared cache (`~/Library/Caches/Pulp/fetchcontent-src/` on macOS, `$XDG_CACHE_HOME/pulp/fetchcontent-src/` on Linux, `%LOCALAPPDATA%/Pulp/fetchcontent-src/` on Windows) contains a dangling symlink or stale-commit entry, `pulp build` aborts with a one-screen remediation message instead of letting `cmake` blow up 200 lines into the configure log. Run `pulp doctor --caches --fix` to heal user-owned drift, or set `PULP_SKIP_CACHE_PREFLIGHT=1` to bypass the gate (#744).

On Windows, `pulp build` also selects a Visual Studio generator automatically when no active MSVC shell is detected on `PATH`.

### test

**Status**: usable

Run the test suite via CTest. Builds first if no build directory exists. Works with both repo-based and standalone projects.

```bash
pulp test                     # Run all tests
pulp test -R Gain             # Run tests matching "Gain"
```

Extra arguments are passed through to `ctest`.

When `pulp test` triggers a cold-start build (no `build/CMakeCache.txt`), the FetchContent cache preflight from `pulp doctor --caches` runs first and aborts with a clear remediation message on any unhealthy entry — same gate `pulp build` applies, same `PULP_SKIP_CACHE_PREFLIGHT=1` bypass (#744).

### status

**Status**: usable

Show project information for either SDK mode or source-tree mode.

```bash
pulp status
```

`pulp status` reports which mode you are in so external projects never silently depend on a random checkout and repo/examples never silently pick up a cached SDK.
In SDK mode it also reports the pinned SDK version plus the resolved SDK path and checkout hints when present.
On macOS and Windows it also reports whether an optional AAX SDK is detected. On
Linux and Ubuntu it reports AAX as unsupported.

### validate

**Status**: usable

Run plugin format validators on all built plugins.

```bash
pulp validate              # CLAP + VST3 (pluginval) + AU + optional AAX
pulp validate --all        # Also run vstvalidator and full AAX validation if installed
pulp validate --json       # Print JSON report to stdout
pulp validate --report out.json  # Write JSON report to file
pulp validate --strict     # CI gate: skipped-because-missing-tool ⇒ exit 1
```

**Validator-discovery preflight (#743).** Before launching any validator,
`pulp validate` runs the same discovery that powers `pulp doctor --validators`.
If any validator on disk has a broken code signature (the classic case:
a copy of `pluginval` ripped out of its `.app` bundle, where amfid will
SIGKILL the process at launch with exit 137 and zero stderr), `pulp
validate` aborts with the exact path + remediation instead of letting
the run die mid-validation. Run `pulp doctor --validators --fix` to
clean up user-owned broken copies, or follow the printed sudo one-liner
for root-owned ones.

**Missing-validator policy.** If `clap-validator`, `pluginval`, `auval`,
or the AAX validator is not installed, affected plugins are reported as
`SKIPPED`. The default mode prints a loud warning listing which tools
are absent and how to install them — a green run without all four
validators is *not* the same as a run where all four passed. Use
`--strict` in CI (or any environment where partial coverage is a bug)
to treat those skips as hard failures.

Checks:
- **CLAP**: uses `clap-validator` if installed, otherwise falls back to CTest dlopen checks
- **VST3**: uses `pluginval` (strictness level 5, 30s timeout) if installed, otherwise skips
- **AU**: uses `auval` on macOS
- **AAX**: uses DigiShell + AAX Validator on macOS/Windows if installed via `PULP_AAX_VALIDATOR_DIR` or the recommended `~/SDKs/avid/aax-validator/` layout
- **VST3 (--all)**: also runs `vstvalidator` if installed (Steinberg SDK tool, optional)
- **AAX (--all)**: runs the broader `aaxval` test suite instead of the faster describe-validation probe

Flags:
- `--all` — run every available validator, including `vstvalidator` and full AAX validation
- `--json` — emit a machine-readable JSON report to stdout (conforms to `validation-report-v1.schema.json`)
- `--report <path>` — write the JSON report to a file

When a validator tool is not installed, the check is reported as SKIPPED with a clear message.
The JSON report conforms to `docs/contracts/validation-report-v1.schema.json`.
On Linux and Ubuntu, AAX validation is never attempted because AAX is unsupported there.

Prints a summary with pass/fail/skip counts.

### run

**Status**: usable

Launch a standalone Pulp application from the build directory.

```bash
pulp run                                            # find and launch first standalone binary
pulp run PulpGain                                   # launch a specific target
pulp run MyApp -- --arg1                            # pass arguments to the launched binary
pulp run --headless --screenshot ui.png             # CI: render offscreen, save PNG
pulp run --headless --screenshot ui.png --frames 60 # render N frames before capture
pulp run --watch                                    # re-launch on source-file changes
```

Searches the active project's build output:
- standalone projects: `build/bin/`
- in-repo examples: `build/examples/`

#### Headless / screenshot flags (#914)

- `--headless` — run without a window. The CLI forwards `--headless`
  to the launched binary and also sets `PULP_HEADLESS=1`, so binaries
  that read either source pick up the headless mode.
- `--screenshot <file>` — save a PNG to `<file>`. Implies `--headless`.
  Forwarded as `--screenshot <file>` and via `PULP_SCREENSHOT=<file>`.
  If `--headless` is given without an explicit screenshot path, the
  CLI defaults to `build/<target>.png`.
- `--frames <n>` — number of frames to render before capture. Default
  1. Forwarded as `--frames <n>` and via `PULP_FRAMES=<n>`.
- `--watch` — re-launch the binary whenever a source file changes.
  Composes with `--headless` / `--screenshot` so the dev loop can
  re-render PNGs on every save.

These flags are intended for CI auto-validation: any plugin standalone
that respects `PULP_HEADLESS` / `PULP_SCREENSHOT` / `PULP_FRAMES` (or
the matching argv flags) can be exercised end-to-end on every PR
without a real window or virtual display.

### cache

**Status**: usable

Manage the Pulp SDK and asset cache at `~/.pulp/` by default.

```bash
pulp cache                  # Show help
pulp cache status           # Show cached SDKs and assets with sizes
pulp cache fetch skia       # Download Skia GPU rendering binaries
pulp cache clean            # Remove all cached assets
```

**Subcommands**:

| Subcommand | What it does |
|------------|-------------|
| `status` | List cached SDK versions and downloaded assets |
| `fetch skia` | Download platform-specific Skia GPU binaries to `~/.pulp/cache/` by default |
| `clean` | Remove all files from the asset cache |

GPU rendering requires Skia binaries. If a standalone project enables GPU features, run `pulp cache fetch skia` to download them.
Set `PULP_HOME` to relocate the cache, SDK, and config root.

### doctor

**Status**: usable

Diagnose environment issues. Checks C++20 compiler, CMake version, git-lfs, LFS file state, external SDKs (VST3, AudioUnit), platform-specific dependencies, and the expected project mode.

```bash
pulp doctor                          # show all checks
pulp doctor --fix                    # auto-fix issues where possible
pulp doctor --ci                     # non-interactive, exit codes only
pulp doctor --dry-run                # show what --fix would do
pulp doctor --versions               # CLI/SDK/Plugin version diagnostics (#499 Slice 1)
pulp doctor --versions --scan-parents # ALSO walk CWD ancestors for pulp_add_* projects (#552)
pulp doctor --versions --json        # emit the diagnostic as stable JSON (#552)
pulp doctor --validators             # discover auval / pluginval / clap-validator + verify signatures (#743)
pulp doctor --validators --fix       # ALSO remove user-owned broken copies; root-owned breakage prints sudo one-liner
pulp doctor --validators --dry-run   # preview what --fix would do
pulp doctor --caches                 # FetchContent shared-source cache health (#744)
pulp doctor --caches --fix           # heal user-owned dangling/stale-commit entries
pulp doctor --caches --fix --dry-run # preview heal without removing anything
pulp doctor --caches --json          # emit the cache report as stable JSON
```

Checks are platform-gated — only relevant checks run on each OS:
- **macOS**: git, compiler, CMake, git-lfs, LFS files, VST3 SDK, AudioUnitSDK, optional AAX SDK/validator, build state
- **Linux**: git, compiler, CMake, git-lfs, LFS files, VST3 SDK, ALSA dev headers, build state
- **Windows**: git, compiler, CMake, git-lfs, LFS files, VST3 SDK, optional AAX SDK/validator, build state

Mode-specific checks:
- **SDK mode**: verifies `pulp.toml`, the installed SDK path or cache, optional checkout hints, and build configuration for the external project
- **Source-tree mode**: verifies the active checkout, pinned external SDKs, LFS state, and build configuration for the repo/examples workflow

For AAX-specific setup details and download guidance, see [AAX Setup](../guides/aax.md).

Exit code is 0 if all checks pass, 1 if any fail.

`pulp doctor --versions` is a dedicated diagnostic (not a drift-check)
that prints the three surface versions side-by-side plus advisory
skew warnings:

- **CLI** — the version baked into the running `pulp` binary
- **Plugin** — read from `.claude-plugin/plugin.json` in the active
  repo (falls back to `~/.claude/plugins/pulp/plugin.json` for
  installed plugin layouts)
- **SDK** — the active project's SDK version, from `pulp.toml`
  (standalone projects) or `CMakeLists.txt` (source-tree mode)

Skew warnings fire when the project's `pulp.toml` declares a
`cli_min_version` higher than the installed CLI, or when the
project SDK is newer than the running CLI. Warnings are advisory
— the command always exits 0 so it's safe to wire into scripts.

The first release that defines the following optional `pulp.toml`
field activates the `cli_min_version` check:

```toml
sdk_version = "0.24.0"
cli_min_version = "0.24.0"   # optional — warn if installed CLI is older
```

Untagged CLI builds (anything not matching `M.N.P` exactly) are
skipped silently, matching the design's forward-compatible
convention.

**Multi-project skew (#552 Slice 1b).** When `~/.pulp/projects.json`
contains registered projects, `pulp doctor --versions` lists each
project with its own SDK / `cli_min_version` pair and surfaces any
skew inline. Entries whose `path` no longer exists are shown with a
`(missing)` tag and a `pulp projects remove <path>` hint — we never
auto-prune; only explicit removal mutates the registry.

`--scan-parents` is an opt-in escape hatch for projects that were
never registered (for instance, a cloned example). It walks the
current directory's ancestor chain looking for `CMakeLists.txt`
files that invoke any `pulp_add_*` macro and surfaces those matches
in-line with a `(scanned)` tag. Ancestor hits are NOT added to the
registry — the design decision is "registry is authoritative,
ancestor scan is a diagnostic escape hatch."

If both a parent directory and a nested child directory contain a
`pulp_add_*` invocation, both appear in the report (deepest-first —
closest ancestor to the current directory comes first). Resolving
"which is the canonical project" is the user's call; the diagnostic
surfaces both so the ambiguity is visible.

`--json` emits the same information as a single JSON object with a
stable shape — `{"cli": {...}, "plugin": {...}, "plugin_min_cli":
{...}, "project_sdk": {...}, "projects": [{"path": ..., "sdk": {...},
"cli_min": {...}, "missing_on_disk": bool, "scanned": bool}, ...],
"findings": [...]}`. Scripts should use the `findings[]` array for
user-visible warnings; per-field semver fields carry
`comparable: true` only when they parse as pure `M.N.P`.
`plugin_min_cli` is populated from the plugin's `plugin.json`
`min_cli_version` field (release-discovery Slice 6, #551); absent in
older plugin builds.

#### `--validators` (#743)

`pulp doctor --validators` is a dedicated diagnostic that discovers the
three plugin-format validators `pulp validate` shells out to —
`auval`, `pluginval`, `clap-validator` — and verifies each candidate's
code signature is intact. It catches the failure mode where a binary
copied OUT of its `.app` bundle (commonly `/usr/local/bin/pluginval`
copied from `/Applications/pluginval.app/Contents/MacOS/`) retains a
signature claim that references peer files inside the bundle. macOS
amfid kills such a binary at launch with exit 137 and zero stderr —
the user has no diagnostic without already knowing this exists.

Per-validator output is one of:

- `OK auval: /usr/bin/auval (valid signature on disk …)`
- `FAIL pluginval: /usr/local/bin/pluginval — invalid Info.plist (plist or signature have been modified). Root-owned — sudo required.`
- `WARN clap-validator: not installed. Run `cargo install clap-validator`.`

`--fix` removes broken **user-owned** copies in place (counted in the
`Auto-fixed` summary). Broken **root-owned** copies are never
auto-elevated — the doctor prints a `sudo rm <path>` one-liner for the
user to run manually. `--fix --dry-run` previews the same actions
without mutating anything. `--fix` is a no-op on a fully healthy env.

Discovery walks each validator's well-known paths in priority order
(system path → cask app bundle → PATH lookup → `~/.cargo/bin`) and
stops at the first existing candidate. The first-existing path wins
deliberately: that's the binary the user's shell will dispatch, so
that's the copy `pulp validate` will SIGKILL on if it's broken.
Masking it with a healthy copy further down the list would defeat the
diagnostic.

Exit code is 0 only when every validator is Healthy. Missing validators
also contribute to a non-zero exit because a host without the validators
can't run `pulp validate` at all — the doctor must surface that.

`pulp validate` runs the same discovery as a preflight before launching
any validator. If any validator is in the Broken state, `pulp validate`
aborts with the exact remediation instead of letting amfid SIGKILL the
run mid-validation.

**FetchContent cache health (#744).** `pulp doctor --caches` audits
the shared-source FetchContent cache that Pulp uses to avoid
re-cloning external SDKs across builds. The cache root is the same
path `pulp_register_fetchcontent_source` populates:

- macOS: `~/Library/Caches/Pulp/fetchcontent-src/`
- Linux: `$XDG_CACHE_HOME/pulp/fetchcontent-src/` (default `~/.cache/pulp/fetchcontent-src/`)
- Windows: `%LOCALAPPDATA%/Pulp/fetchcontent-src/`

Each cache entry is classified as one of:

| Status | Meaning | `--fix` action |
|--------|---------|----------------|
| `[ok]` | Healthy — entry exists and (if a symlink) target exists, cached REF matches the declared `pulp_register_fetchcontent_source(... REF ...)` in the active project's `CMakeLists.txt`. | (no-op) |
| `[!!] dangling-symlink` | Entry is a symlink whose target no longer exists. CMake's `FETCHCONTENT_SOURCE_DIR_*` override would fail at configure time. | `rm` the symlink — next configure refetches. |
| `[!!] stale-commit` | The directory name's REF suffix differs from the declared REF. The pin in `CMakeLists.txt` advanced but the user's old cache is still authoritative. | `rm -rf` the entry — next configure refetches. |
| `[!!] root-owned` | Entry is not user-writable (likely owned by root from a stray `sudo`). | Reported only; agent never tries to `sudo rm`. Manual: `sudo rm -rf <path>`. |

Exit code is 0 when every entry is `[ok]`, 1 otherwise. The same
discovery code runs as a preflight inside `pulp build` and `pulp test`
when a CMake reconfigure is needed — set `PULP_SKIP_CACHE_PREFLIGHT=1`
to bypass the gate (intended for sealed CI environments that can't
auto-heal).

`--caches --fix` removes only user-owned entries marked `[!!]
dangling-symlink` or `[!!] stale-commit`. Root-owned entries are
report-only by design — automatic `sudo` is out of scope so agents
don't silently elevate. `--caches --fix --dry-run` previews what
would be removed without touching the filesystem.

`--caches --json` emits a stable shape:
`{"cache_root": "...", "healthy": bool, "entries": [{"name", "path",
"status", "is_symlink", "resolved_target", "declared_ref",
"cached_ref", "dep_name", "reason", "remediation", "fixable"}, ...]}`.
The `status` field uses the lowercased label set above
(`healthy`, `dangling-symlink`, `stale-commit`, `root-owned`,
`unknown`).

### projects

**Status**: usable

Manage the `~/.pulp/projects.json` registry. `pulp create` and
`pulp new` register new projects automatically on successful
scaffold; these commands exist so users can add projects created
outside of `pulp create` (clones, manual checkouts) and prune stale
entries. Registry entries are read by `pulp doctor --versions` to
produce per-project skew reports.

```bash
pulp projects list                       # show registered projects
pulp projects add                        # register the current directory
pulp projects add ~/code/my-plugin       # register a specific directory
pulp projects remove ~/code/old-plugin   # forget a project by path
```

The registry is a plain JSON file with one top-level `projects`
array; each entry has `path`, `name`, and `registered_at`. The
location is `$PULP_HOME/projects.json` (defaulting to
`~/.pulp/projects.json`). A missing registry file is treated as an
empty list — no first-run setup is required.

### project

**Status**: usable

Per-project SDK pin management. Updates a consumer project's pinned
Pulp SDK version and records an undo batch at
`~/.pulp/bump-undo-<timestamp>.json` so mistakes are one command away
from recovery.

In standalone SDK-mode projects (`pulp.toml` present), the SDK pin is
`pulp.toml` `sdk_version`. `pulp project bump` updates that field and
the versioned `find_package(Pulp X.Y.Z ...)` line together. It does
not rewrite `project(NAME VERSION ...)`; that remains the app/plugin
product version. If `sdk_path` points at a managed Pulp SDK cache for
the old version, it is moved to the matching new cache path. Custom
`sdk_path` values are left alone and verified later by `pulp build`.

In legacy source-embedded projects, the command recognizes
`FetchContent_Declare(pulp ... GIT_TAG vX.Y.Z)`,
`pulp_add_project(NAME VERSION X.Y.Z ...)`, and
`project(NAME VERSION X.Y.Z ...)`.

```bash
pulp project bump                     # bump CWD project to CLI's own version
pulp project bump 0.32.0              # bump to explicit version (positional)
pulp project bump --to=0.32.0         # bump to explicit version (named)
pulp project bump --all               # iterate ~/.pulp/projects.json
pulp project bump --all --dry-run     # show plan without writing
pulp project bump --force-dirty       # skip the git-clean check
pulp project bump --allow-downgrade   # target older than current pin
pulp project bump --allow-cli-skew    # target newer than installed CLI
pulp project bump --allow-redundant   # ignore origin/main already-newer guard
pulp project bump --verify-builds     # build after bump; roll back on failure

pulp project undo                     # revert the newest batch
pulp project undo <timestamp>         # revert a specific batch
```

**Safety rails:** branch pins (`GIT_TAG main`) and SHA pins are
skipped with a diagnostic; dirty pin-bearing files are gated behind
`--force-dirty`; target older than current is gated behind
`--allow-downgrade`; target newer than the installed CLI is gated
behind `--allow-cli-skew`; worktrees where `origin/main` already pins
the target-or-newer SDK are skipped unless `--allow-redundant` is set;
and `--all` isolates per-project failures so one broken project
doesn't abort the rest. Running inside the Pulp source checkout is
refused because that is a framework release/version operation, not a
consumer project SDK bump.

**Migration notes** from Slice 3 (`#548`) print after a successful
bump so users see any API changes the hop introduced.

**When to use `pulp upgrade` vs `pulp project bump`:** use
`pulp upgrade` to replace the installed Pulp CLI/SDK toolchain. Use
`pulp project bump` after that when the current project should move to
that SDK. The Claude `/upgrade` flow exposes this as "upgrade the
tool" vs "upgrade the tool and bump this project's SDK pin".

**Post-upgrade hook:** the `update.bump_projects` config key (prompt
| auto | off; default prompt) controls whether `pulp upgrade` prints
a `pulp project bump --all` hint after a successful CLI upgrade.

### ci-local

**Status**: legacy (prefer [Shipyard](https://github.com/danielraffel/Shipyard))

> **Note:** For most CI workflows, use `shipyard run` and `shipyard ship` instead of `pulp ci-local`. Shipyard is Pulp's primary CI tool as of v0.3.0 and provides the same target matrix with evidence-gated merges. `pulp ci-local` remains available as an advanced fallback — see issue #120 for removal timeline.

Local-first CI control plane for Pulp. This is the shared operator surface for:

- machine-global local/SSH queueing
- exact-SHA validation on this Mac and configured hosts
- deliberate GitHub Actions dispatch/status when cloud orchestration is needed

```bash
pulp ci-local run
pulp ci-local run --smoke
pulp ci-local check 123
pulp ci-local status
pulp ci-local cleanup
pulp ci-local cleanup --dry-run
pulp ci-local cleanup --apply
pulp ci-local cloud workflows
pulp ci-local cloud defaults
pulp ci-local cloud history
pulp ci-local cloud compare build
pulp ci-local cloud recommend build
pulp ci-local cloud run build feature/my-branch
pulp ci-local cloud run build feature/my-branch --provider namespace
pulp ci-local cloud run build feature/my-branch --provider namespace --macos-runner-selector-json '"namespace-profile-big-apple"'
pulp ci-local cloud run build feature/my-branch --provider namespace --macos-runner-selector-json '"nscloud-macos-tahoe-arm64-6x14"'
pulp ci-local cloud run docs-check feature/my-branch --provider namespace --wait
pulp ci-local cloud run docs-check feature/my-branch --provider namespace --runner-selector-json '"namespace-profile-big-apple"'
pulp ci-local cloud namespace doctor
pulp ci-local cloud namespace setup
pulp ci-local cloud status latest --refresh
```

Local queue commands:

- `run` — queue validation and wait for completion
- `check` — queue validation for an existing PR
- `ship` — push, open PR, queue CI, merge on green
- `enqueue` / `drain` / `bump` / `cancel` — queue management
- `logs` / `evidence` / `status` — saved results and operator visibility
- `cleanup` — inspect or prune retained local-CI artifacts; dry-run by default,
  `--apply` is blocked while jobs are running, and `--include-prepared` also
  removes cached build/install state that later reruns will rebuild

Cloud companion commands:

- `cloud workflows` — list the GitHub workflows and supported runner providers known to this checkout
- `cloud defaults` — show the effective workflow/provider defaults plus where current selector values came from (local config versus repo-variable fallback)
- `cloud history` — show recent tracked cloud runs plus any configured billing-period rollup
- `cloud compare [workflow]` — compare observed cloud providers for one workflow using recorded history, including latest success timing
- `cloud recommend [workflow]` — recommend a cloud provider from recorded history
- `cloud run [workflow] [branch]` — dispatch a GitHub Actions workflow by branch; `docs-check` accepts `--runner-selector-json`, while `build` also accepts one-off `--linux-runner-selector-json`, `--windows-runner-selector-json`, and `--macos-runner-selector-json` overrides for per-leg routing
- `cloud status [dispatch-id|latest]` — show tracked GitHub run state plus queue-delay/elapsed timing when available; Namespace-backed runs also report provider runtime/machine-shape truth when `nsc` can match the instances; `--refresh` re-queries GitHub for the selected run
- `cloud namespace doctor` — verify that `nsc` is installed, login is valid, and the current workspace is visible
- `cloud namespace setup` — thin wrapper that runs `nsc login` if needed and then shows the same Namespace status

Current cloud scope:

- GitHub Actions remains the orchestrator
- `docs-check` is the first runner-provider pilot and supports `github-hosted` and `namespace`
- `build` now also supports `github-hosted` and `namespace`; the default cloud
  build covers Linux and Windows only so macOS can stay local-first
- `docs-check` can use an explicit `--runner-selector-json` override or a docs-check-specific local config default before falling back to the repo Namespace selector variable
- `build` can take Linux/Windows Namespace selectors from the local config keys
  `github_actions.workflows.build.providers.namespace.linux_runner_selector_json`
  and `.windows_runner_selector_json`, or from the repo variables
  `PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON` and
  `PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON`
- macOS Namespace is opt-in for `build`: set
  `--macos-runner-selector-json` for a one-off run, or set
  `github_actions.workflows.build.providers.namespace.macos_runner_selector_json`
  locally or `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON` if you want an explicit
  macOS Namespace validation run
- selector overrides can use either a Namespace profile label like
  `"namespace-profile-generouscorp-macos"` or a direct machine label like
  `"nscloud-macos-tahoe-arm64-6x14"`
- that selector must point at a real macOS-capable Namespace profile: GitHub
  labels and matrix names alone do not prove the underlying OS, so a Linux
  Namespace profile can appear as a macOS leg while actually executing on Linux
- if macOS should remain local by default, keep the shared macOS selector unset
  and use `--macos-runner-selector-json` only for one-off cloud validation runs
- if you plan to use the Namespace provider, install the `nsc` CLI and run `nsc login` first; that is the recommended operator setup path for this phase
- VM/SSH target configuration and Namespace provider configuration remain separate: local/SSH hosts stay in the normal local CI target config, while Namespace routing and login state live behind the `cloud namespace` helper surface
- `validate` and `sanitizers` remain `github-hosted` only in this phase
- cloud dispatch records are persisted beside local CI state, but they do not enter the local queue
- `status` includes recent tracked cloud summaries without contacting GitHub; use `cloud status --refresh` when you want live GitHub state
- tracked cloud runs now persist queue-delay and elapsed-duration timing so later comparison commands can report real provider speedups instead of ad hoc estimates
- estimated cost reporting is optional and local-config driven; every derived number is labeled `estimated; verify provider pricing`
- provider-reported billing totals are opt-in and off by default; when enabled,
  Pulp shows them separately from tracked-run estimates because they are repo-wide
  current-period figures
- if the provider CLI does not expose billing totals, Pulp still reports runtime and machine shape instead of inventing invoice truth
- `status` also reports the current local-CI footprint for bundles, prepared state, logs, results, and tracked cloud runs
- `cleanup` supports the operator-facing retention workflow: inspect reclaimable space first, then re-run with `--apply` only when no local CI job is active

Namespace profile setup note:

- `nsc` is enough for login verification and instance/history inspection, but
  GitHub Actions runner-profile creation is still a Namespace dashboard step in
  this phase
- create new profiles under `GitHub Actions -> Profiles`
- the UI profile name omits the GitHub selector prefix; for example a profile
  shown as `generouscorp-macos` is referenced from Pulp as
  `"namespace-profile-generouscorp-macos"`
- for ad hoc runs, Namespace also supports direct machine labels without a saved
  profile, for example `"nscloud-macos-tahoe-arm64-6x14"`
- after creating a one-off macOS profile, validate it with:
  `pulp ci-local cloud run build <branch> --provider namespace --macos-runner-selector-json '"namespace-profile-generouscorp-macos"'`
- confirm the backing shape with `nsc instance history --all -o json`; a valid
  macOS profile should report `shape.os = "macos"` and
  `shape.machine_arch = "arm64"`

### ship

**Status**: experimental

Signing and packaging subcommands.

```bash
pulp ship sign --identity "Developer ID Application: ..."
pulp ship sign --identity "..." --entitlements path/to/entitlements.plist
pulp ship package --version 1.0.0
pulp ship check
```

**Subcommands**:

| Subcommand | What it does |
|------------|-------------|
| `sign`     | Code-sign all built plugin bundles (VST3, CLAP, AU) |
| `package`  | Create `.pkg` installers in `artifacts/` |
| `check`    | Check signing status of all built plugins |

`sign` requires `--identity`. The default entitlements file is `ship/templates/entitlements.plist`.

`package` creates per-format `.pkg` files using `pkgbuild`. macOS only.

### pr

**Status**: usable

Create, validate, and merge a PR through the canonical ship flow.

`pulp pr` is the primary "ship this" orchestrator referenced by the CI skill.
By default it delegates to `shipyard pr` (single source of truth for ship
orchestration). Use `--native` only for diagnostics when debugging the
CLI-side fallback path. Natural-language triggers in agent conversations
("push to main", "ship this", "ship it", "we're done", "merge this",
"push it", "run CI", "push a PR") all route here — see the
[CI skill](../../.agents/skills/ci/SKILL.md) (`.agents/skills/ci/SKILL.md`)
for the authoritative trigger list.

```bash
pulp pr
pulp pr --base origin/main
pulp pr --title "feat(cli): document pulp pr and sync CI policy"
pulp pr --no-ship
pulp pr --no-push
pulp pr --dry-run

# Fallback when the local `pulp` binary is broken (for example wgpu
# dylib load failure). Equivalent for the default ship cycle:
shipyard pr
```

Flags:

| Flag | Description |
|------|-------------|
| `--base <ref>` | Diff base (default: `origin/main`) |
| `--title <s>` | PR title (default: tip commit subject) |
| `--no-ship` | Create PR but skip `shipyard ship` |
| `--no-push` | Stop after bump commit; do not push or create PR |
| `--dry-run` | Print the plan without executing steps |
| `-h`, `--help` | Show help |

Notes:

- Default behavior is shim delegation to `shipyard pr` — the single source
  of truth for ship orchestration. Do not treat `gh pr create` + `shipyard ship`
  as a substitute; that sequence bypasses the skill-sync and version-bump
  gates `pulp pr` runs first.
- `--native` runs an in-CLI fallback that performs the same gates + PR flow
  without delegating to Shipyard. Diagnostic use only.
- For the canonical list of natural-language ship triggers and the full
  policy, see the [CI skill](../../.agents/skills/ci/SKILL.md)
  (`.agents/skills/ci/SKILL.md`).

### docs

**Status**: usable

Browse local documentation and status manifests. All subcommands read from local files in `docs/` only -- no web calls.

```bash
pulp docs                         # Show help
pulp docs index                   # List available docs
pulp docs search <query>          # Search docs for a string
pulp docs open <slug>             # Print a doc by slug
pulp docs show support <thing>    # Look up support status
pulp docs show command <name>     # Look up a CLI command
pulp docs show cmake <name>       # Look up a CMake function
pulp docs show style              # Show code style rules
pulp docs check                   # Validate docs consistency
```

**Subcommands**:

| Subcommand | What it does |
|------------|-------------|
| `index` | Print a readable list of available docs from `docs-index.yaml` |
| `search <query>` | Case-insensitive search across all Markdown files in `docs/` |
| `open <slug>` | Resolve slug via `docs-index.yaml` and display the file |
| `show support <thing>` | Look up platform/format/subsystem support from `support-matrix.yaml` |
| `show command <name>` | Look up a CLI command from `cli-commands.yaml` |
| `show cmake <name>` | Look up a CMake function from `cmake-functions.yaml` |
| `show style` | Display style rules from `style-rules.yaml` with links to policy docs |
| `check` | Validate docs consistency: manifest links, index completeness, status vocabulary, module dependencies vs CMake |

### design

**Status**: experimental

Launch the local AI-powered design tool used for token, shader, and style iteration.

```bash
pulp design
pulp design path/to/design-tool.js
pulp design --script path/to/design-tool.js
pulp design --build-dir /tmp/pulp-design-parity-build
```

`pulp design` now configures/builds `pulp-design-tool` on demand before launch. When run inside
a Pulp checkout it loads `examples/design-tool/design-tool.js` from that checkout and builds into
that checkout's `build/` directory.

Use `--script` to point at a different JS entry, and `--build-dir` when you are working from a
nonstandard build tree such as a separate worktree build directory.

When run outside a Pulp checkout, `pulp design` can currently auto-bind only when the `pulp`
binary itself lives inside a Pulp build tree such as `.../build/tools/cli/pulp`. Generic
PATH-installed or symlinked CLI setups are not fully SDK-mode aware yet; use `--build-dir` and
`--script` explicitly in split layouts where the project repo and the Pulp SDK live in different
directories.

The selected build environment is the authority for supported behavior. `pulp design` prints the
chosen root, build dir, and script path so the provenance is explicit.

The design tool chat now supports provider/model-aware local execution in the UI.
The current app exposes a provider selector (`Claude`, `Codex`), a model selector,
and a reasoning-effort selector for Codex/OpenAI models.

### design-debug

**Status**: experimental

Run the before/after/diff harness for design-chat prompts. This is the
automation/debug companion to `pulp design`.

```bash
pulp design-debug --prompt "make the gain knob look like macOS 7" --target k1
pulp design-debug --prompt "design a cyberpunk interface for a modern synth plugin" --target all --provider claude --model claude-sonnet-4-6
pulp design-debug --prompt "make the gain knob look like a precision analyzer control" --target k1 --provider codex --model gpt-5.4 --reasoning-effort xhigh
pulp design-debug --prompt "warm analog EQ" --target all --response-file saved-response.json
pulp design-debug --prompt "make the gain knob look like premium brushed aluminum" --target k1 --capture-backend live-gpu
```

Artifacts are written by default under `build/design-debug/`:
- `*-before.png`
- `*-after.png`
- `*-diff.png`
- `*-target-before.png`, `*-target-after.png`, `*-target-diff.png` for targeted runs
- `*-prompt.txt`
- `*-response.txt`
- `*-debug-state.json`
- `*-apply-summary.txt`
- `*-report.json`
- `latest-report.json`
- `latest-run.json`
- `runs.jsonl`

The JSON report records:
- `provider`, `model`, `reasoning_effort`
- `target` and `target_bounds`
- `target_region`, `target_region_source`, and target-only diff stats (`target_diff_pixels`, `target_diff_pct`) when a widget target is selected
- `debug_state` from the design tool (`changedColors`, `changedDimensions`, `widgetLookIds`, summary, request text)
- the exact `ai_command` or live `driver_command` used for local execution
- screenshot-diff stats (`similarity_pct`, `diff_pixels`, `mean_error`)

Useful flags:
- `--provider claude|codex`
- `--model <name>`
- `--reasoning-effort low|medium|high|xhigh`
- `--capture-backend skia|coregraphics|live-gpu`
- `--response-file <json-or-text>` to replay a saved model response without calling AI
- `--script <path>` to load a custom design-tool JS file
- `--design-tool-bin <path>` to point `live-gpu` runs at a built `pulp-design-tool`
- `--output-dir <dir>` to redirect artifact output
- `--width`, `--height`, `--scale` to control the render size
- `--delay-ms`, `--after-delay-ms` to control baseline/post-apply capture timing in `live-gpu` mode
- `--ai-cli <template>` to override the local AI command template

Backend behavior:
- The default `--capture-backend skia` path renders through an offscreen Skia surface,
  so widget SkSL is present in the `before`/`after` images and the report records
  `render_backend: "skia-headless"` with `widget_sksl_render_supported: true`.
- `--capture-backend live-gpu` drives the real `pulp-design-tool` app in automation
  mode, captures before/after images from the actual Skia/Graphite presentation path,
  and records `render_backend: "skia-live-gpu"` with `sksl_gpu_supported: true`.
- `--capture-backend coregraphics` is still available for comparison, but it does not
  faithfully render custom widget SkSL.

Remaining limitation:
- `skia` and `coregraphics` still validate headless render paths, not the live app.
  Use `live-gpu` when you need proof from the actual design-tool renderer.

### inspect

**Status**: experimental

Launch the built component inspector binary with its demo surface.

```bash
pulp inspect
```

This command delegates to `tools/screenshot/pulp-screenshot` in the active build tree. Build the repo first if the binary is missing.

### import-design

**Status**: experimental

Import designs from Figma, Stitch, v0, Pencil, or Claude Design source files into generated Pulp UI code.

```bash
pulp import-design --from figma --file frame.json
pulp import-design --from figma --url 'https://figma.com/design/...' --frame 'Plugin UI'
pulp import-design --from stitch --file screen.html --screen 'Main'
pulp import-design --from v0 --url 'https://v0.dev/t/abc123' --output ui.js
pulp import-design --from pencil --file ui.json --output ui.js --tokens tokens.json
pulp import-design --from v0 --file card.tsx --dry-run
pulp import-design --from claude --file design.html --classnames classnames.json
```

Supports `--url` (fetches via curl), `--frame` (Figma frame selection), and `--screen` (Stitch screen selection). See [Design Import API Reference](design-import.md) for the full flag list.

For `--from claude`, the CLI emits a `classnames.json` artifact alongside the generated JS view and `tokens.json`. The artifact maps `classname → { cssProp(camelCase): cssValue, ... }` for every `<style>` rule with a plain classname selector — `@pulp/css-adapt` (and downstream) consumes it to merge class-based styles into inline before forwarding to bridge calls. Mirrors the output of Spectr's `tools/extract-html-bundle/extract.mjs` (pulp #1035).

| Flag | Description |
|------|-------------|
| `--classnames <path>` | Where to write the classname artifact (default: `classnames.json`). Only emitted for `--from claude`. |
| `--emit classnames` | Force-emit `classnames.json` (default on for `--from claude`). |
| `--no-emit-classnames` | Skip the classname artifact for the run. |

### export-tokens

**Status**: experimental

Export a theme as W3C Design Tokens JSON.

```bash
pulp export-tokens --file theme.json --tokens tokens.json
pulp export-tokens --dry-run
```

### audio

**Status**: experimental

Repo-level audio analysis tooling. Manages offline audio models (text-to-audio / text-to-excerpt retrieval) and reads reproducible excerpt bundles. This is developer tooling for building datasets and evaluation corpora — not a runtime API.

```bash
pulp audio                                      # Show help
pulp audio model list [--json]                  # List registered models
pulp audio model status [--json]                # Show configured + resolved model
pulp audio model activate <model-id> [--json]   # Pick the active model
pulp audio excerpt-find --text "warm analog pad" --input /path/to/corpus [options]
pulp audio read-bundle <path-to-bundle> [--json]
```

**Subcommands**:

| Subcommand | What it does |
|------------|-------------|
| `model list` | List all registered audio models with backend and tags |
| `model status` | Show the configured model, resolved checkpoint, and whether it is loadable |
| `model activate <id>` | Select the active model and persist the state file |
| `excerpt-find` | Score audio files (or a directory) against a text query and emit an excerpt bundle |
| `read-bundle` | Pretty-print a previously emitted excerpt bundle |

Useful `excerpt-find` flags: `--text`, `--input`, `--model`, `--recursive`, `--top`, `--window-ms`, `--hop-ms`, `--min-score`, `--max-candidates-per-file`, `--bundle-out`, `--dry-run`. All subcommands accept `--json` for machine-readable output.

### sdk

**Status**: usable

Manage the pinned Pulp SDK installation at `~/.pulp/sdk*/` used by standalone projects. `pulp create` and `pulp build` already pull the SDK in transparently when needed; `pulp sdk` is the explicit control surface.

```bash
pulp sdk                                      # Show help
pulp sdk install                              # Download and cache the pinned SDK from GitHub releases
pulp sdk install --version 0.2.0              # Install a specific version
pulp sdk install --local                      # Build and install the SDK from the current Pulp checkout
pulp sdk status                               # Show cached and locally-built SDK versions
pulp sdk clean                                # Remove all cached SDK versions
```

Set `PULP_HOME` to relocate the SDK cache, asset cache, and config root.

### dev

**Status**: usable

Unified development loop. Combines `build --watch` with optional test, validate, and launch-an-app steps in a single command so you can keep one terminal open while iterating.

```bash
pulp dev                                      # Watch and rebuild
pulp dev --test                               # Watch, rebuild, run tests
pulp dev --test --test-filter=Knob            # Watch, rebuild, run tests matching Knob
pulp dev --test --validate                    # Watch, rebuild, test, and validate built plugins
pulp dev --run pulp-gain-standalone           # Watch, rebuild, relaunch app on each rebuild
pulp dev --design ui.js                       # Watch, rebuild pulp-design-tool, relaunch with ui.js
pulp dev --target pulp-format                 # Pass --target to cmake --build
pulp dev --run my-app -- --arg1 --arg2        # Arguments after `--` go to the launched binary
pulp dev --allow-unsupported-sdk             # Bypass the CLI-vs-project SDK guard (unsupported)
```

Flags:

| Flag | Description |
|------|-------------|
| `--test`, `-t` | Run tests after each successful build |
| `--test-filter=PATTERN` | Run only tests matching PATTERN (implies `--test`) |
| `--validate` | Run quick plugin dlopen validation after build |
| `--run TARGET` | Launch TARGET from the build dir; relaunch on rebuild |
| `--design SCRIPT` | Build `pulp-design-tool` and launch it with SCRIPT |
| `--target T` | Pass `--target T` to `cmake --build` |
| `--allow-unsupported-sdk` | Bypass the CLI-vs-project SDK compatibility guard and continue anyway (unsupported) |
| `-- args...` | Arguments passed to the launched app |

`pulp dev` runs the same active-project compatibility preflight as `pulp build`. If the project pins an SDK or `cli_min_version` newer than the installed CLI, the command stops before SDK resolution/build and points at `pulp upgrade`.

### loop

**Status**: experimental

Leveraged-prototype focus mode (issue [#940](https://github.com/danielraffel/pulp/issues/940)). `pulp loop` is the explicit "I'm in single-platform iteration mode" switch. It records the focus platform in `~/.pulp/config.toml` under `[loop]` so the user can leave the mode and return to cross-platform iteration deliberately, and drives the same watch + rebuild + screencap loop as `pulp dev` — but pinned to one platform's toolchain so the slow cross-platform configure paths (Skia/Dawn/threejs FetchContent) can be skipped when other platforms add unrelated cost.

```bash
pulp loop                           # Enter focus mode on the auto-detected host
pulp loop --platform=macos          # Pin to macOS explicitly
pulp loop --platform=linux --test   # Pin + run tests on every save
pulp loop --status                  # Print the current focus state
pulp loop --off                     # Restore cross-platform mode
pulp loop --watch-issues 924,927    # (Slice 3, deferred — issue #947)
pulp loop --ar-swap-from feat/x     # (Slice 2, deferred — issue #946)
```

Flags:

| Flag | Description |
|------|-------------|
| `--platform=<macos\|linux\|windows>` | Override the auto-detected host platform |
| `--off` | Restore cross-platform mode by clearing the focus marker |
| `--status` | Print the current focus state and exit |
| `--no-watch` | Persist focus state and exit without entering the watch loop |
| `--watch-issues N1,N2,...` | (Slice 3, deferred — [#947](https://github.com/danielraffel/pulp/issues/947)) Poll `gh pr list` for state flips |
| `--ar-swap-from <ref>` | (Slice 2, deferred — [#946](https://github.com/danielraffel/pulp/issues/946)) ABI-checked `.o` swap |
| `--test`, `-t` | Run tests after each successful build |
| `--test-filter=PATTERN` | Run only tests matching PATTERN (implies `--test`) |
| `--validate` | Run quick plugin dlopen validation after build |
| `--run TARGET` | Launch TARGET from build dir, relaunch on rebuild |
| `--target T` | Pass `--target T` to `cmake --build` |
| `--allow-unsupported-sdk` | Bypass the CLI-vs-project SDK compatibility guard |
| `-- args...` | Arguments passed to the launched app |

The CLI persists `[loop] focus_platform = "..."` in `~/.pulp/config.toml`. Run `pulp loop --off` (or pair with `shipyard pr` / `pulp pr`) before landing the consumer-side PR — the ship path validates cross-platform regardless, but exiting focus mode explicitly keeps subsequent local iteration honest.

See [docs/guides/focus-mode.md](../guides/focus-mode.md) for the full playbook (when to use, when not to, and how to file framework issues from a focus-mode session).

### scan

**Status**: usable

Walk the OS plug-in paths and print every VST3 / AU / AUv3 / CLAP / LV2 plug-in that was found. Mirrors what `pulp::host::PluginScanner` does at runtime. Useful for sanity-checking your local plug-in installation or for narrowing down which plug-in to feed to `pulp host`.

```bash
pulp scan                           # Scan every supported format the build includes
pulp scan --format clap             # Scan only CLAP
pulp scan --format vst3             # Only VST3
pulp scan --format au               # Only AU v2
pulp scan --format auv3             # Only AUv3
pulp scan --format lv2              # Only LV2
pulp scan -f clap                   # Short alias for --format
```

Output is one line per plug-in: `[<format>]` header per section, then `<name>  <bundle-path>`.

### host

**Status**: experimental

Load a plug-in out-of-DAW and run a short synthetic audio block through it. Smoke-tests the hosting pipeline without launching Logic, Reaper, or Ableton.

```bash
pulp host /path/to/MyPlugin.clap                       # Load a CLAP bundle
pulp host /path/to/MyPlugin.clap --format clap         # Explicit format
pulp host /path/to/MyPlugin.vst3 --format vst3
pulp host /path/to/MyPlugin.component --format au
pulp host /path/to/MyPlugin.lv2 --format lv2 --id https://example.com/plugins/my-plugin
pulp host -h                                           # Show help
```

Flags:

| Flag | Description |
|------|-------------|
| `--format <fmt>`, `-f <fmt>` | Format: `clap` (default), `vst3`, `au`, `auv3`, `lv2` |
| `--id <unique-id>` | Select a specific plug-in descriptor by URI / unique-id (used for LV2 and multi-plugin CLAP bundles) |

Prints plug-in metadata (name, vendor, version, format, parameter count) and the peak output level from a 256-sample synthetic block at 48 kHz. Exit code 0 on success, 1 if the bundle could not be loaded, 2 if `prepare()` failed.

### tool

**Status**: usable

Manage the third-party developer tools Pulp can optionally use (formatters, validators, importers). Tools are described in `tools/packages/tool-registry.json` and installed under `~/.pulp/tools/` — they are kept out of the system PATH so Pulp-managed installs can never clobber the system copy.

```bash
pulp tool                           # Show help
pulp tool list                      # Show every registered tool and its install state
pulp tool install clap-validator    # Download and install one tool
pulp tool install --all             # Install every tool available on the current platform
pulp tool install <id> --force      # Reinstall even if already present
pulp tool uninstall <id>            # Remove a pulp-managed tool install
pulp tool path <id>                 # Print the absolute path to the installed tool's binary
pulp tool run <id> [args...]        # Run the installed tool with pass-through arguments
pulp tool doctor                    # Health check: which tools are installed, which are missing, which are unavailable on this platform
```

Install methods come from the registry — today `binary_download` (pinned release artifact) and `python_pip` (pipx-style isolated install). `pulp tool doctor` is the per-platform companion to `pulp doctor`.

### upgrade

**Status**: usable

Update the Pulp CLI binary to the latest (or a specific) version.

```bash
pulp upgrade                                      # upgrade to latest release
pulp upgrade 0.2.0                                # install specific version
pulp upgrade --check-only                         # report cached latest release; no download
pulp upgrade --notes                              # print migration notes for installed -> cached latest
pulp upgrade --notes --json                       # same, stable-shape JSON (agent-consumable)
pulp upgrade --notes --from 0.25.0 --to 0.29.0    # explicit hop override
```

Downloads the release from GitHub, replaces the current binary, and verifies. Requires `curl`.

`--check-only` reads the on-disk cache written by the on-every-invocation background refresh (release-discovery #547 Slice 2) and prints installed/latest/notes. If the cache is empty (first run), it falls through to a single live GitHub query.

`--notes` is the Slice 3 (#548) surface: it filters the embedded migration index (built from `docs/migrations/*.md` at compile time) through each entry's `applies_if` expression and prints only the notes relevant to the upgrade hop. No network, no binary swap. The JSON variant emits a stable-shape document (`from`, `to`, `entries[].{version, breaking, summary, applies_if, body}`) that Slice 4's `/upgrade` Claude Code skill consumes.

### config

**Status**: usable

Read or write `~/.pulp/config.toml` settings. Release-discovery Slice 2 (#547) + Slice 5 (#550).

```bash
pulp config get update.mode
pulp config set update.mode manual
pulp config set update.check_interval_hours 12
pulp config list
```

Supported keys (all under `[update]`):

- `update.mode` — one of `auto | prompt | manual | off` (default `prompt`). Slice 5 (#550) wires all four modes into the invocation path:
    - `auto` — silently stages the new release via `~/.pulp/pending-upgrade`; the swap completes on the next invocation.
    - `prompt` — prints a one-line banner per new version; 24h snooze via `~/.pulp/update-snooze` respected if present.
    - `manual` — prints a one-line "Run `pulp upgrade` when you're ready" notice per new version; never prompts.
    - `off` — zero network calls, zero notices. Suitable for CI and air-gapped environments.

  Changing `update.mode` clears `~/.pulp/update-snooze` so the new mode takes effect on the next invocation.
- `update.check_interval_hours` — integer hours between background checks (default `24`). The 24h default stays under the 60/hour anonymous GitHub API rate limit by a wide margin.
- `update.channel` — `stable | beta` (default `stable`). Reserved for a future slice; ignored today.
- `update.bump_projects` — `prompt | auto | off` (default `prompt`). Controls whether a successful `pulp upgrade` nudges the user toward `pulp project bump --all`.

### clean

**Status**: usable

Remove the build directory.

```bash
pulp clean
```

### help

Print usage information.

```bash
pulp help
```

### version

**Status**: usable

Show, bump, or check version consistency across all surfaces (CMakeLists.txt, SDK constant, CHANGELOG, AU Info.plist).

```bash
pulp version                  # Show current SDK and project versions
pulp version bump patch       # Increment patch version
pulp version bump minor       # Increment minor version
pulp version bump major --plugin  # Bump plugin version (pulp_add_plugin VERSION)
pulp version check            # Verify version consistency
```

The `bump` subcommand updates `CMakeLists.txt project(VERSION)` and adds a CHANGELOG.md entry. The SDK version constant is derived from CMake via `configure_file`, so a rebuild picks up the change automatically. Use `--plugin` to bump the `pulp_add_plugin(... VERSION ...)` line instead.

The `check` subcommand verifies:
- SDK version constant matches CMakeLists.txt
- AU Info.plist template uses a computed version integer (not hardcoded)
- CHANGELOG latest heading matches CMakeLists.txt

### add

**Status**: usable

Add a third-party package from the Pulp package registry.

```bash
pulp add signalsmith-stretch                       # add a package
pulp add rtneural --license-override commercial    # accept a non-standard license
pulp add some-lib --platform-guard                 # add with platform guard
pulp add dr-libs --no-cmake                        # metadata only, skip CMake wiring
```

Performs license checking, platform compatibility analysis, overlap detection, CMake generation (`cmake/pulp-packages.cmake`), and updates `packages.lock.json`, `DEPENDENCIES.md`, and `NOTICE.md`.

### remove

**Status**: usable

Remove a previously added package.

```bash
pulp remove signalsmith-stretch
```

Cleans up the lock file, CMake declarations, and metadata entries.

### list

**Status**: usable

Show installed packages.

```bash
pulp list              # human-readable table
pulp list --json       # JSON output
```

### search

**Status**: usable

Search the package registry.

```bash
pulp search "pitch detection"
pulp search dsp
pulp search fft --format json
```

### update

**Status**: usable

Check for and apply package updates.

```bash
pulp update            # dry-run: show available updates
pulp update --apply    # apply updates and regenerate CMake
```

### suggest

**Status**: usable

Context-aware package recommendations.

```bash
pulp suggest --description "pitch shifting"
pulp suggest --analyze src/my_processor.cpp
pulp suggest --alternative pffft
```

### target

**Status**: usable

Manage project platform targets stored in `pulp.toml`.

```bash
pulp target list                  # show current targets
pulp target add Windows-arm64     # add a target
pulp target remove Linux-x64     # remove a target
```

Default targets (if none configured): `macOS-arm64`, `Windows-x64`, `Linux-x64`.

### audit (package extensions)

The existing `pulp audit` command now supports package-specific flags:

```bash
pulp audit --packages     # verify lock file integrity
pulp audit --platforms    # check package/platform coverage
pulp audit --licenses     # verify license compatibility
```

These flags are handled natively; without them, `pulp audit` delegates to the Python audit script as before.

## Global Flags

| Flag | Description |
|------|-------------|
| `--no-color` | Disable color output (also respects `NO_COLOR` env var) |

Color output is auto-detected based on TTY. Non-TTY environments (pipes, CI) get plain text automatically.

## Caveats

- Standalone projects are detected by walking up from the current directory looking for `pulp.toml` without `core/`.
- If both a standalone project and a parent Pulp repo are present, the standalone project wins.
- Pulp repo mode is detected by walking up from the current directory looking for a directory with both `CMakeLists.txt` and `core/`.
- The `ship` subcommands are macOS-specific (they use `codesign` and `pkgbuild`).
- `pulp upgrade` requires internet access and `curl` (macOS/Linux) or PowerShell (Windows).
