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
pulp create "MPE Synth" --type instrument --mpe    # instrument with MPE support
pulp create "My App" --type app                    # standalone audio application
pulp create "My Project" --type bare               # minimal skeleton
pulp create "My FX" --manufacturer "Acme Audio"    # custom manufacturer
pulp create "My FX" --output ~/projects/my-fx      # custom output directory
pulp create "Android App" --type app --targets android --no-build # scaffold Android files
pulp create "Debug Knob" --in-tree                 # add an example under examples/
pulp create "Kit Gain" --template ./my-template-kit # scaffold from a local template kit
pulp create "Pinned FX" --pin                      # write an exact SDK version
pulp create "Debug FX" --debug                     # configure Debug instead of Release
pulp create "My FX" --no-build                     # scaffold only, skip build
pulp create "My FX" --no-interactive               # CI/scripting mode (no prompts)
```

Available types: `effect` (default), `instrument`, `app`, `bare`. Built-in templates are in `tools/templates/<type>/`.

`--template <name-or-kit-dir>` accepts either a built-in template name or an explicit local template kit directory containing `pulp.package.json`. Local template kits are validated before scaffolding, must declare kind `template`, and must export exactly one safe template directory. They do not execute package CMake, JavaScript, scripts, or dynamic libraries. If a template kit declares dependency packages, install those curated dependencies explicitly with `pulp add <id>` first; `pulp create` will not widen the trust boundary by adding them implicitly. A template kit only builds format targets for entry templates it exports, so a small CLAP/Standalone starter does not pretend to support VST3/AU.

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

The full create path is meant to behave the same from a normal terminal, CI, or agent-driven workflows. The CLI prints which mode it selected and why, while `--no-interactive`, `PULP_PROJECTS_DIR`, and `PULP_HOME` make project creation predictable for automation.

When the Rust front end runs its native `--ci` scaffolder instead of delegating to `pulp-cpp`, it creates files and prints the follow-up `pulp build` command, but it does not run doctor checks, SDK fetch/cache setup, project registration, configure, build, or ctest. Use the delegated C++ create path for the one-shot scaffold/build/test proof.

Mode truth:
- **SDK mode** means you are in an external project building against a pinned installed Pulp SDK artifact.
- **Source-tree mode** means you are inside the Pulp checkout building the repo or its examples against live source.
- `pulp create` defaults to SDK mode unless you explicitly ask for an in-tree example with `--in-tree`.

What the full create path does:
1. Runs `pulp doctor` checks (fails fast if environment is broken)
2. In standalone product mode: if run from inside a Pulp checkout, prepares pinned dependencies from that checkout and caches a local SDK install; otherwise downloads and caches the SDK release
3. Scaffolds source files from built-in templates or an explicitly named local template kit (processor, format entries, test, CMakeLists.txt)
4. In in-tree mode: adds the project to `examples/CMakeLists.txt`
5. In standalone product mode: generates `pulp.toml` with `sdk_version = "latest"` by default, an exact SDK version when `--pin` is passed, and local SDK hints when created from a checkout
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
`PULP_AAX_SDK_DIR` or auto-discovered in a standard user-local SDK path. Linux
and Ubuntu do not support AAX.

### build

**Status**: usable

Configure and build the project. Auto-detects when CMake reconfiguration is needed. Works with both repo-based and standalone projects.

```bash
pulp build                    # Build all targets
pulp build --target PulpGain_VST3  # Build specific target
pulp build -j8                # Parallel jobs
pulp build --watch            # Build and watch for changes
pulp build --watch --test     # Build, watch, run tests on change
pulp build --watch --test-filter=Gain # Watch and run matching tests on change
pulp build --watch --validate # Build, watch, run quick validation on change
pulp build --install          # macOS: validate, then install built plugin bundles
pulp build --install --skip-validation # macOS debug escape hatch; bypasses validation
pulp build --allow-unsupported-sdk # Bypass the CLI-vs-project SDK guard (unsupported)
pulp build --check-identity    # Verify .pulp/identity.lock before configure (Track 3.12)
pulp build --check-identity --allow-identity-change  # Treat identity drift as a warning
pulp build --js-engine=v8      # Force the JS engine backend and reconfigure
pulp build --format wam        # Build the WAMv2 (Emscripten) web plugin into build-wam/
pulp build --format wclap      # Build the WebCLAP (wasi-sdk) module into build-wclap/
pulp build -f wclap -j8        # Short form, with a cmake passthrough flag
```

Extra arguments are passed through to `cmake --build`.

`--format wam|wclap` (short: `-f`) builds a **web plugin format** instead of the native plugins, using a separate toolchain and build directory so it never collides with the native `build/`:

- `--format wam` configures with the Emscripten wrapper (`emcmake cmake`) into `build-wam/`. Requires `emcmake` on `PATH` — source your `emsdk_env.sh` first.
- `--format wclap` configures plain `cmake` with the wasi-sdk toolchain (`tools/cmake/wasi-toolchain.cmake`) into `build-wclap/`, producing a CLAP-in-WebAssembly `.wasm`. Errors clearly if the toolchain file is not in the checkout.

Web formats build to `.wasm`/`.js` and are not installed to native plug-in folders, so `--format` cannot be combined with `--install`, `--validate`, or `--watch`. See [web-plugin-support.md](web-plugin-support.md) for hosting the output.

`--check-identity` runs the same comparison as `pulp identity check` before the configure step, so a PR that changes an AU 4CC / VST3 FUID / CLAP id / AAX product code without re-recording the lock fails the build with a per-field diff. See `docs/reference/identity-lock.md`.

The `--watch` flag enters a file-watching loop after the initial build. It polls source files every 500ms and rebuilds on changes. Combine with `--test` to run tests after each successful rebuild and `--validate` to run quick dlopen checks.

On macOS, `--install` runs the strict validator gate before copying AU, VST3, and CLAP bundles into the user's plug-in folders. `--skip-validation` is only accepted together with `--install` and is intended for adapter debugging; `--install` cannot be combined with `--watch`.

For standalone projects (detected via `pulp.toml`), automatically sets `CMAKE_PREFIX_PATH` to the hinted local SDK when available, otherwise to the cached SDK release.
Before configure/build, `pulp build` also compares the active project's pinned `sdk_version` / `cli_min_version` against the running CLI. If the project is ahead, it fails fast and points at `pulp upgrade`; use `--allow-unsupported-sdk` only as an explicit unsupported escape hatch.

When `pulp build` decides a CMake reconfigure is required, it also runs the FetchContent cache preflight from `pulp doctor --caches` first. If the shared cache (`~/Library/Caches/Pulp/fetchcontent-src/` on macOS, `$XDG_CACHE_HOME/pulp/fetchcontent-src/` on Linux, `%LOCALAPPDATA%/Pulp/fetchcontent-src/` on Windows) contains a dangling symlink or stale-commit entry, `pulp build` aborts with a one-screen remediation message instead of letting `cmake` blow up 200 lines into the configure log. Run `pulp doctor --caches --fix` to heal user-owned drift, or set `PULP_SKIP_CACHE_PREFLIGHT=1` to bypass the gate.

On Windows, `pulp build` also selects a Visual Studio generator automatically when no active MSVC shell is detected on `PATH`.

### test

**Status**: usable

Run the test suite via CTest. Builds first if no build directory exists. Works with both repo-based and standalone projects.

```bash
pulp test                     # Run all tests
pulp test -R Gain             # Run tests matching "Gain"
```

Extra arguments are passed through to `ctest`.

When `pulp test` triggers a cold-start build (no `build/CMakeCache.txt`), the FetchContent cache preflight from `pulp doctor --caches` runs first and aborts with a clear remediation message on any unhealthy entry — same gate `pulp build` applies, same `PULP_SKIP_CACHE_PREFLIGHT=1` bypass.

### status

**Status**: usable

Show project information for either SDK mode or source-tree mode.

```bash
pulp status
```

`pulp status` reports which mode you are in so external projects never silently depend on a random checkout and repo/examples never silently pick up a cached SDK.
In SDK mode it also reports the pinned SDK version plus the resolved SDK path and checkout hints when present.
In source-tree mode it also reports the effective PR workflow (`shipyard`,
`github`, or `manual`) and whether the selected workflow's local tool is
available. Public Pulp installs do not install Shipyard or GitHub CLI; those
are contributor/source-checkout tools checked when the PR workflow needs them.
It also reports the effective `pulp import-design` defaults, including whether
they came from the built-in `live/js` default, `~/.pulp/config.toml`, or
`PULP_IMPORT_DESIGN_DEFAULT_*` environment overrides.
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
pulp validate --screenshot # Capture plugin editor PNGs under artifacts/screenshots/
pulp validate --target standalone ./build/MyApp.app  # Validate an explicit macOS app bundle
```

**Validator-discovery preflight.** Before launching any validator,
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
- `--strict` — treat skipped-because-missing-tool as a hard failure
- `--screenshot` — capture plugin editor PNGs under `artifacts/screenshots/` through `pulp::view::capture_view()`
- `--target <standalone|auv3|macho|all> <bundle...>` — run macOS runtime validators on explicit bundle paths instead of walking `build/{CLAP,VST3,AU,AAX}`

When a validator tool is not installed, the check is reported as SKIPPED with a clear message.
The JSON report conforms to `docs/contracts/validation-report-v1.schema.json`.
On Linux and Ubuntu, AAX validation is never attempted because AAX is unsupported there.

`--screenshot` is the project-facing batch capture path for built plugin
editors. It uses `capture_view()` so GPU-required views route through the GPU
capture backend, native-overlay-only views fail with an explicit reason instead
of producing a misleading blank PNG, and clear-only frames are rejected by the
screenshot content floor. This flag is best for validation artifacts; use
`pulp run --headless --screenshot <file>` when you need a one-shot standalone
capture.

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
pulp run --audio-inspector                          # open the live Audio Inspector window
pulp run --audio-probe-json probe.json              # dump live probe metrics as JSON, then exit
pulp run --audio-scope-json scope.json              # dump live scope acquisition/measurements JSON
```

Searches the active project's build output:
- standalone projects: `build/bin/`
- in-repo examples: `build/examples/`

`pulp run` launches a standalone host, so it may activate the system audio
output even when the UI is headless. The CLI prints a pre-launch notice by
default; set `PULP_RUN_AUDIO_NOTICE=0` only for deliberate quiet automation.
For no-speaker signal evidence, prefer the offline `HeadlessHost` / Audio
Doctor paths under `pulp audio validate`.

#### Headless / screenshot flags

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

#### Live Audio Inspector flags

The live Audio Inspector is a floating developer window that observes the
realtime output-boundary probe (peak/RMS/dBFS, clip/NaN counts, silence
runs). See the [Audio Inspector guide](../guides/audio-inspector.md) for
the full picture, including the in-app `Cmd/Ctrl+Shift+A` chord and the
dev-on/ship-off `PULP_ENABLE_AUDIO_PROBES` gating.

- `--audio-inspector` — open the live Audio Inspector window. Forwarded
  as `--audio-inspector` and via `PULP_AUDIO_INSPECTOR=1`. Does not imply
  `--headless` (a dev may want the visible window); composes with
  `--screenshot`, which then also captures the panel as
  `<stem>.audio-inspector.png`.
- `--audio-probe-json <file>` — the programmatic readout: after the
  frame delay, write the live probe's latest snapshot as a flat JSON
  object to `<file>`, then exit. Implies `--headless`. Forwarded as
  `--audio-probe-json <file>` and via `PULP_AUDIO_PROBE_JSON=<file>`.
  This is visually headless, but it still launches the standalone host and
  may open the live audio device. It is distinct from the offline
  `pulp audio validate` Doctor: the Inspector reads *live* metrics from a
  running host, the Doctor analyses a rendered WAV offline without speakers.
- `--audio-scope-json <file>` — the programmatic Scope readout: after the
  frame delay, write versioned `pulp.audio.scope.v1` JSON containing a copied
  sample-window acquisition plus measurements such as peak-to-peak, RMS, DC
  offset, crest factor, and conservative rising-zero frequency. Implies
  `--headless`, but still launches the standalone host and may open the live
  audio device. It cannot be combined with `--audio-inspector` because both are
  single consumers of the live capture FIFO.
- `--audio-scope-window <samples>` / `--audio-scope-trigger <none|raw|off|rising-zero>` /
  `--audio-scope-channel <index>` — acquisition controls for
  `--audio-scope-json`.
- `--audio-capture-wav <file>` — capture the live output to a WAV after the frame
  delay, then exit, so the offline `pulp audio validate` verbs can analyze it.
  `--audio-capture-frames <n>` sets the ring window (0 = as much as the ring
  holds). Implies `--headless` but still launches the live audio device, and
  shares the single capture FIFO with `--audio-inspector` / `--audio-scope-json`
  (the three are mutually exclusive). NOTE: this dumps the **earliest** window
  after the stream starts (int16), which is robust for `validate summarize` /
  `assert` (presence / level / clip / NaN); steady-state `doctor` (THD/response)
  and `compare` want `--audio-capture-rolling` instead.
- `--audio-capture-rolling <file>` — capture the live output to a **float** WAV
  after the frame delay, then exit. Unlike `--audio-capture-wav`, this keeps the
  **last** (steady-state) window — the window `validate doctor`/`compare` want —
  with no int16 quantization floor. `--audio-capture-rolling-frames <n>` sets the
  window (0 = as much as the ring holds). `--audio-capture-rolling-format
  float|int24` picks the sample format — **float** (default, full precision) or
  **int24** (integer, ≈ −144 dBFS floor, ~75% the size, universal DAW/tool
  compatibility). Implies `--headless` but still launches the live audio device.
  The standalone runs one capture mode per invocation, so this is mutually
  exclusive with `--audio-inspector`, `--audio-scope-json`, and
  `--audio-capture-wav`.

Display-only waveform controls:

- `PULP_AUDIO_INSPECTOR_TRIGGER=rising-zero` — opt into rising-zero-crossing
  trace alignment for a more stable visual display. Raw buffer display remains
  the default.
- `PULP_AUDIO_INSPECTOR_GRID=0` — hide the waveform grid.
- `PULP_AUDIO_INSPECTOR_SCALE=<n>` — zoom the horizontal window over the copied
  real samples (`1.0` shows the full captured buffer).

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

Diagnose environment issues. Checks C++20 compiler, CMake version, git-lfs, LFS file state, generated WidgetBridge API artifacts, external SDKs (VST3, AudioUnit), platform-specific dependencies, and the expected project mode.

```bash
pulp doctor                          # show all checks
pulp doctor --fix                    # auto-fix issues where possible
pulp doctor --ci                     # non-interactive, exit codes only
pulp doctor --dry-run                # show what --fix would do
pulp doctor --versions               # CLI/SDK/Plugin version diagnostics
pulp doctor --versions --scan-parents # ALSO walk CWD ancestors for pulp_add_* projects
pulp doctor --versions --json        # emit the diagnostic as stable JSON
pulp doctor --validators             # discover auval / pluginval / clap-validator + verify signatures
pulp doctor --validators --fix       # ALSO remove user-owned broken copies; root-owned breakage prints sudo one-liner
pulp doctor --validators --dry-run   # preview what --fix would do
pulp doctor --caches                 # FetchContent shared-source cache health
pulp doctor --caches --fix           # heal user-owned dangling/stale-commit entries
pulp doctor --caches --fix --dry-run # preview heal without removing anything
pulp doctor --caches --json          # emit the cache report as stable JSON
pulp doctor --host-quirks            # show the runtime DAW host-quirks policy + enforced accommodations
pulp doctor quirks                   # synonym for --host-quirks
pulp doctor --au-cache --dry-run     # preview macOS AudioComponentRegistrar refresh
pulp doctor --only WidgetBridge      # check generated WidgetBridge .d.ts/docs staleness
```

`pulp doctor --host-quirks` reports whether Pulp is enforcing DAW
host-quirk accommodations and under which policy. It prints the effective
tier policy (`off` / `validated-only` / `all`) and where it came from
(compile-time default, the `PULP_HOST_QUIRKS` env var, or a programmatic
`set_host_quirk_policy()` call), the detected host + version, and the list
of currently-enforced accommodations with their validation tier. The same
section is appended to the default `pulp doctor` output.

Override the policy at runtime without recompiling:

```bash
PULP_HOST_QUIRKS=off            pulp doctor --host-quirks  # disable all accommodations
PULP_HOST_QUIRKS=validated-only pulp doctor --host-quirks  # only bench-validated fixes
PULP_HOST_QUIRKS=all            pulp doctor --host-quirks  # every detected quirk (default)
```

Per-quirk provenance — `source_type`, `evidence`, and `last_verified`
dates — lives in `core/format/host-quirks.json`. See
[host-quirks policy](host-quirks-policy.md) for the full opt-in / opt-out
story and the precedence rules.

`pulp doctor --au-cache` refreshes macOS Audio Unit registration metadata by
stopping `AudioComponentRegistrar` so macOS respawns it on the next AU host scan.

In source-tree mode, the default doctor includes `WidgetBridge generated API`.
It verifies that `packages/pulp-react/src/bridge-globals.generated.d.ts`,
`packages/pulp-react/src/bridge-mock-functions.generated.ts`,
`packages/pulp-react/src/bridge-mock-safe-functions.generated.ts`, and
`docs/reference/js-bridge.md` exist and carry the current embedded input
fingerprint for the bridge manifest, generator, and capability inputs. Run
`python3 tools/scripts/generate_widget_bridge_api.py --check` for the exact
content check, or run `python3 tools/scripts/generate_widget_bridge_api.py --write`
manually to refresh stale outputs.
Use it after changing AU `Info.plist` metadata such as type, manufacturer, or
description when `auval` or a DAW still sees stale values. `--dry-run` prints
the command instead of running it. On non-macOS hosts the flag is accepted as a
no-op and exits 0 so cross-platform scripts do not need OS conditionals.

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

**Multi-project skew.** When `~/.pulp/projects.json`
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
`min_cli_version` field; absent in
older plugin builds.

#### `--validators`

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

**FetchContent cache health.** `pulp doctor --caches` audits
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

Manage the `~/.pulp/projects.json` registry. `pulp create` registers
new projects automatically on successful scaffold; these commands
exist so users can add projects created outside of `pulp create`
(clones, manual checkouts) and remove stale entries. Registry entries
are read by `pulp doctor --versions` to produce per-project skew
reports.

```bash
pulp projects list                       # show registered projects
pulp projects list --json                # machine-parseable JSON output
pulp projects add                        # register the current directory
pulp projects add ~/code/my-plugin       # register a specific directory
pulp projects remove ~/code/old-plugin   # forget a project by path
```

`--json` emits the same shape the Rust CLI port emits, so cross-binary consumers see byte-identical output. Schema: `{registry, projects: [{path, name, registered_at, missing_on_disk}]}`. `missing_on_disk` is `true` when the project directory has been deleted since registration.

The registry is a plain JSON file with one top-level `projects`
array; each entry has `path`, `name`, and `registered_at`. The
location is `$PULP_HOME/projects.json` (defaulting to
`~/.pulp/projects.json`). A missing registry file is treated as an
empty list — no first-run setup is required.

### project

**Status**: usable

Per-project SDK pin management. Pins a consumer project to a specific
Pulp SDK version, switches it back to floating mode, and records pin
undo batches at
`~/.pulp/bump-undo-<timestamp>.json` so mistakes are one command away
from recovery.

In standalone SDK-mode projects (`pulp.toml` present), the SDK pin is
`pulp.toml` `sdk_version`. `pulp project pin` updates that field and
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
pulp project pin                      # pin CWD project to CLI's own version
pulp project pin 0.32.0               # pin to explicit version (positional)
pulp project pin --to=0.32.0          # pin to explicit version (named)
pulp project pin --all                # iterate ~/.pulp/projects.json
pulp project pin --all --dry-run      # show plan without writing
pulp project pin --force-dirty        # skip the git-clean check
pulp project pin --allow-downgrade    # target older than current pin
pulp project pin --allow-cli-skew     # target newer than installed CLI
pulp project pin --allow-redundant    # ignore origin/main already-newer guard
pulp project pin --verify-builds      # build after pin; roll back on failure

pulp project unpin                    # set sdk_version = "latest"
pulp project unpin --dry-run          # show the unpin rewrite without writing

pulp project bump                     # deprecated alias for `pin`

pulp project undo                     # revert the newest batch
pulp project undo <timestamp>         # revert a specific batch
```

`pulp project bump` remains a deprecated alias for `pulp project pin`
through the compatibility window, but new docs and scripts should use
`pin`.

`pulp project unpin` preserves the `sdk_version` field and rewrites its
value to `"latest"`. That marker resolves to the newest installed SDK
under `~/.pulp/sdk/<x.y.z>/` on each rebuild.

**Cross-binary parity:** `pulp project pin` / `bump` and `pulp project undo` round-trip byte-exactly between the C++ and Rust CLI implementations. A pin written by one binary is correctly understood by the other binary's `undo`, including the optional `notes:[...]` field the Rust port emits. The C++ undo-batch parser silently skips unknown ARRAY / OBJECT fields it doesn't recognize so future schema additions don't desync the parser.

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

**Migration notes** print after a successful bump so users see any API
changes the hop introduced.

**When to use `pulp upgrade` vs `pulp project pin`:** use
`pulp upgrade` to replace the installed Pulp CLI/SDK toolchain. Use
`pulp project pin` after that when the current project should move to
that SDK. The Claude `/upgrade` flow exposes this as "upgrade the
tool" vs "upgrade the tool and bump this project's SDK pin".

**Post-upgrade hook:** the `update.bump_projects` config key (prompt
| auto | off; default prompt) controls whether `pulp upgrade` prints
a project-pin hint after a successful CLI upgrade.

### ci-host

**Status**: experimental

Optional, discoverable wrapper around `tools/ci/setup-ci-host.sh` for onboarding a
Mac as a Tart-VM CI host (install prereqs, create the local VM stores, register the
host-class runner label, optionally copy a golden in and run a one-shot validation
build). This is an **advanced/contributor path — never required**, and Shipyard
stays encouraged-not-mandated. The real work lives in the script; the command just
makes it discoverable and forwards flags.

```bash
pulp ci-host setup --class m5                       # minimum: register the m5 host-class label
pulp ci-host setup --class m5 --copy-from 'macstudio:/Volumes/Workshop/VMs/vms/pulp-build-runner:latest'
pulp ci-host setup --class m5 --validate            # also run a one-shot VM build to prove it
pulp ci-host setup --help                           # full flag list (delegated to the script)
```

Common flags (forwarded verbatim to `setup-ci-host.sh`):

- `--class <name>` — **required** host class for the runner label (`m5`, `studio`, `macbook`, …)
- `--copy-from <ssh:path | path>` — rsync a golden in from another host/drive (sparse-safe)
- `--validate` — after setup, run one ephemeral VM build on the host-only label
- `--no-agent` — do everything except install/load the launchd agent

Runs from inside a Pulp checkout (it resolves `tools/ci/setup-ci-host.sh`). For the
from-scratch host recipe and gotchas, see
[mac-ci-host-setup.md](../guides/mac-ci-host-setup.md) and the `tart-ci` skill.

### macos

**Status**: experimental

Retarget just the macOS leg of a PR without disturbing the Linux/Windows matrix.
This is an operator command for cases where a PR should move between local,
Namespace, or GitHub-hosted macOS capacity.

```bash
pulp macos status --pr 1910
pulp macos retarget --pr 1910 --to local
pulp macos retarget --pr 1910 --to namespace
pulp macos retarget --pr 1910 --to github-hosted
```

`retarget` cancels in-flight macOS-bearing runs for that PR and dispatches
`build-macos.yml` on the selected runner pool. Branch protection is satisfied by
the latest workflow that publishes the required `macos` check. See
[local-ci.md](../guides/local-ci.md#per-pr-macos-retargeting-pulp-macos) for the
runner variables and operator workflow.

### overflow

**Status**: experimental

Configure the macOS overflow routing variables read by `build.yml`. This is a
repo-operator surface for deciding where new macOS jobs go when local capacity is
busy; it does not cancel in-flight jobs.

```bash
pulp overflow status
pulp overflow enable
pulp overflow enable --to '"namespace-profile-generouscorp-macos"'
pulp overflow disable
pulp overflow threshold
pulp overflow threshold 1
```

`enable` sets the overflow target, `disable` removes it for future dispatches,
and `threshold` gets or sets the busy-run count that trips overflow. See
[local-ci.md](../guides/local-ci.md#pulp-overflow-operator-surface) for the
exact repository variables and rollback notes.

### ci-local

**Status**: legacy (prefer [Shipyard](https://github.com/danielraffel/Shipyard))

> **Note:** For most CI workflows, use `shipyard run` for validation and `shipyard pr` for PR creation/shipping/tracking instead of `pulp ci-local`. Shipyard is Pulp's primary CI tool and provides the same target matrix with evidence-gated merges. `pulp ci-local` remains available as an advanced fallback while legacy workflows are still supported.

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

### harness

Run the catalog coverage harness and deterministic visual snapshot harness. Coverage mode delegates to `tools/harness/verifier.py` and compares `compat.json` support claims against machine-derived oracles. Visual mode delegates to `tools/harness/visual/runner.py` and compares fixture-declared JSON or PNG captures against checked-in goldens.

```bash
pulp harness --surface=yoga
pulp harness coverage --surface=yoga
pulp harness --all
pulp harness --surface=yoga --json
pulp harness --surface=yoga --no-docs
pulp harness visual --verify --all
pulp harness visual --verify --all --actuals-dir build/visual-actuals
pulp harness visual --generate --surface=yoga --entry=yoga/box-sizing
```

By default, coverage mode writes `build/harness-coverage-<sha>.json`, `build/harness-coverage.md`, and `docs/reports/harness-coverage.md`. Use `--json` for stdout-only machine output. Coverage JSON includes `validation_routes` per surface for supported entries with a typed validation route, legacy test reference, or explicit exclusion. It also includes `visual_coverage`, counted from typed runtime fixture refs that resolve to checked-in goldens; `visual_pass` remains as a compatibility alias.

Visual mode requires the `pulp-test-visual` target to be built first. Use `--build-dir` or `--binary` when the binary is not under the default `build/` or `build-visual/` directories. Fixture metadata decides whether a golden is semantic JSON or raster PNG. JSON snapshots use tolerance-aware semantic diffs; PNG snapshots use exact-byte comparison on the canonical raster lane. When verification fails, pass `--actuals-dir build/visual-actuals` to write failed actual JSON/PNG captures under `build/visual-actuals/<surface>/<fixture>.<json|png>` for inspection or artifact upload.

### bake

Freeze a graph into a signed, distributable `.pulpbake` artifact, and verify one.

```bash
pulp bake myrack.pulpgraph -o myrack.pulpbake --sign-key signing.key
pulp bake verify myrack.pulpbake --trust signing.key
```

`bake` loads the `.pulpgraph`, prepares it, freezes it through the bake lowering,
and signs it with Ed25519. A graph that isn't self-contained (a hosted plugin
node, a MIDI/automation/sidechain lane, or a non-opted-in Custom type) is refused
with the specific reason and a non-zero exit. `verify` checks the signature and
runs the bounded parse against a `--trust` key set — it never executes the plan
or loads custom state. Key handling reuses the reload-trust key file, the same
path `ship swap-pack` uses.

### ship

**Status**: experimental

Signing and packaging subcommands.

```bash
pulp ship sign --identity "Developer ID Application: ..."
pulp ship sign --identity "..." --entitlements path/to/entitlements.plist
pulp ship sign --identity "..." --path MyApp.app   # sign one explicit artifact
pulp ship package --version 1.0.0
pulp ship check
pulp ship swap-pack --bundle ui/ --plugin-id com.you.synth   # sign a hot-reload UX bundle (caps inferred; key from keychain)
pulp ship doctor                                   # make signing non-interactive (no keychain/1Password prompt)
pulp ship notarize --path MyApp-1.0.dmg --api-key ~/key.p8 --api-key-id ABC --api-issuer <uuid>
pulp ship notarize --path MyApp-1.0.dmg            # notarize + staple one artifact
pulp ship notarize --dry-run                       # print resolved argv, no submit
pulp ship release --pkg --identity "..." --installer-identity "..."
pulp ship share MyApp.app --identity "..."         # one-shot: sign+notarize+verify
pulp ship appcast --url https://example.com/MyApp-1.0.pkg --version 1.0.0
pulp ship appcast --url artifacts/MyApp-1.0.pkg --download-url https://example.com/MyApp-1.0.pkg --sign-key <base64-key>
pulp ship auv3-xcodeproj MyPlugin --sdk iphonesimulator --dry-run
```

**Subcommands**:

| Subcommand | What it does |
|------------|-------------|
| `sign`     | Code-sign all built plugin bundles (VST3, CLAP, AU), or one `--path` artifact |
| `notarize` | Submit packaged artifacts to Apple notarytool (macOS); prefer `release` or `--path` with `.pkg`, `.dmg`, or `.zip` |
| `package`  | Create macOS `.pkg`/`.dmg`, Windows NSIS, Linux `.deb`/`.tar.gz`, or Android APK/AAB packages in `artifacts/` |
| `release`  | macOS one-command pipeline: sign → package → **notarize the .pkg/.dmg it builds** → staple |
| `share`    | One-shot for sharing a single artifact: sign → wrap `.app` in DMG → notarize → staple → Gatekeeper-verify |
| `appcast`  | Generate a Sparkle-compatible appcast feed from a package URL or local artifact |
| `auv3-xcodeproj` | Generate an Xcode project for an AUv3 target (macOS) |
| `check`    | Check signing status of built desktop plugins or Android APK/AAB artifacts |
| `doctor`   | Make signing+notarization non-interactive (no keychain/1Password prompt): self-heal the dedicated signing keychain and validate the file-based `.p8` notary key. Run automatically as a best-effort preflight by `sign`. |

`doctor` materializes a dedicated signing keychain authorized for `codesign` (so the login keychain / 1Password is never consulted) and validates a file-based App Store Connect `.p8` notary key. `--check-online` also proves the `.p8` against Apple (read-only) and refreshes the optional `pulp-notary` keychain profile; `--print-env` emits resolved identity/keychain handles (no secret values). Secrets live in `~/.config/pulp/secrets/` (`keychain.env` + `notary.env`), never in the repo; same-named env vars override the files. No build directory is required.

`sign` requires `--identity`. The default entitlements file is `ship/templates/entitlements.plist`.
`--path` signs exactly one explicit desktop artifact instead of scanning the build dirs:
macOS `.app`/`.dmg`/plugin bundles, or Windows `.exe`/plugin bundles. `.pkg`
installers are signed at creation time with a Developer ID **Installer**
identity, not here.

`package` creates per-format `.pkg` files using `pkgbuild` on macOS, or `.dmg` files with `--dmg`. On Windows, it packages VST3/CLAP bundles as an NSIS `.exe` installer; `--per-user` switches plugin destinations to `%LOCALAPPDATA%\Programs\Common\...`, and plugin-only installers do not create Start Menu shortcuts. On Linux, it packages VST3/CLAP/LV2 bundles as a `.deb` using `dpkg-deb`, with a `.tar.gz` fallback when `dpkg-deb` is unavailable. If no Linux plugin bundles are present, it reports `no VST3/CLAP/LV2 plugins found` instead of creating an empty macOS-style artifact summary. For Android, `--target android` runs the Gradle package flow and copies APK/AAB outputs into `artifacts/`.

For notarization, prefer `pulp ship release` for the end-to-end sign/package/notarize flow, or `pulp ship notarize --path <artifact>` for one packaged upload container (`.pkg`, `.dmg`, or `.zip`). Raw `.app` bundles are rejected with a pointer to `share`; raw plugin bundle directories should be packaged before distribution.

`appcast` writes `artifacts/appcast.xml` by default, or the path passed with `--output`. It appends the newest item to an existing feed when one parses, defaults `--version` to `0.1.0`, accepts optional `--notes`, `--title`, and `--min-os`, and records a local artifact's file size when `--url` points at a readable path. `--download-url` overrides the enclosure URL written to the feed, so a local artifact can be signed while Sparkle downloads from the public URL. The file served from `--download-url` must be byte-identical to the local artifact passed as `--url`, because the feed length and Ed25519 signature are computed from the local bytes. `--sign-key` computes a Sparkle Ed25519 signature only for local artifact paths; remote URLs fail closed instead of emitting an unsigned feed that looks signed.

#### `pulp ship share` — one-off "sign it for a friend"

`share` is the opinionated, single-command path for handing a build to someone
without running the full release pipeline. Point it at a `.app`, `.dmg`, or
`.pkg`:

```bash
pulp ship share MyApp.app --identity "Developer ID Application: Name (TEAMID)"
pulp ship share MyApp.app --identity "..." --output dist --entitlements entitlements.plist
pulp ship share MyApp.app --dry-run        # print the plan, do nothing
```

For a `.app` it code-signs (hardened runtime + secure timestamp), wraps it in a
DMG under `artifacts/`, signs the DMG, notarizes + staples it, then runs the
exact `spctl -a -t open --context context:primary-signature` check Gatekeeper
performs on download. A green result means the recipient will **not** see
"Unnotarized Developer ID". Notarization credentials resolve through the same
chain as `pulp ship notarize` (App Store Connect API key preferred). `.dmg`
inputs skip the wrap step; `.pkg` inputs are assumed already installer-signed
and are only notarized + verified.

For `.app` inputs, use `--output <dir>` to choose where the generated DMG lands
instead of `artifacts/`, and `--entitlements <plist>` to override the default
app-signing entitlements.

`release --dmg`/`--pkg` notarizes and staples the distributable it produces, so
the artifact it leaves in `artifacts/` is Gatekeeper-ready, not merely signed.

`auv3-xcodeproj` generates a separate CMake Xcode build directory for a project
that contains an AUv3 target. `--sdk` accepts `iphonesimulator`, `iphoneos`, or
`macosx`; default output is `build/xcode/<target>-<sdk>`. The generated build
hint targets `<target>_AUv3`. Use `--dry-run` to print the CMake invocation and
build hint without requiring Xcode. For the macOS lane, the generated project
also contains the runnable containing-app target `<target>_AUv3Host`.

#### `pulp ship notarize`

Two credential lanes are supported. The App Store Connect API key flow is
preferred (Apple's modern, scope-controlled path); the legacy Apple-ID +
app-specific-password flow remains as a fallback for existing users.

**Preferred — App Store Connect API key** (`xcrun notarytool submit
--key/--key-id/--issuer`):

| Flag             | Env var                  | notary.env key             |
|------------------|--------------------------|----------------------------|
| `--api-key`      | `PULP_NOTARY_KEY_PATH`   | `PULP_NOTARY_KEY_PATH`     |
| `--api-key-id`   | `PULP_NOTARY_KEY_ID`     | `PULP_NOTARY_KEY_ID`       |
| `--api-issuer`   | `PULP_NOTARY_ISSUER_ID`  | `PULP_NOTARY_ISSUER_ID`    |

**Legacy** (`xcrun notarytool submit --apple-id/--team-id/--password`):

| Flag            | Env var          | config.toml             |
|-----------------|------------------|-------------------------|
| `--apple-id`    | `PULP_APPLE_ID`  | `signing.apple.apple_id`|
| `--team-id`     | `PULP_TEAM_ID`   | `signing.apple.team_id` |
| `--password`    | —                | `signing.apple.password` (default `@keychain:AC_PASSWORD`) |

**Resolution precedence** (highest wins): CLI flag → environment variable →
`~/.config/pulp/secrets/notary.env` (override path via `PULP_NOTARY_ENV` or
`--env-file <path>`) → `~/.pulp/config.toml` (legacy fields only).

The ASC lane wins when all three pieces resolve. Otherwise the legacy lane
applies when `--apple-id` + `--team-id` resolve. If neither lane is complete,
the command prints both setup recipes and exits non-zero.

Other flags: `--staple` (skip submission, staple already-notarized bundles),
`--dry-run` (print the resolved `xcrun notarytool` argv and exit 0; never
contacts Apple — useful in CI and for verifying credential resolution).

### pr

**Status**: usable

Create, validate, and merge a PR through the canonical ship flow.

`shipyard pr` is the primary "ship this" orchestrator referenced by the CI
skill. `pulp pr` remains a compatibility wrapper that delegates to
`shipyard pr` by default, with explicit `github` and `manual` workflows for
humans who opt out of Shipyard in their local checkout. Use `--native` only
for diagnostics when debugging the CLI-side fallback path. Natural-language
triggers in agent conversations
("push to main", "ship this", "ship it", "we're done", "merge this",
"push it", "run CI", "push a PR") all route here — see the
[CI skill](../../.agents/skills/ci/SKILL.md) (`.agents/skills/ci/SKILL.md`)
for the authoritative trigger list.

```bash
shipyard pr
pulp pr
pulp pr --base origin/main
pulp pr --title "feat(cli): document pulp pr and sync CI policy"
pulp pr --workflow github
pulp pr --workflow manual
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
| `--workflow <m>` | One-shot workflow override: `shipyard`, `github`, or `manual` |
| `--no-ship` | Diagnostics-only native fallback flag; do not use as the normal PR path |
| `--no-push` | Stop after bump commit; do not push or create PR |
| `--dry-run` | Print the plan without executing steps |
| `-h`, `--help` | Show help |

Notes:

- Default behavior is shim delegation to `shipyard pr` — the single source
  of truth for ship orchestration. Do not treat `gh pr create` + `shipyard ship`
  as a substitute; that sequence bypasses the skill-sync and version-bump
  gates and can leave a PR outside Shipyard's tracked state.
- The PR workflow is resolved in this order: `--workflow`, then
  `PULP_PR_WORKFLOW`, then `~/.pulp/config.toml` `[pr] workflow`, then the
  default `shipyard`.
- `github` means direct GitHub CLI mode through `gh`; it requires an installed
  and authenticated `gh` and does not create Shipyard tracking state.
- `manual` prints the intended commands and exits before pushing or creating a
  PR. It is for people who want to use the GitHub UI, forks, or other tooling.
- `shipyard` is intentionally not silently downgraded to `github` when the
  Shipyard binary is missing. Install the pinned source-checkout tool with
  `./tools/install-shipyard.sh`, or choose another workflow explicitly.
- `--native` runs an in-CLI fallback that performs the same gates + PR flow
  without delegating to Shipyard. Diagnostic use only.
- Direct `gh pr create` is an explicit emergency/manual bypass only. If used,
  document the Shipyard tracking gap and reconcile by resuming or re-shipping
  through Shipyard when possible.
- For the canonical list of natural-language ship triggers and the full
  policy, see the [CI skill](../../.agents/skills/ci/SKILL.md)
  (`.agents/skills/ci/SKILL.md`).

### docs

**Status**: usable

Browse local documentation and status manifests. Reader subcommands read from local files in `docs/` only -- no web calls. Build subcommands invoke the local docs tooling.

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
pulp docs build-site              # Generate the static docs site
pulp docs build-api               # Generate API reference docs
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
| `build-site` | Generate the static docs site through MkDocs |
| `build-api` | Generate API reference docs through Doxygen |

### design

**Status**: experimental

Launch the local AI-powered design tool used for token, shader, and style iteration.

```bash
pulp design
pulp design path/to/design-tool-core.js
pulp design --script path/to/design-tool-core.js
pulp design --build-dir /tmp/pulp-design-parity-build
```

`pulp design` now configures/builds `pulp-design-tool` on demand before launch. When run inside
a Pulp checkout it loads `examples/design-tool/design-tool-core.js` from that checkout and builds
into that checkout's `build/` directory. The design-tool UI is split across `design-tool-*.js`
concern modules; the entry module is the path you pass, and the host loads its sibling modules in
order from the same directory.

Use `--script` to point at a different JS entry, and `--build-dir` when you are working from a
nonstandard build tree such as a separate worktree build directory.

When run outside a Pulp checkout, `pulp design` can currently auto-bind only when the CLI
binary lives inside a Pulp build tree such as `.../build/pulp` (Rust) with its
`.../build/tools/cli/pulp-cpp` delegate. Generic PATH-installed or symlinked CLI setups
are not fully SDK-mode aware yet; use `--build-dir` and `--script` explicitly in split
layouts where the project repo and the Pulp SDK live in different directories.

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

Connect to a running Pulp inspector server. With no `--port`, the CLI
auto-discovers the newest `pulp-inspector-*.port` file in the system temp
directory.

```bash
pulp inspect
pulp inspect --port 49152
pulp inspect --command DOM.getDocument
pulp inspect --command State.getParameters
```

Options:

- `--host HOST` - connect to a host other than `127.0.0.1`
- `--port PORT` - connect to an explicit inspector port
- `--command METHOD` - send one inspector command and print the response
- `--params JSON` - JSON params for `--command`
- `--output FILE` - write a one-shot command response to a file

`Runtime.evaluate`, `Capture.screenshot`, and `Capture.screenshotNode` are
reserved inspector protocol methods, but currently return explicit unavailable
errors until script-engine and host-capture references are wired into the
inspector domain.

### motion

**Status**: experimental

Agent-facing wrappers around the inspector `Motion.*` protocol. Start the
host with `PULP_MOTION_SERVER=1`, then use `pulp motion` to record traces,
inspect active traces, replay `.motion.jsonl` fixtures, and toggle motion-cost
sampling from a terminal.

```bash
pulp motion record --view Card --out card-fade.motion.jsonl
pulp motion stop --trace-id 1
pulp motion snapshot
pulp motion list-traces
pulp motion load-fixture card-fade.motion.jsonl
pulp motion scrub 30
pulp motion play
pulp motion pause
pulp motion cost enable
pulp motion cost disable
```

Options:

- `--host HOST` - inspector host, defaulting to `127.0.0.1`
- `--port PORT` - inspector port; defaults to auto-discovery from the same temp-file hint as `pulp inspect`
- `--json` - emit JSON where the subcommand supports it

Subcommands:

| Subcommand | Inspector method | Description |
|------------|------------------|-------------|
| `record [--view NAME] [--out FILE] [--fps N] [--metrics SPEC]` | `Motion.startTrace` | Start a trace and print the trace id. `--out` names the intended fixture path and prints sink guidance; the CLI does not write JSONL itself. |
| `stop [--trace-id N]` | `Motion.stopTrace` | Release an active trace. |
| `snapshot` | `Motion.snapshot` | Print tracing, active-trace, emitted-event, and cost-attribution state. |
| `list-traces` | `Motion.listTraces` | List inspector-owned trace ids. |
| `load-fixture PATH` | `Motion.loadFixture` | Load a `.motion.jsonl` fixture into the scrubber. |
| `scrub FRAME` | `Motion.scrubTo` | Move the scrubber playhead to a frame. |
| `play` / `pause` | `Motion.play` / `Motion.pause` | Control fixture playback. |
| `cost enable` / `cost disable` | `Motion.enableCost` / `Motion.disableCost` | Toggle the cost-attribution channel for the session. |

See [Motion Observability](../guides/motion-observability.md) for the full
runtime trace, fixture replay, and cost-attribution workflow.

### trace

**Status**: experimental

Agent-facing wrappers around the inspector `Trace.*` Perfetto-tracing
protocol. Start the host with `PULP_TRACE_SERVER=1`, then use `pulp trace` to
start/stop a tracing session, run SQL over the captured `.pftrace`, run L0
preset queries, or ask for a one-shot narrated root cause. Motion tells you
*what changed* on screen; tracing tells you *where the time went*.

```bash
pulp trace start --categories dsp,render --out /tmp/x.pftrace
pulp trace stop                                   # → prints the .pftrace path
pulp trace query "SELECT name, dur FROM slice ORDER BY dur DESC LIMIT 20"
pulp trace query "SELECT count(*) FROM slice" --trace /tmp/x.pftrace   # offline, no live session
pulp trace query --preset dsp-hotspots
pulp trace slowest-frames
pulp trace xruns
pulp trace layout-vs-paint
pulp trace snapshot
pulp trace explain "why is my plugin slow to open?"
pulp trace doctor                                 # readiness: inspector + build + trace_processor
pulp trace fetch                                  # download the pinned trace_processor (zero-install offline query)
pulp trace open /tmp/x.pftrace                    # serve on loopback + open in the Perfetto UI
```

Options:

- `--port PORT` - inspector port; defaults to `9147` / `$PULP_INSPECTOR_PORT`
- `--json` - emit the raw inspector JSON response instead of the pretty form

Subcommands:

| Subcommand | Inspector method | Description |
|------------|------------------|-------------|
| `start [--categories LIST] [--out FILE.pftrace] [--ring-mb N]` | `Trace.startSession` | Begin a session recording the selected span categories into an in-process ring. |
| `stop` | `Trace.stopSession` | Flush the session and print the `.pftrace` path. |
| `query "<sql>" [--format json\|table\|csv]` | `Trace.query` | Run SQL over the live captured trace; JSON by default. |
| `query "<sql>" --trace FILE.pftrace` | `trace_processor` (offline) | Run SQL against a flushed `.pftrace` without a live session, via `trace_processor_shell` (`$PULP_TRACE_PROCESSOR` → pinned Pulp-fetched build → `$PATH`; see `pulp trace fetch` / `doctor`). Returns trace_processor's native table; `--format`/`--preset` are live-path only. |
| `query --preset <name>` | `Trace.query` | Run a named trace-stdlib preset. |
| `slowest-frames` | `Trace.query` | L0 preset: frames over the vsync budget, worst first. |
| `xruns` | `Trace.query` | L0 preset: audio xrun / deadline-miss events. |
| `dsp-hotspots` | `Trace.query` | L0 preset: per-node DSP cost, most expensive first. |
| `layout-vs-paint` | `Trace.query` | L0 preset: one-row-per-category frame cost split. |
| `snapshot` | `Trace.snapshot` | Print `{tracing_active, categories, ring_bytes, out_path}`. |
| `explain "<question>"` | `Trace.explain` | One-shot narrated root cause + chain of evidence + fix. |
| `doctor` | client-side + `Trace.snapshot` | Readiness check. Aggregates inspector reachability and `trace_processor` availability (`$PULP_TRACE_PROCESSOR` → pinned Pulp-fetched build → `$PATH`) with the inspector's `compiled_in` / `active` / `last_trace_path`, then reports `ready_to_capture` and `ready_to_query`. `--json` emits the flat readiness object. |
| `fetch` | client-side | Download + SHA-256-verify the pinned `trace_processor_shell` (Perfetto v57.2) into `$PULP_HOME` so offline `query --trace` works zero-install. Idempotent (no-op when present). `--json` emits `{version, platform, path, already_present}`. |
| `open <file.pftrace> [--no-browser] [--keep-alive-seconds N]` | client-side | Serve the trace from a loopback-only HTTP server and open it in the Perfetto UI via `?url=` (browsers block `file://`). `--no-browser` prints the URLs to paste; `--keep-alive-seconds` bounds how long the server waits for the UI to fetch. `--json` emits `{trace_path, serve_url, perfetto_url, browser_opened, served}`. |

The span category taxonomy is `dsp`, `dsp.node`, `render`, `layout`, `canvas`,
`text`, `js`, `gpu`, `state`, `io`. Tracing is a dev-only tool: never ship a
plugin with `PULP_TRACING` enabled.

### tweaks

**Status**: experimental

Inspect the `pulp-tweaks.json` sidecar the inspector overlay writes for
direct-manipulation edits. Tweaks are keyed by `stable_anchor_id`; after a
design re-import an anchor may no longer exist in the live tree, so the
tweak silently stops applying. `pulp tweaks diff` surfaces this — it is the
CLI mirror of the inspector's drift drawer, sharing the same drift-detection
logic underneath.

```bash
pulp tweaks diff
pulp tweaks diff --tweaks pulp-tweaks.json --design design.json
pulp tweaks diff --design design.json --json
```

`tweaks diff` classifies every stored tweak against a design snapshot:

- **clean** - the anchor (and property) still resolve
- **drifted** - the anchor survives but the targeted property is gone
- **orphaned** - the anchor itself is gone (re-import removed the element)

The design snapshot comes from `--design FILE`, a small JSON "anchors
manifest" in any of three shapes:

```json
["anchor-a", "anchor-b"]
```

```json
{ "anchors": ["anchor-a", "anchor-b"] }
```

```json
{ "anchors": { "anchor-a": ["paint.color", "layout.padding"] } }
```

The first two forms enable anchor-only matching (orphan detection); the
third (anchor → property-path map) additionally enables property-level
drift detection. With `--design` omitted, the snapshot is empty and every
tweak is reported as orphaned.

Options:

- `--tweaks FILE` - path to `pulp-tweaks.json` (default: auto-resolved project sidecar)
- `--design FILE` - anchors-manifest JSON to diff against (default: empty)
- `--json` - emit the drift report as JSON instead of human-readable text

Exit code: `0` when no drift, `1` when drift is found, `2` on a usage or
file error.

### import-design

**Status**: partial

Import designs from local Figma `.fig` files, Figma REST/file JSON, the Pulp
Figma plugin, Stitch, v0, Pencil, Claude Design, React JSX, or Google DESIGN.md
source files into generated Pulp UI code.

```bash
pulp import-design --from fig --file design.fig --outline
pulp import-design --from fig --file design.fig --frame 'Plugin UI' --output ui.js
pulp import-design --from figma --file frame.json
pulp import-design --from figma --url 'https://figma.com/design/...' --frame 'Plugin UI'
pulp import-design --from figma-plugin --file design.pulp.zip
pulp import-design --from stitch --file screen.html --screen 'Main'
pulp import-design --from v0 --url 'https://v0.dev/t/abc123' --output ui.js
pulp import-design --from pencil --file ui.json --output ui.js --tokens tokens.json
pulp import-design --from v0 --file card.tsx --dry-run
pulp import-design --from claude --file design.html --classnames classnames.json
pulp import-design --from designmd --file DESIGN.md --tokens out.json
pulp import-design --from jsx --file bundle.js --mode live --emit js --output live-ui.js
pulp import-design --from jsx --file bundle.js --mode baked --emit cpp --output imported_ui.cpp
```

Accepted `--from` values: `fig`, `figma`, `figma-plugin`, `stitch`, `v0`, `pencil`, `claude`, `designmd`, `jsx`.

Supports `--url` (fetched through an argv-safe `curl` invocation into a unique temporary file), `--frame` (Figma frame selection; required guid or name for `--from fig` unless using `--outline`), `--outline` / `--page` for local `.fig` files, and `--screen` (Stitch screen selection). See [Design Import API Reference](design-import.md) for the full flag list.

For `--from claude`, the CLI emits a `classnames.json` artifact alongside the generated JS view and `tokens.json`. The artifact maps `classname → { cssProp(camelCase): cssValue, ... }` for every `<style>` rule with a plain classname selector — `@pulp/css-adapt` (and downstream) consumes it to merge class-based styles into inline before forwarding to bridge calls. Mirrors the output shape of Spectr's `tools/extract-html-bundle/extract.mjs`.

For `--from designmd`, the CLI emits **only** a `tokens.json` (W3C
DTCG) — no `ui.js`, because DESIGN.md describes a design system, not
a screen. See [Import: DESIGN.md](imports/designmd.md) for the full
contract (supported subset, reference resolution, detection rules,
exit codes, diagnostics, and current limitations).

| Flag | Description |
|------|-------------|
| `--output <path>` | Destination for the primary generated artifact. The built-in default primary artifact is live JS at `ui.js`; sidecars remain anchored beside this path when not explicitly overridden. |
| `--emit {js\|ir-json\|cpp\|swiftui}` | Select the primary artifact kind. `js`, `ir-json`, `cpp`, and `swiftui` are implemented; `cpp` and `swiftui` require `--mode baked`. `swiftui` emits a baked native SwiftUI view (`ImportedPulpView.swift` + a per-view `<RootView>Theme.swift` + binding manifest). Built-in default: `js`; persistent default: `import_design.default_emit`. |
| `--mode {live\|baked}` | Select the import runtime model. Built-in default: `live`; persistent default: `import_design.default_mode`. `baked` emits canonical IR or baked C++ via `--emit ir-json\|cpp`. |
| `--snapshot-semantics {fail\|warn\|accept}` | JSX baked snapshot policy. `fail` rejects dynamic APIs by default, `warn` proceeds with diagnostics, and `accept` proceeds silently. |
| `--allow-network-fetch` | Allow DesignIR asset-manifest HTTP(S) fetches at import time. |
| `--asset-cache <path>` | Asset cache directory for HTTP(S) imports. Defaults to `PULP_IMPORT_ASSET_CACHE` or the user cache. |
| `--asset-timeout-ms <ms>` | Per-request network asset timeout. |
| `--asset-hash <uri=sha256>` | Expected content hash for an asset URI; may be repeated. |
| `--classnames <path>` | Where to write the classname artifact (default: `classnames.json`). Only emitted for `--from claude`. |
| `--emit classnames` | Force-emit `classnames.json` (default on for `--from claude`). |
| `--no-emit-classnames` | Skip the classname artifact for the run. |
| `--tokens <path>` | Output token file (default: `tokens.json`; `theme.css` for `--format css-variables`). |
| `--screenshot-backend {skia\|coregraphics}` | `--validate` render backend. `skia` (default) composites file-backed images; `coregraphics` draws an image's filename placeholder, so it is not faithful for asset-rich designs. |
| `--knob-style {silver\|sprite\|auto\|standard\|default}` | Knob rendering mode. The default is the native silver/vector path; `sprite` opts into PNG sprite skinning. |
| `--fader-style {skin\|skinned\|default\|plain}` | Fader rendering mode. The default is derived skinning; `default` and `plain` opt out to the unskinned native look. |
| `--meter-style {skin\|skinned\|default\|plain}` | Meter rendering mode. The default is derived skinning; `default` and `plain` opt out to the unskinned native look. |
| `--format {w3c\|css-variables\|tailwind\|json-tailwind\|css-tailwind}` | Token export format. `w3c` (DTCG JSON) is the default; `css-variables` emits CSS custom properties (`.dark` modes → `@media (prefers-color-scheme: dark)`); the `tailwind` variants require `--from designmd`. Unknown values exit 2. |

With `--emit ir-json`, relative asset references from a `--url` import resolve
against the source URL. The manifest keeps the authored relative URI and also
records the resolved `source_url` used for HTTP(S) fetching.

The shipped default remains live runtime import: `--mode live --emit js`.
Persist different defaults with:

```bash
pulp config set import_design.default_mode baked
pulp config set import_design.default_emit ir-json
```

Set `import_design.default_emit cpp` for baked C++ by default. If only
`import_design.default_mode baked` is set, `ir-json` is implied. For temporary
session overrides, use `PULP_IMPORT_DESIGN_DEFAULT_MODE` and
`PULP_IMPORT_DESIGN_DEFAULT_EMIT`; direct CLI flags override the matching
config and environment value. `pulp status` shows the effective defaults.

With `--from jsx --mode live --emit js`, the CLI writes the precompiled JSX
runtime bundle verbatim for runtime import. That pass-through path rejects
`--validate`, `--reference`, `--diff`, and `--debug` because it does not parse or
render the bundle. With `--from jsx --mode baked --emit ir-json|cpp`, the CLI
captures a runtime snapshot into DesignIR and records snapshot provenance. DOM
bundles are captured through the DOM walker; live/native bundles that render
through `@pulp/react` are frozen from the native `WidgetBridge` tree and record
`runtime_native_snapshot` plus `snapshotSource: native-view`.
Dynamic APIs such as `setInterval`, `setTimeout`, `requestAnimationFrame`,
`Date.now`, `new Date`, `performance.now`, `Math.random`, and `fetch` fail by
default under `--snapshot-semantics fail`; comments and string literals are
ignored. Use `warn` to continue with a structured diagnostic, or `accept` to
continue without that diagnostic.

### export-tokens

**Status**: experimental

Export a theme as W3C Design Tokens JSON.

```bash
pulp export-tokens --file theme.json --tokens tokens.json
pulp export-tokens --dry-run
```

### audio

**Status**: experimental

Repo-level audio analysis tooling. Manages offline audio model metadata, reads reproducible excerpt bundles, and runs live/offline Audio Scope analysis. The current `excerpt-find` path is a WAV-first deterministic/null-backend ranking scaffold for dataset and evaluation workflows; it does not run semantic text/audio embedding inference yet. This is developer tooling for building datasets and evaluation corpora — not a runtime API.

```bash
pulp audio                                      # Show help
pulp audio model list [--json]                  # List registered models
pulp audio model status [--json]                # Show configured + resolved model
pulp audio model activate <model-id> [--json]   # Activate an installed model
pulp audio excerpt-find --text "warm analog pad" --input /path/to/wavs [options]
pulp audio read-bundle <path-to-bundle> [--json]
pulp audio scope [target] --window 2048 --trigger rising-zero --channel 0 [--json scope.json]
pulp audio scope --input-wav tone.wav --window 2048 [--json scope.json] [--png scope.png]
pulp audio validate summarize <file.wav> [--json]
pulp audio validate doctor <file.wav> [--thd] [--response f1,f2,...] [--fundamental <hz>]
pulp audio validate compare <a.wav> <b.wav> [--mode null|spectral] [--tolerance <dbfs>]
pulp audio validate assert <audio-run-dir-or-assertions.json>
pulp audio compare <reference.wav> <candidate.wav> [--profile tonal-balance|added-hf|noise-roughness|graininess|stereo-width|transient-integrity] [--reference-role peer|golden] [--align none|latency|varispeed:R|stretch:R|pitch:S|ratio:auto] [--threshold <t>] [--json report.json]
pulp audio render --plugin <bundle> --out <file.wav> (--duration-ms <n> | --duration-frames <n>) [options]
```

**Subcommands**:

| Subcommand | What it does |
|------------|-------------|
| `model list` | List all registered audio models with backend and tags |
| `model status` | Show the configured model, resolved checkpoint, and whether it is loadable |
| `model activate <id>` | Activate an installed audio model and persist the state file |
| `excerpt-find` | Rank WAV windows deterministically from a text query, then emit an excerpt bundle with backend metadata |
| `read-bundle` | Pretty-print a previously emitted excerpt bundle |
| `scope` | Capture `pulp.audio.scope.v1` JSON from a live standalone target or a speakerless offline WAV; offline mode can also write a PNG trace artifact |
| `validate summarize` | Decode a WAV and print an agent-readable signal summary (peak/RMS/DC/dominant pitch); `--json` for machine output |
| `validate doctor` | Offline Audio Doctor over a WAV: THD/THD+N (`--thd`) and/or spectrum magnitude at checkpoints (`--response`); writes a JSON curve artifact |
| `validate compare` | Sample-residual (null) verdict between two WAVs; exits nonzero past tolerance. `--mode spectral` currently applies a looser default tolerance to the same residual (a true spectral-distance metric is a later slice) |
| `validate assert` | Re-check a stored `assertions.json` (or an `audio-run/` dir holding one); exits nonzero on any failing assertion |
| `compare` | Advisory, agent-facing before/after judgment between two WAVs (measure → compare → judge). Delegates to the opt-in [Audio Quality Lab](../guides/audio-quality-lab.md) tool (no DSP links into the CLI); level-matches, runs one `--profile` axis (`tonal-balance` \| `added-hf` \| `noise-roughness` \| `graininess` \| `stereo-width` \| `transient-integrity`), prints a typed evidence envelope + verdict. Exits nonzero **only** when it could not measure (invalid), never for a judgment — distinct from the pass/fail `validate compare` gate. Prints an install hint + exit 1 when the tool is absent |
| `render` | Offline scenario render of a plugin bundle through the host slot (no DAW, no audio device): load `--plugin`, drive it from declarative flags, write a WAV, and emit metrics JSON matching `validate summarize --json`. `--latency-report` additionally **proves** the plugin's reported latency against the delay actually in its audio |

Useful `excerpt-find` flags: `--text`, `--input`, `--model`, `--recursive`, `--top`, `--window-ms`, `--hop-ms`, `--min-score`, `--max-candidates-per-file`, `--bundle-out`, `--dry-run`. Inputs are WAV files or directories of WAV files today; unsupported files are reported as skipped. The `model`/`excerpt-find`/`read-bundle` subcommands accept `--json` for machine-readable output.

The `validate` subcommands are the offline analysis CLI over captured audio. They analyze decoded WAV files and re-check `assertions.json` manifests (or directories containing one) with the reusable `pulp::audio-analysis` library — they do **not** instantiate a plugin (the generic CLI is not tied to a `Processor`; controlled-stimulus render is the test-side `RenderScenario`). The `assertions.json` schema is a `{"schema_version", "assertions": [...]}` document where each entry names a `check` (`not_silent`, `silent`, `no_nan_inf`, `peak_below`, `frequency_near`), a `file` (relative to the JSON), and the check's named tolerance.

`render` is the offline counterpart that *does* load a plugin: it takes an explicit `--plugin <bundle>` (the generic CLI has no registered factory, so a bundle is the only render source), drives it through `pulp::host::PluginSlot` block-by-block from declarative flags, writes an int16 WAV, and emits the same `pulp::audio-analysis` metrics JSON as `validate summarize --json` (`--manifest <file>` to a file, `--json` to stdout). Drive it with `--input-signal silence|sine:<hz>[,<dbfs>]|noise[:<seed>]|impulse[:<frame>]` or `--input <file.wav>` (used as-is at `--sample-rate`; no resampling — a rate mismatch shifts pitch), `--param <id>=<value>[@frame]`, and `--midi note:<note>,<vel>,<on>[,<off>]`. **`--param` values are in the PLAIN parameter domain** (the parameter's native `min..max`), **not normalized `[0,1]`** — matching `PluginSlot::set_parameter` / `ParameterEvent::value`; an `@frame` suffix delivers the change **sample-accurately** at that frame. The per-block parameter queue is forwarded straight to `PluginSlot::process`, which every loader applies at the event's sample offset — CLAP (`clap_event_param_value` at `header.time`), VST3 (`IParameterChanges` add-point), AU (`AudioUnitScheduleParameters` buffer offset); LV2 applies it block-rate, which is LV2's control-port contract. (A plugin that itself reads its parameters once per block will still step at block boundaries — that is the plugin's own rate, not the CLI's.) The render uses the `--in-channels`/`--out-channels` bus widths you specify (like `pulp host`); use `--in-channels 0` for instrument/no-input renders, and use at least one input channel for `--input` or sine `--input-signal`. The metrics JSON is computed from the float render — it matches the int16 WAV except below the ~−96 dBFS int16 floor, and at clipping the command warns that the float peak exceeds the hard-clamped file.

#### Proving reported latency (`--latency-report`)

> For when to reach for this, when *not* to, and how far to trust the numbers,
> see the [latency-proof guide](../guides/latency-proof.md). Most plugins have
> zero latency and do not need any of this.

A plugin with nonzero latency tells the host a number, and the host slides the
whole track by it to keep everything in the session aligned. Nothing checks the
number. When the true delay and the reported delay drift apart — an FFT size
changes, a filter stage is added, an oversampling ratio is tweaked — the plugin
silently misaligns against every other track, and no build or test fails.

`--latency-report <file.json>` proves the claim: it measures the delay actually
present in the rendered output and compares it against the plugin's
`latency_samples()`.

```bash
# A bypassed plugin's output should be its input, delayed by exactly the
# latency it reports. Param 3 is PulpGain's bypass; use your own plugin's.
pulp audio render --plugin My.clap --out /tmp/o.wav --duration-ms 500 \
    --input-signal noise --param 3=1 --latency-report /tmp/latency.json
```

Two policies:

- **`delayed-null`** (the default with `noise`) nulls the output against the
  input delayed by *D*, sweeping *D*. It checks **every sample**, so a
  one-sample misreport fails. It requires the plugin to be in a declared
  pass-through / bypass / fully-dry mode for the render — arrange that with
  `--param`.
- **`marker`** (the default with `impulse`) finds the single onset in the output
  and subtracts its position in the input. Weaker (one onset, not every sample),
  but it works for plugins that *reshape* the signal — a convolver, a filter —
  where nulling against the input is meaningless. Declare any delay the plugin
  adds that is **not** latency (leading silence in a known IR) with
  `--latency-intrinsic <n>`, so it is not charged to the latency report.

**It never guesses.** A stimulus that cannot pin the delay down comes back
`inconclusive`, never `match`: silence, a tone whose period is a whole number of
samples (delays one period apart produce identical audio), an output that is not
a delayed copy of the input at all, a non-unique marker, or a plugin whose report
*moved* mid-render. The report itself is kept separate from the measurement and
from the verdict — a plugin that reports zero because its **format cannot expose
latency at all** (Pulp's hosted LV2 slot) is `unsupported`, which is not the same
claim as "reports zero" and is never shown as verified.

**The artifact's existence is not the result.** The command exits **nonzero**
when the claim is disproven *or* when it was asked for and could not be proven —
an unprovable claim is a failed claim. Gate on the exit code, not on whether the
file appeared. The same evidence, with the same verdict, is what
`pulp_audio_render` returns over MCP with `latency: true`.

**How much to trust a pass.** The artifact carries the two numbers the verdict
was drawn from: `null_depth_db` (how completely the delayed input explained the
output — a pure delay line reaches the -200 dB floor; an STFT that reconstructs
the signal nulls to about -137 dB) and `ambiguity_margin_db` (how much worse the
best *competing* delay scored). A pass with a 2 dB margin cleared the bar but is
one small change away from being a coin flip; read the numbers, don't just read
the verdict.

**Pinning the intended value (`--latency-expect <n>`).** By default the proof is
*self-consistency*: the audio is delayed by exactly what the plugin reports,
which is all the host needs. It will not catch a plugin whose true delay **and**
report both grew together — someone doubles an FFT size and the plugin honestly
reports its new, larger latency. It is correctly compensated; it just got slower.
`--latency-expect` pins the value the plugin is *supposed* to have, so that drift
fails too. It never masks a real mismatch: if the audio and the report already
disagree, that is what you are told.

`pulp audio scope` is the lower-level sample-window view. Live mode wraps
`pulp run --audio-scope-json` and may open the audio device; use
`--input-wav <path>` for speakerless offline analysis. Both paths emit the same
versioned JSON schema so CLI, MCP, and plugin agents can compare live and
offline captures without duplicating trigger or measurement logic. `--png` is
offline-only and writes a deterministic trace image of the acquired real
samples.

### sdk

**Status**: usable

Manage the pinned Pulp SDK installation at `~/.pulp/sdk*/` used by standalone projects. `pulp create` and `pulp build` already pull the SDK in transparently when needed; `pulp sdk` is the explicit control surface.

```bash
pulp sdk                                      # Show help
pulp sdk install                              # Download and cache the pinned SDK from GitHub releases
pulp sdk install --version 0.2.0              # Install a specific version
pulp sdk install --local                      # Build and install the SDK from the current Pulp checkout
pulp sdk available                            # List SDK versions available on GitHub releases
pulp sdk status                               # Show cached and locally-built SDK versions
pulp sdk clean                                # Remove all cached SDK versions
```

Set `PULP_HOME` to relocate the SDK cache, asset cache, and config root.

### dev

**Status**: usable

Unified development loop. Combines `build --watch` with optional test, validate, and launch-an-app steps in a single command so you can keep one terminal open while iterating.

The live watch/relaunch loop is implemented by the C++ delegate (`pulp-cpp`).
Normal installed/source builds ship the Rust `pulp` front end with that sibling
delegate, so `pulp dev` forwards to the full watch loop when `pulp-cpp` is
available. If the delegate is unavailable or fallthrough is disabled, the Rust
fallback runs one configure/build pass, optionally runs tests, optionally
launches once, and prints a watch-loop stub notice instead of watching for
changes. The `--validate` and `--allow-unsupported-sdk` dev-loop behavior is
therefore part of the delegated C++ path.

```bash
pulp dev                                      # Watch and rebuild
pulp dev --test                               # Watch, rebuild, run tests
pulp dev --test --test-filter=Knob            # Watch, rebuild, run tests matching Knob
pulp dev --test --validate                    # Watch, rebuild, test, and validate built plugins
pulp dev --run pulp-gain-standalone           # Watch, rebuild, relaunch app on each rebuild
pulp dev --hot-dsp --run my-reloadable-standalone  # Watch, rebuild, live DSP hot-swap (no relaunch)
pulp dev --design ui.js                       # Watch, rebuild pulp-design-tool, relaunch with ui.js
pulp dev --target pulp-format                 # Pass --target to cmake --build
pulp dev --run my-app -- --arg1 --arg2        # Arguments after `--` go to the launched binary
pulp dev --allow-unsupported-sdk             # Bypass the CLI-vs-project SDK guard (unsupported)
```

Flags:

| Flag | Description |
|------|-------------|
| `--test`, `-t` | Run tests after each successful watch build, or after the Rust fallback's one build pass |
| `--test-filter=PATTERN` | Run only tests matching PATTERN (implies `--test`) |
| `--validate` | Delegated C++ path: run quick plugin dlopen validation after build |
| `--run TARGET` | Launch TARGET from the build dir; delegated watch mode relaunches on rebuild, Rust fallback launches once |
| `--hot-dsp` | With `--run`: keep the launched app alive across rebuilds so its `ReloadableShell` watcher hot-swaps the rebuilt DSP logic library in place instead of a process relaunch (live DSP dev loop — edit → audible without losing audio/UI state). Requires `--run`. |
| `--design SCRIPT` | Build `pulp-design-tool` and launch it with SCRIPT; delegated watch mode relaunches on rebuild, Rust fallback launches once |
| `--target T` | Pass `--target T` to `cmake --build` |
| `--allow-unsupported-sdk` | Delegated C++ path: bypass the CLI-vs-project SDK compatibility guard and continue anyway (unsupported) |
| `-- args...` | Arguments passed to the launched app |

`pulp dev` runs the same active-project compatibility preflight as `pulp build`. If the project pins an SDK or `cli_min_version` newer than the installed CLI, the command stops before SDK resolution/build and points at `pulp upgrade`.

### loop

**Status**: experimental

Leveraged-prototype focus mode. `pulp loop` is the explicit "I'm in single-platform iteration mode" marker. It records the focus platform in `~/.pulp/config.toml` under `[loop]` so the user can leave the mode and return to cross-platform iteration deliberately, then runs the normal watch + rebuild loop using the current project's build configuration. Surrounding tooling can read the advisory focus marker when it needs platform-specific behavior; `pulp loop` itself does not rewrite the build graph.

```bash
pulp loop                           # Enter focus mode on the auto-detected host
pulp loop --platform=macos          # Mark macOS focus explicitly
pulp loop --platform=linux --test   # Mark Linux focus + run tests on every save
pulp loop --status                  # Print the current focus state
pulp loop --off                     # Restore cross-platform mode
```

Flags:

| Flag | Description |
|------|-------------|
| `--platform=<macos\|linux\|windows>` | Override the auto-detected focus marker |
| `--off` | Restore cross-platform mode by clearing the focus marker |
| `--status` | Print the current focus state and exit |
| `--no-watch` | Persist focus state and exit without entering the watch loop |
| `--watch-issues N1,N2,...` | Recognized compatibility flag; prints a diagnostic and continues the normal loop unless `--no-watch` is also passed |
| `--ar-swap-from <ref>` | Recognized compatibility flag; prints a diagnostic and continues the normal loop unless `--no-watch` is also passed |
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

Walk the OS plug-in paths and print every VST3 / AU / AUv3 / CLAP / LV2 plug-in bundle that was found. The installed Rust `pulp scan` path is a filesystem inventory: it does not dlopen plug-ins or query factories, so names are filename-derived and vendor / version / unique-id metadata is not surfaced. The C++ delegate (`pulp-cpp scan`) still owns the rich `pulp::host::PluginScanner` metadata path.

```bash
pulp scan                           # Filesystem inventory for every supported format
pulp scan --format clap             # Scan only CLAP
pulp scan --format vst3             # Only VST3
pulp scan --format au               # Only AU v2
pulp scan --format auv3             # Only AUv3
pulp scan --format lv2              # Only LV2
pulp scan -f clap                   # Short alias for --format
pulp scan --no-load                 # Compatibility no-op on Rust; filesystem-only mode for pulp-cpp
pulp scan --help                    # Print usage; never opens any plug-in
```

Output is one line per plug-in: `[<format>]` header per section, then `<name>  <bundle-path>`.

On the Rust front end, `--no-load` is accepted for compatibility and is effectively the default behavior. On `pulp-cpp scan`, `--no-load` skips the dlopen step entirely and uses the same filename-derived inventory mode; use it when the rich path errors out with `libc++abi: terminating` or when you want a quick path-only listing.

`pulp scan --help` is handled by the Rust CLI help path and does not enumerate or load plug-ins. `pulp-cpp scan --help` has the same pre-scan help behavior, so help remains safe while diagnosing a malformed plug-in that crashes the rich metadata path.

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

### import

**Status**: experimental

Read an existing audio-plugin project read-only and emit a Pulp migration scaffold. Framework importers are vendor-specific add-on tools that live in their own private repos; the Pulp SDK owns only the generalized substrate — a discovery index of known frameworks, a JSON-over-stdio service-provider interface (SPI) to drive an installed importer, and the emission step (the SDK writes files; the importer only proposes a plan).

The command is **vendor-agnostic**: framework identity is runtime DATA loaded from `tools/import/known-frameworks.json`, the one place real source-framework markers appear. The SDK code names no framework and no vendor.

```bash
pulp import detect ./MyProject                                  # Rank candidates; print install hint
pulp import ./MyProject                                         # Alias for detect
pulp import inspect --from <framework> ./MyProject -o ir.json   # Resolve importer → SPI analyze → ProjectIR
pulp import inspect --from <framework> ./MyProject --importer-cmd "python3 spi.py"
pulp import emit --from <framework> ./MyProject --output ./scaffold
```

Subcommands:

| Subcommand | Description |
|------------|-------------|
| `detect <dir>` | Scan the directory against the known-frameworks markers and print ranked candidates (framework id + confidence + evidence) plus the install hint for the top match. Works with **no importer installed**. |
| `inspect --from <fw> <dir>` | Resolve the importer (tool registry or `--importer-cmd`) and run its SPI `analyze` verb to produce a ProjectIR. When no importer is resolvable, prints the install hint and exits non-zero. ProjectIR can include `integration_requirements` for optional packages, SDK/provider options, and source assets the scaffold needs to preserve. |
| `emit --from <fw> <dir> --output <out>` | Resolves the importer, runs its SPI `analyze` then `emit` verbs to get an **EmissionManifest**, then the **SDK writes** a buildable Pulp migration scaffold under `<out>`. The importer only proposes files (generated/stub carry inline content; verbatim portable-core copies and safe source assets carry an absolute `copy_from`); the SDK materialises them, runs a clean-room output scan over every generated file, and writes `migration_status.json` + a `.pulp-import-provenance.json` marker. Skewed/symmetric source parameter curves emit as shaped `ParamRange`s (skew + symmetric fields), no longer downgraded to linear. |

`inspect` / `emit` flags:

| Flag | Description |
|------|-------------|
| `--from <framework>` | Framework id (see `pulp import detect`) |
| `--framework-path <path>` | The user's own framework checkout (read-only; never vendored) |
| `--extra-include <dir>` | Extra include directory passed to the importer (repeatable) |
| `-o`, `--output-ir <file>` | `inspect`: write the ProjectIR JSON to a file |
| `--report <file.md>` | `inspect`: write a human-readable report |
| `--output <dir>` | `emit`: scaffold output directory |
| `--importer-cmd <cmd>` | Override importer resolution with an explicit command string |
| `--accept-importer-terms` | Accept the importer's terms of use non-interactively (CI). The acceptance is still recorded under `~/.pulp`. |

**Accept-to-run terms gate.** Before `inspect` / `emit` drives an importer, the user must give explicit affirmative acceptance of that importer's terms of use (mirrors `pulp add --accept-license`). The terms body is runtime DATA carried by the add-on importer (the SDK names no vendor and ships no terms body of its own); the SDK surfaces it, then records acceptance under `~/.pulp/importer-terms-accepted.json` keyed by importer id + a hash of the terms text. A changed terms version (new hash) re-prompts. Interactively the gate is a type-to-accept prompt; in CI pass `--accept-importer-terms`. Without a terminal and without the flag, the gate blocks with a non-zero exit rather than hanging. An importer that declares no terms passes the gate transparently.

The importer is resolved against `tools/packages/tool-registry.json`: an importer tool declares the `frameworks` it handles plus `spi_min` / `spi_max` (the SPI version window) and `sdk_min` / `sdk_max`. The SDK negotiates the SPI version on every call and fails loudly on a mismatch ("upgrade Pulp" / "upgrade the importer") rather than misbehaving silently. The data contracts are `tools/import/schemas/project-import-ir-v0.schema.json` and `tools/import/schemas/import-spi-v0.schema.json`.

**Who writes what (clean-room boundary).** The importer is a separate add-on and never writes into the user's tree — it returns an EmissionManifest over the SPI `emit` verb. The **SDK** writes every file, and before writing each `generated` file it runs a clean-room output denylist scan (sourced from the known-frameworks content markers) that rejects framework source or vendor banners; a `copied-user-file` is the user's own DSP, copied verbatim and recorded in provenance, so it is exempt. A misbehaving importer therefore cannot smuggle framework code into the scaffold. The SDK also writes `migration_status.json` (the migration verdict + unresolved notes) and `.pulp-import-provenance.json` (importer id, framework, SPI version, emit timestamp, source-tree hash, per-file provenance).

### identity

**Status**: experimental

Manage `.pulp/identity.lock` — the committed pin of each plugin's AU 4CC, manufacturer code, AAX product code, optional VST3 FUID, and optional CLAP plugin id. The lock is the audit trail that "this plugin's host-visible identity has not silently changed". See [docs/reference/identity-lock.md](identity-lock.md) for the schema and Track 3.12 of the macOS plugin-authoring plan for the rationale.

```bash
pulp identity record                              # Write/refresh .pulp/identity.lock
pulp identity record --allow-identity-change      # Accept drift and overwrite the lock
pulp identity record --dry-run                    # Print what would be written
pulp identity check                               # Compare lock vs project, exit 1 on drift
pulp identity check --allow-identity-change       # Treat drift as success
```

The same check is wired into `pulp build --check-identity` so CI can fail any PR that changes a host-visible identity field without an explicit `pulp identity record --allow-identity-change` step.

### tool

**Status**: usable

Manage the third-party developer tools Pulp can optionally use (formatters, validators, importers). Tools are described in `tools/packages/tool-registry.json` and installed under `~/.pulp/tools/` — they are kept out of the system PATH so Pulp-managed installs can never clobber the system copy.

```bash
pulp tool                           # Show help
pulp tool list                      # Show every registered tool and its install state
pulp tool info video-proof          # Show one tool's install/package metadata
pulp tool info video-proof --json   # Emit the same metadata as JSON
pulp tool install clap-validator    # Download and install one tool
pulp tool install --all             # Install every tool available on the current platform
pulp tool install <id> --force      # Reinstall even if already present
pulp tool install <id> --version <v>  # Install and pin to a user-chosen version (durable override)
pulp tool install <importer>        # Install a framework importer add-on (checksummed, version-window-checked)
pulp tool install <importer> --from <path|file://...>  # Install from a local package (offline / pinned artifact)
pulp tool update <id>               # Re-install a managed tool at the latest registry pin (clears a prior override)
pulp tool update <id> --version <v> # Update and pin to a user-chosen version (durable override)
pulp tool uninstall <id>            # Remove a pulp-managed tool, or an importer (also removes its skill)
pulp tool path <id>                 # Print the absolute path to the installed tool's binary
pulp tool run <id> [args...]        # Run the installed tool with pass-through arguments
pulp tool doctor                    # Health check: which tools are installed, which are missing, which are unavailable on this platform
pulp tool doctor <id> [--run]       # Check one tool; --run executes the resolved tool path with no args

pulp add <importer>                 # Alias for `pulp tool install <importer>`
```

Install methods come from the registry — today `binary_download` (pinned release artifact), `python_pip` (pipx-style isolated install), `npm_package` (repo-local npm wrapper installed under `~/.pulp/tools/npm-packages/<id>/`), and `importer_package` (a checksummed, per-platform framework-importer archive). `pulp tool doctor` is the per-platform companion to `pulp doctor`.

**Updating and overriding a tool's version.** Every `managed_by_pulp` tool is user-updatable and version-overridable without waiting for Pulp to bump its committed registry pin. `pulp tool update <id>` re-installs the tool at the registry pin (the latest known-good version Pulp ships) and clears any prior user override; `pulp tool update <id> --version <v>` (or `pulp tool install <id> --version <v>`) re-installs at an explicit version and records it as a durable override. The active version resolves by precedence, highest first: the `PULP_TOOL_<ID>_VERSION` env var (session-scoped; id upper-cased with non-alphanumerics turned into `_`), then the durable override in `$PULP_HOME/tool-overrides.json`, then the registry `pinned_version`. `pulp tool info <id>` (and `--json`, as `active_version` / `active_version_source`) reports which version is active and where it came from. This "added tools stay user-updatable + overridable" convention — and the `validate_registry.py` check that enforces it — is documented in [extending-pulp.md](extending-pulp.md#every-added-tool-must-be-user-updatable-and-overridable). The aggregate form reports installable-but-missing tools without failing; the targeted form returns non-zero when the named tool is unknown, unavailable, or not installed. With `--run`, the targeted form executes the resolved tool path with no arguments and returns its exit code; `npm_package` entries use that path as their wrapper smoke check.

**Framework importers.** An importer is a vendor-specific add-on (described in the tool-registry with `category: "importer"`) that drives Pulp's JSON-over-stdio import SPI. Installing one is gated three ways: the importer's `[sdk_min, sdk_max]` must include the running SDK and its `[spi_min, spi_max]` window must overlap the SDK's supported import-SPI window (a mismatch fails loudly with an "upgrade Pulp" / "upgrade the importer" message); the fetched or local package's `sha256` must match the digest pinned in the registry (a mismatch refuses to install); and the importer's bundled `SKILL.md` is installed into `~/.agents/skills/<importer>/` on install and removed on uninstall. Each install is recorded under `~/.pulp/importers/<id>.json` (id, version, sha256, SDK version, SPI window, paths, terms metadata) so uninstall and version checks work, and so the importer-terms accept-gate composes with the same record. `pulp add <importer>` routes to the same install path. Use `--from <path|file://...>` to install from a local package rather than the registry URL (offline installs, pinned artifacts, CI). The producer side — how prebuilt per-platform artifacts are built, hosted, pinned per SDK release, and signed/notarized, and the bundled-libclang choice — is documented in [framework-importer-packaging.md](framework-importer-packaging.md); this CLI consumes that contract, it does not decide it.

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

Downloads the release from GitHub, installs the archive's top-level companion payloads next to the current binary, replaces the current binary, and verifies. Current archives install Rust `pulp` plus the `pulp-cpp` fallthrough delegate together. Requires `curl`.

`--check-only` reads the on-disk cache written by the on-every-invocation background refresh and prints installed/latest/notes. If the cache is empty (first run), it falls through to a single live GitHub query.

`--notes` filters the embedded migration index (built from `docs/migrations/*.md` at compile time) through each entry's `applies_if` expression and prints only the notes relevant to the upgrade hop. No network, no binary swap. The JSON variant emits a stable-shape document (`from`, `to`, `entries[].{version, breaking, summary, applies_if, body}`) for the `/upgrade` Claude Code skill.

### config

**Status**: usable

Read or write `~/.pulp/config.toml` settings.

```bash
pulp config get pr.workflow
pulp config set pr.workflow github
pulp config get update.mode
pulp config set update.mode manual
pulp config set update.check_interval_hours 12
pulp config set import_design.default_mode baked
pulp config set import_design.default_emit ir-json
pulp config set claude.send_user_file off
pulp config list
```

Supported PR workflow key:

- `pr.workflow` — one of `shipyard | github | manual` (default `shipyard`).
  `shipyard` delegates to the pinned Shipyard contributor tool, `github` uses
  the GitHub CLI (`gh`) directly, and `manual` prints instructions without
  mutating PR state. `PULP_PR_WORKFLOW` overrides this value for one command.

Supported update keys:

- `update.mode` — one of `auto | prompt | manual | off` (default `prompt`). All four modes are wired into the invocation path:
    - `auto` — silently stages the new release via `~/.pulp/pending-upgrade`; the swap completes on the next invocation.
    - `prompt` — prints a one-line banner per new version; 24h snooze via `~/.pulp/update-snooze` respected if present.
    - `manual` — prints a one-line "Run `pulp upgrade` when you're ready" notice per new version; never prompts.
    - `off` — zero network calls, zero notices. Suitable for CI and air-gapped environments.

  Changing `update.mode` clears `~/.pulp/update-snooze` so the new mode takes effect on the next invocation.
- `update.check_interval_hours` — integer hours between background checks (default `24`). The 24h default stays under the 60/hour anonymous GitHub API rate limit by a wide margin.
- `update.channel` — `stable | beta` (default `stable`). Reserved for future release-channel support; ignored today.
- `update.bump_projects` — `prompt | auto | off` (default `prompt`). Controls whether a successful `pulp upgrade` nudges the user toward `pulp project pin --all`.

Supported import-design keys:

- `import_design.default_mode` — `live | baked` (default `live`). Controls the
  default import runtime model when `pulp import-design` is run without
  `--mode`. `PULP_IMPORT_DESIGN_DEFAULT_MODE` overrides this value for one
  environment/session.
- `import_design.default_emit` — `js | ir-json | cpp` (default `js`). Controls
  the default primary artifact when `pulp import-design` is run without
  `--emit`. `PULP_IMPORT_DESIGN_DEFAULT_EMIT` overrides this value for one
  environment/session. If the default mode is `baked` and this key is unset,
  Pulp implies `ir-json`.

Supported Claude Code plugin keys:

- `claude.send_user_file` — `on | off` (default `on`). When `on`, the Pulp
  Claude Code plugin's `SessionStart` hook injects a preference telling the
  agent to surface generated image/file artifacts (screenshots, rendered
  designs, diagrams, build outputs) with the `SendUserFile` tool so they embed
  in the Claude app, instead of only printing a path. Set `off` to suppress the
  injection. Read at session start by
  `hooks/scripts/inject-claude-prefs.sh`.

### coverage

**Status**: experimental

Run local coverage tooling that mirrors CI's `Diff coverage required` gate.

```bash
pulp coverage                  # show coverage tooling help
pulp coverage diff             # run the full local diff-coverage check
pulp coverage diff TARGET ...  # build specific test targets before checking
```

`pulp coverage diff` shells out to `tools/scripts/local_diff_cover.sh`.
Thresholds and file filters live in `tools/scripts/coverage_config.json`, the
same source consumed by the GitHub Actions coverage workflow.

Set `PULP_SKIP_DIFF_COVER=1` for docs-only or workflow-only changes where the
diff-coverage build is intentionally out of scope.

### minos

**Status**: experimental

Minimum-OS tooling. Reports the lowest OS a build needs, read straight from the
compiled artifact — the floor is the highest minimum among everything linked in
(Pulp's libraries, the C++ runtime, your own code). See the
[Minimum OS Support](../guides/minimum-os-support.md) guide for the full
explanation.

```bash
pulp minos                              # show help
pulp minos measure <binary>             # one binary's OS floor
pulp minos sweep --sdk-prefix <path>            # rebuild + measure every consumer
pulp minos sweep --sdk-prefix <path> --dry-run  # print the sweep plan only
pulp minos update --to 0.640.0          # dry-run: bump every consumer's SDK pin
pulp minos update --to 0.640.0 --open-prs       # actually open the update PRs
pulp minos publish-runbook --to 0.640.0         # print the republish steps
```

`pulp minos measure` reads a single binary and prints `<kind> <floor>`, e.g.
`macho 13.3` (macOS 13.3), `elf 2.34` (glibc 2.34), `pe 10.0` (Windows 10). It
accepts a `.dylib`/`.so`/`.dll`, a `.a` static archive, an executable, or a
plugin bundle's inner binary. It shells out to
`tools/scripts/measure_min_os.py`.

`pulp minos sweep` rebuilds every downstream consumer against one installed SDK
(the directory containing `lib/cmake/Pulp`) and reports each project's floor next
to the SDK floor, flagging `DRIFT` when a project needs a higher OS than the SDK
declares. It exits non-zero on any build failure or drift, so it can gate a
release. Options pass through to `tools/scripts/sdk_consumer_sweep.py` (`--only`,
`--json`, `--dry-run`). The consumer list is
`planning/sdk-consumers/consumers.yaml`; per-repo build knobs are in
`tools/scripts/sdk_consumer_sweep_recipes.yaml`. The sweep needs PyYAML
(`python3 -m pip install pyyaml`).

`pulp minos update` bumps every buildable consumer's pinned SDK version to
`--to <version>`. It is **dry-run by default** — it prints the per-repo pin
changes and writes nothing. Pass `--open-prs` to actually clone each repo, apply
the edit on a branch, commit, push, and open a PR. It rewrites the common pin
forms (`pulp.toml` `sdk_version`, `find_package(Pulp X.Y.Z)`, FetchContent
`GIT_TAG`) and leaves a floating `sdk_version = "latest"` untouched. Shells out to
`tools/scripts/sdk_consumer_update.py`.

`pulp minos publish-runbook` prints the rebuild + package + publish steps for
every consumer that ships a package. It **prints only** — it never runs a build,
signs anything, or touches a release. Full auto-publish is deliberately left to
the per-repo step: each packaged demo has its own signing identity and release
process, and publishing mutates public releases.

`pulp minos measure` is also exposed to agents over MCP as the `pulp_minos`
tool; `sweep`, `update`, and `publish-runbook` are CLI-only because they clone,
build, and (optionally) open PRs across many repositories.

### clean

**Status**: usable

Remove the build directory.

```bash
pulp clean
```

### fmt

**Status**: usable

Run `clang-format` against Pulp source files using the project root's
`.clang-format`. Walks `core/`, `examples/`, `inspect/`, `test/`,
`tools/`, and `ship/` by default, or the path arguments you pass.

```bash
pulp fmt                        # rewrite all .cpp/.hpp/.h/.mm in place
pulp fmt path/to/file.cpp ...   # restrict to specific paths
pulp fmt --dry-run              # report diffs without rewriting
pulp fmt --check                # CI-friendly alias for --dry-run
```

`pulp fmt` requires `clang-format` on `PATH`. Skips `build/`, `_deps/`,
`generated/`, and `external/` so vendored / build-output code is
untouched.

### help

Print usage information.

```bash
pulp help
```

### version

**Status**: usable

Show, bump, or check version consistency across the framework, plugin, changelog, and generated metadata surfaces.

```bash
pulp version                  # Show current SDK and project versions
pulp version --json           # Emit a machine-readable version snapshot
pulp version bump patch       # Increment patch version
pulp version bump minor       # Increment minor version
pulp version bump major --plugin  # Bump plugin version (pulp_add_plugin VERSION)
pulp version check            # Verify version consistency
pulp version check --with-bump-check  # Also run the PR version-bump gate report
```

The Rust user-facing CLI also supports `--json` on the default `version` view. The JSON object reports the running CLI version, Claude plugin version, plugin minimum CLI version, and discovered plugin manifest path using the same semver-compatible field shape as `pulp doctor --versions --json`.

The `bump` subcommand updates `CMakeLists.txt project(VERSION)` and adds a CHANGELOG.md entry. The SDK version constant is derived from CMake via `configure_file`, so a rebuild picks up the change automatically. Use `--plugin` to bump the `pulp_add_plugin(... VERSION ...)` line instead.

The `check` subcommand verifies:
- SDK version constant matches CMakeLists.txt
- AU Info.plist template uses a computed version integer (not hardcoded)
- CHANGELOG latest heading matches CMakeLists.txt
- Claude plugin manifest version is valid semver
- Claude marketplace top-level version matches the plugin manifest
- Claude marketplace `plugins[0].version` matches the plugin manifest
- `--with-bump-check` also runs `tools/scripts/version_bump_check.py --mode=report`

### kit

**Status**: experimental

Share reusable Pulp code, UI, and templates with a review step before they touch a project.

Use `pulp kit` for Pulp-native building blocks: DSP source, UI widgets, design tokens, templates, validation fixtures, and graph/native components. The value is practical:

- developers package real Pulp pieces once instead of copying example folders between projects;
- users and reviewers see the files, licenses, capabilities, and project changes before approval;
- agents can inspect structured metadata without running untrusted package code.

This is intentionally separate from `pulp add`. `pulp add rubberband` means "add a curated dependency from Pulp registry metadata." `pulp kit validate ./thing` or `pulp kit validate ./thing.pulpkit` means "inspect this local artifact before trusting or applying it."

The workflow is inspect, plan, verify, approve, apply:

1. `validate` / `inspect` read `pulp.package.json` and declared files only.
2. `plan` previews the project changes without writing files.
3. `verify` runs declared validation-profile checks after the plan has been reviewed; optional screenshot execution requires an explicit flag.
4. `apply --yes` writes only reviewed, owned project files and records the reviewed manifest digest.
5. `remove --yes` deletes only constrained lock-recorded kit paths under `pulp-kits/<kit-id>/...` plus the known generated lock/CMake files.

Trust rules:

- metadata commands never run package CMake, JavaScript, shell scripts, dynamic libraries, remote search, or content installers;
- `.pulpkit` and `.pulpcontent` archives must include `files.sha256.json`, and every payload file must be listed and hash-matched before the manifest is trusted;
- validation checks manifest shape, licenses, `requires.pulp`, `requires.cpp`, known Pulp module dependencies, and declared evidence hashes before plan/apply;
- `content-pack` manifests can be searched, validated, and inspected, but `pulp kit plan/apply/publish` rejects them; use `pulp content ...` for data-only packs;
- dependency packages declared by a kit resolve only through the existing curated `pulp add <id>` machinery.

```bash
pulp kit search basic --root ./fixtures/packages --lane kit --json
pulp kit search content --root ./fixtures/packages --lane content --json
pulp kit validate ./fixtures/packages/gain-dsp-kit
pulp kit validate ./fixtures/packages/basic-ui-kit --json
pulp kit inspect ./fixtures/packages/simple-plugin-template --json
pulp kit plan ./fixtures/packages/gain-dsp-kit --project .
pulp kit verify ./fixtures/packages/basic-ui-kit --project . --json
pulp kit verify ./fixtures/packages/basic-ui-kit --project . --execute-screenshots --json
pulp kit apply ./fixtures/packages/basic-ui-kit --project . --yes
pulp kit remove dev.pulp.fixtures.basic-ui-kit --project . --yes
pulp kit pack ./fixtures/packages/basic-ui-kit --output ./dist/basic-ui-kit.pulpkit
pulp kit publish ./fixtures/packages/basic-ui-kit --dry-run --json
pulp kit publish ./fixtures/packages/basic-ui-kit --dry-run --registry-manifest ./registry/pulp-registry-manifest.json --json
pulp kit init --kind source --id com.example.my-kit --dir ./my-kit
pulp create "Kit Gain" --template ./fixtures/packages/simple-plugin-template --no-build --ci
pulp create "Kit Gain" --template ./fixtures/packages/simple-plugin-template --ci
```

Package-backed templates can include generated-project tests, but those tests
are optional in standalone SDK builds unless the generated project can find
Catch2. The required review artifact is the declared generated-project diff;
the required build proof is the exported plugin/app format targets.

Subcommands:

| Subcommand | What it does |
|------------|--------------|
| `search [query]` | Search local `pulp.package.json`, `.pulpkit`, and `.pulpcontent` artifacts without executing package code; archives must pass `files.sha256.json` checks before their manifest is indexed, and results are classified as `kit` or `content` lanes |
| `validate <path>` | Validate a kit directory, content-pack directory, `pulp.package.json`, `.pulpkit`, or `.pulpcontent` archive without executing package code |
| `inspect <path>` | Print the manifest summary, capabilities, dependency package ids, and validation issues |
| `plan <path>` | Produce a reviewable project-mutation plan from a kit directory, manifest, or `.pulpkit` archive without writing files; rejects `content-pack` manifests, and dependency packages are resolved only by curated `pulp add <id>` ids |
| `verify <path>` | Run declared validation-profile checks after plan review; default mode is metadata-only, and `--execute-screenshots` explicitly renders Pulp screenshot profiles through the project screenshot tool and compares `expectedImage` baselines when declared, honoring optional `visualToleranceBytes` |
| `apply <path> --yes` | Apply a reviewed local kit plan from a kit directory, manifest, or `.pulpkit` archive: rejects `content-pack` manifests, writes `.pulp/kits.lock.json` with `manifest_sha256`, generated `cmake/pulp-kits.cmake`, declared copied files, and UI-kit interface metadata |
| `remove <kit-id> --yes` | Remove an installed kit using only `.pulp/kits.lock.json` ownership records |
| `pack <path>` | Create a `.pulpkit` or `.pulpcontent` archive with `files.sha256.json` |
| `publish <path> --dry-run` | Run the metadata-only kit publish gate: rejects `content-pack` manifests, strict manifest validation, license inventory, NOTICE-compatible license files via `exports.licenses`, human review, validation profiles, kind-specific evidence, local quality badges, compatibility summary, and optional signed canonical registry-manifest verification. Remote publishing is still disabled. |
| `init` | Scaffold a developer-oriented fixture manifest for `source`, `ui-kit`, or `template` |

Developer notes:

- `search` is local discovery only. It never fetches from a registry and never makes a result safe to apply.
- Template kits can seed a project with `pulp create "<name>" --template <kit-dir>`. The template is validated, exported files are copied, and dependency packages are never installed implicitly.
- UI kits copy scripts, tokens, and assets under `pulp-kits/<kit-id>/`. After review, attach one reviewed UI script and optional tokens/assets with `pulp_use_kit_ui(...)`; apply alone does not attach UI code.
- Graph/native kits use the same inspect/plan/apply flow. Validation requires explicit realtime claims, and signed `node-pack` kits cannot claim iOS/AUv3 support.
- Agent-authored kits need `authoring.humanReview.reviewed = true` before publish dry-run can pass.
- `pulp kit publish --dry-run` is local readiness only. It checks NOTICE-compatible license exports, validation evidence, compatibility, local quality badges, and optional signed `pulp-registry-manifest-v1`; remote publishing is disabled.

### content

**Status**: experimental

Validate and install data-only content packs for installed plugins.

Use `pulp content` for end-user data such as presets, themes, samples, and wavetables. Plugin authors get a standard expansion-pack format instead of custom installers. Users get validation, an explicit install target, and removal that leaves their own presets alone. Agents can reject mismatched packs before install because plugins declare the content kinds they actually support.

Keep the three lanes distinct:

- `pulp add <name>` adds curated developer dependencies from the Pulp registry.
- `pulp kit ...` reviews artifacts that may transform a project.
- `pulp content ...` installs read-only data into a plugin-specific content directory.

The workflow is validate, preview, approve, install/update. Install, update, and remove require `--yes`. `.pulpcontent` archives must include `files.sha256.json`; every payload file must be listed and hash-matched before preview, install, or update.

`preview` reads the trusted `pulp.plugin-runtime.json` emitted by the plugin and reports compatibility, target plugin, accepted content kinds, and hot-reload/rescan/restart policy. `update` takes an explicit local path, not a registry name or URL, and rolls back a replaced version on failure. Content commands copy data only; they never run package CMake, JavaScript, scripts, dynamic libraries, or remote fetches. Removal deletes only the installed content-pack root, not user-created presets or edits.

Plugins opt in with `ContentRegistry` or `PresetManager`. Prefer declaring content support in CMake with `pulp_add_plugin(... CONTENT_CAPABILITIES ... CONTENT_KINDS ...)`; that generates the `pulp.plugin-runtime.json` used by agents, previews, and `ValidationHarness::validate_plugin_runtime_manifest(...)`.

```cmake
pulp_add_plugin(MySynth
    ...
    CONTENT_CAPABILITIES content.presets.v1 content.samples.v1
    CONTENT_KINDS presets samples
    CONTENT_HOT_RELOAD_KINDS presets)
```

```json
{
  "schema": "pulp.plugin-runtime.v1",
  "pluginId": "dev.example.synth",
  "content": {
    "capabilities": ["content.presets.v1", "content.samples.v1"],
    "kinds": ["presets", "samples"],
    "reload": {
      "hotReloadKinds": ["presets"],
      "manualRescanKinds": []
    }
  }
}
```

```bash
pulp content validate ./fixtures/packages/basic-content-pack --json
pulp content preview ./fixtures/packages/basic-content-pack --plugin-runtime ./build/PulpSynth.pulp.plugin-runtime.json --plugin dev.example.synth --json
pulp kit pack ./fixtures/packages/basic-content-pack --output ./dist/basic-content-pack.pulpcontent
pulp content install ./dist/basic-content-pack.pulpcontent --plugin dev.example.synth --yes
pulp content update ./dist/basic-content-pack.pulpcontent --plugin dev.example.synth --yes
pulp content list --plugin dev.example.synth --json
pulp content rescan --json
pulp content reveal dev.pulp.fixtures.basic-content-pack --plugin dev.example.synth --version 0.1.0
pulp content remove dev.pulp.fixtures.basic-content-pack --plugin dev.example.synth --version 0.1.0 --yes
```

Subcommands:

| Subcommand | What it does |
|------------|--------------|
| `validate <path>` | Validate a `.pulpcontent` archive or `content-pack` directory without executing package code |
| `preview <path> --plugin-runtime <manifest>` | Preview compatibility and reload/restart policy without installing anything |
| `install <path> --plugin <id> --yes` | Copy a validated content pack into the plugin-specific user content root and update the content index with `plugin_id` and `manifest_sha256` |
| `update <path> --plugin <id> --yes` | Replace or add a validated local content pack, write the target plugin id and new manifest digest, and roll back a replaced version on failure |
| `list [--plugin <id>]` | List installed content packs |
| `rescan` | Rebuild `Content/index.json` from installed local manifests without copying, deleting, fetching, or executing package code; index entries include `plugin_id` and `manifest_sha256` |
| `reveal <package-id> --plugin <id>` | Print the installed content path |
| `remove <package-id> --plugin <id> --yes` | Remove an installed content-pack root |

### add

**Status**: usable

Add a curated third-party dependency from the Pulp package registry.

`pulp add` is intentionally narrow. Package names resolve through Pulp-controlled registry metadata, not arbitrary GitHub/GitLab URLs or manifest-bearing local paths. Use `pulp kit validate/plan/apply` for local or external Pulp-native artifacts that can transform a project.

```bash
pulp add signalsmith-stretch                       # add a package
pulp add lame --accept-license LGPL-2.0            # accept a restricted copyleft license after review
pulp add rubber-band --license-override commercial # use a separate commercial license
pulp add some-lib --platform-guard                 # add with platform guard
pulp add dr-libs --no-cmake                        # metadata only, skip CMake wiring
```

Performs license checking, platform compatibility analysis, overlap detection, CMake generation (`cmake/pulp-packages.cmake`), and updates `packages.lock.json`, `DEPENDENCIES.md`, and `NOTICE.md`. Restricted licenses require `--accept-license <SPDX>` after review. `--license-override commercial` is a project-owned assertion that separate commercial terms cover a package the registry policy would otherwise block.

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
pulp search fft --refresh
```

Use `--refresh` with a query to bypass the remote-registry cache while
searching. `--format json` emits machine-readable output; omit `--format` for
the default text output.

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
pulp suggest --description "onset detection" --include-license-gated
```

Suggestions omit packages that require license review or a commercial override
by default. Pass `--include-license-gated` when you explicitly want those
candidates included. `--format json` emits machine-readable output; omit
`--format` for the default text output.

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
- Most `ship` subcommands are platform-specific: macOS signing/notarization uses
  `codesign`, `pkgbuild`, and `notarytool`; Windows signing uses `signtool`;
  Android packaging/signing uses Gradle and Android SDK build tools.
- `pulp upgrade` requires internet access and `curl` (macOS/Linux) or PowerShell (Windows).
