# CLI Reference

The `pulp` CLI wraps common build, test, validation, and shipping operations.

Source: `tools/cli/pulp_cli.cpp`

## Commands

### create

**Status**: usable

Create a new plugin project from templates. Checks environment, scaffolds source files, builds, and runs tests.

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
6. Configures, builds the test target, and runs tests
7. Reports plugin artifact locations

Default formats are platform-gated:
- **macOS**: VST3, AU, CLAP, Standalone
- **Linux**: VST3, CLAP, LV2, Standalone
- **Windows**: VST3, CLAP, Standalone
- **app/bare**: Standalone only

### build

**Status**: usable

Configure and build the project. Auto-detects when CMake reconfiguration is needed. Works with both repo-based and standalone projects.

```bash
pulp build                    # Build all targets
pulp build --target PulpGain_VST3  # Build specific target
pulp build -j8                # Parallel jobs
```

Extra arguments are passed through to `cmake --build`.

For standalone projects (detected via `pulp.toml`), automatically sets `CMAKE_PREFIX_PATH` to the hinted local SDK when available, otherwise to the cached SDK release.

### test

**Status**: usable

Run the test suite via CTest. Builds first if no build directory exists. Works with both repo-based and standalone projects.

```bash
pulp test                     # Run all tests
pulp test -R Gain             # Run tests matching "Gain"
```

Extra arguments are passed through to `ctest`.

### status

**Status**: usable

Show project information for either SDK mode or source-tree mode.

```bash
pulp status
```

`pulp status` reports which mode you are in so external projects never silently depend on a random checkout and repo/examples never silently pick up a cached SDK.
In SDK mode it also reports the pinned SDK version plus the resolved SDK path and checkout hints when present.

### validate

**Status**: usable

Run plugin format validators on all built plugins.

```bash
pulp validate              # CLAP + VST3 (pluginval) + AU
pulp validate --all        # Also run vstvalidator if installed
pulp validate --json       # Print JSON report to stdout
pulp validate --report out.json  # Write JSON report to file
```

Checks:
- **CLAP**: uses `clap-validator` if installed, otherwise falls back to CTest dlopen checks
- **VST3**: uses `pluginval` (strictness level 5, 30s timeout) if installed, otherwise skips
- **AU**: uses `auval` on macOS
- **VST3 (--all)**: also runs `vstvalidator` if installed (Steinberg SDK tool, optional)

Flags:
- `--all` — run every available validator, including vstvalidator
- `--json` — emit a machine-readable JSON report to stdout (conforms to `validation-report-v1.schema.json`)
- `--report <path>` — write the JSON report to a file

When a validator tool is not installed, the check is reported as SKIPPED with a clear message.
The JSON report conforms to `docs/contracts/validation-report-v1.schema.json`.

Prints a summary with pass/fail/skip counts.

### run

**Status**: usable

Launch a standalone Pulp application from the build directory.

```bash
pulp run                    # find and launch first standalone binary
pulp run PulpGain           # launch a specific target
pulp run MyApp -- --arg1    # pass arguments to the launched binary
```

Searches the active project's build output:
- standalone projects: `build/bin/`
- in-repo examples: `build/examples/`

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
pulp doctor             # show all checks
pulp doctor --fix       # auto-fix issues where possible
pulp doctor --ci        # non-interactive, exit codes only
pulp doctor --dry-run   # show what --fix would do
```

Checks are platform-gated — only relevant checks run on each OS:
- **macOS**: compiler, CMake, git-lfs, LFS files, VST3 SDK, AudioUnitSDK, build state
- **Linux**: compiler, CMake, git-lfs, LFS files, VST3 SDK, ALSA dev headers, build state
- **Windows**: compiler, CMake, git-lfs, LFS files, VST3 SDK, build state

Mode-specific checks:
- **SDK mode**: verifies `pulp.toml`, the installed SDK path or cache, optional checkout hints, and build configuration for the external project
- **Source-tree mode**: verifies the active checkout, pinned external SDKs, LFS state, and build configuration for the repo/examples workflow

Exit code is 0 if all checks pass, 1 if any fail.

### ci-local

**Status**: experimental

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

Artifacts are written by default under `planning/screenshots/design-debug/`:
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

### import-design

**Status**: experimental

Import designs from Figma, Stitch, v0, or Pencil source files into generated Pulp UI code.

```bash
pulp import-design --from figma --file frame.json
pulp import-design --from figma --url 'https://figma.com/design/...' --frame 'Plugin UI'
pulp import-design --from stitch --file screen.html --screen 'Main'
pulp import-design --from v0 --url 'https://v0.dev/t/abc123' --output ui.js
pulp import-design --from pencil --file ui.json --output ui.js --tokens tokens.json
pulp import-design --from v0 --file card.tsx --dry-run
```

Supports `--url` (fetches via curl), `--frame` (Figma frame selection), and `--screen` (Stitch screen selection). See [Design Import API Reference](design-import.md) for the full flag list.

### export-tokens

**Status**: experimental

Export a theme as W3C Design Tokens JSON.

```bash
pulp export-tokens --file theme.json --tokens tokens.json
pulp export-tokens --dry-run
```

### upgrade

**Status**: usable

Update the Pulp CLI binary to the latest (or a specific) version.

```bash
pulp upgrade              # upgrade to latest release
pulp upgrade 0.2.0        # install specific version
```

Downloads the release from GitHub, replaces the current binary, and verifies. Requires `curl`.

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
