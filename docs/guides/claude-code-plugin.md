# Claude Code Plugin

Pulp includes a Claude Code plugin that provides commands, skills, and hooks for the full audio plugin development lifecycle. If you're using Claude Code, we highly recommend installing it.

## CLI expectations

As of Pulp v0.78.1, the user-facing `pulp` CLI is the Rust binary. Source
builds produce `./build/pulp`, while release installers place `pulp` and the
C++ fallthrough delegate `pulp-cpp` side by side. Slash commands and skills
should use `pulp` on PATH, or `./build/pulp` inside a source build. Use
`pulp-cpp` only for rollback/debug comparisons.

For PR/shipping workflows, agents and humans should use `shipyard pr`. Direct
`gh pr create` is a manual bypass only because it can leave the PR outside
Shipyard-managed tracking state. `pulp pr` defaults to Shipyard, while
`pulp config set pr.workflow github` and `manual` are explicit local opt-outs
for humans who do not want Shipyard in their checkout.

## Installation

### From the marketplace (recommended)

Install the Pulp CLI first so `pulp` and `pulp-mcp` are on `$PATH`, then add
the Claude Code plugin from the public marketplace:

```bash
# 1. Install the Pulp CLI and MCP binary.
curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh

# 2. Add the marketplace and install the plugin.
claude plugin marketplace add Generous-Corp/pulp
claude plugin install pulp
```

Then restart Claude Code or run `/reload-plugins` to activate.

### From a local clone (source-tree development)

If you're developing Pulp from a checkout, or testing local plugin changes,
install the plugin from that clone instead:

```bash
# Clone the repository.
git clone https://github.com/Generous-Corp/pulp.git
cd pulp

# Install the checkout into Claude Code.
claude plugin add "$PWD"
```

Then restart Claude Code or run `/reload-plugins` to activate.

## What You Get

### Slash Commands

The plugin loads commands from `.claude/commands`; this table lists common
examples rather than the full source-tree inventory.

| Command | Description |
|---------|-------------|
| `/build` | Build the project (configure + compile) |
| `/test [pattern]` | Run tests, optionally filtered by pattern |
| `/create <name>` | Scaffold a new plugin or app project |
| `/status` | Show project status, build state, configuration |
| `/validate` | Run plugin format validators and validation reports |
| `/design [style]` | AI-driven design session with natural language |
| `/ship` | Sign, notarize, and package for distribution |
| `/import-design` | Import from Figma, Stitch, v0, Pencil, Claude Design, DESIGN.md, or React JSX |
| `/kit` | Search, inspect, plan, apply, remove, pack, or scaffold Pulp kits |
| `/content` | Validate, install, update, list, rescan, remove, or reveal data-only content packs |
| `/version` | Show, bump, or check version consistency |

### Skills

The plugin loads skills from `.agents/skills`. Skills activate automatically
based on context, and the table below lists common examples rather than the full
source-tree inventory.

| Skill | Activates when you... |
|-------|-----------------------|
| **ci** | Say "ship this", "create a PR", "run CI", "merge to main" — uses `shipyard pr` for PR creation, tracking, cross-platform validation, and merge-on-green |
| **engine** | Ask about JS engines, Three.js performance, switching to V8/JSC |
| **import-design** | Want to import a design from Figma or other tools |
| **content** | Want to validate or install preset/theme/sample/wavetable content packs |
| **webview-ui** | Want to build a WebView-based UI panel |

### Hooks

The plugin includes hooks that run automatically:

- **docs-reminder** — When you modify files in `core/`, `examples/`, or `tools/cli/`, reminds you to update documentation manifests
- **cli-plugin-sync** — When you modify the CLI or MCP server, reminds you to check if the plugin commands and skills need matching updates
- **inject-claude-prefs** (`SessionStart`) — Reads `claude.send_user_file` from `~/.pulp/config.toml` (default **on**) and, when enabled, tells the agent to surface generated image/file artifacts with the `SendUserFile` tool so they embed in the Claude app instead of being printed as a bare path. Toggle with `pulp config set claude.send_user_file off` (and `on` to re-enable).

### MCP server

The plugin ships an MCP (Model Context Protocol) server — `pulp-mcp` — that
exposes Pulp operations as callable tools, so Claude Code (and other MCP
clients) can drive them in one turn instead of multiple shell calls.

| Category | Tools |
|---|---|
| Build / test / status | `pulp_build`, `pulp_test`, `pulp_status`, `pulp_validate` (`screenshot=true` for validation editor PNGs), `pulp_create`, `pulp_docs_check`, `pulp_docs_search` |
| UI rendering + interaction | `pulp_screenshot` (render demo/script UI fixtures to PNG), `pulp_simulate_click`, `pulp_get_view_tree` |
| Live plugin inspection (inspector protocol) | `pulp_inspect_dom`, `pulp_inspect_params`, `pulp_inspect_set_param` (gesture-wrapped numeric param write), `pulp_inspect_screenshot` (currently returns the inspector unavailable error until host-capture wiring lands), `pulp_inspect_evaluate` (currently returns the inspector unavailable error until ScriptEngine wiring lands), `pulp_inspect_performance`, `pulp_inspect_audio` |
| Motion tracing + fixture replay | `pulp_motion_start_trace`, `pulp_motion_stop_trace`, `pulp_motion_snapshot`, `pulp_motion_list_traces`, `pulp_motion_load_fixture`, `pulp_motion_scrub_to`, `pulp_motion_play`, `pulp_motion_pause`, `pulp_motion_enable_cost`, `pulp_motion_disable_cost` |
| Perfetto tracing (live-session inspector RPCs) | `pulp_trace_start`, `pulp_trace_stop`, `pulp_trace_snapshot`, `pulp_trace_query`, `pulp_trace_explain` (client-side `pulp trace doctor` / `open` / `fetch` and offline `query --trace` have no inspector RPC, so no MCP tool) |
| Audio model / WAV-first excerpt-find / live probe/scope JSON / third-party plugin inspection + offline render / advisory before-after compare | `pulp_audio_model_list`, `pulp_audio_model_status`, `pulp_audio_model_activate`, `pulp_audio_excerpt_find`, `pulp_audio_read_bundle`, `pulp_audio_probe_json`, `pulp_audio_scope`, `pulp_audio_plugin_inspect`, `pulp_audio_render`, `pulp_audio_compare` |
| Timeline project editing + rendering | `pulp_timeline_project_open`, `pulp_timeline_command_apply`, `pulp_timeline_validate`, `pulp_timeline_explain`, `pulp_timeline_render` |
| Kit manifests | `pulp_kit`, `pulp_kit_search`, `pulp_kit_validate`, `pulp_kit_inspect`, `pulp_kit_plan`, `pulp_kit_verify`, `pulp_kit_apply`, `pulp_kit_remove`, `pulp_kit_pack`, `pulp_kit_publish_check`, `pulp_kit_init` |
| Content packs | `pulp_content`, `pulp_content_validate`, `pulp_content_preview`, `pulp_content_install`, `pulp_content_update`, `pulp_content_list`, `pulp_content_rescan`, `pulp_content_remove`, `pulp_content_reveal` |

Use `pulp_audio_probe_json` as the quick live-health check for a standalone
target. It runs the existing `pulp run --audio-probe-json` path through
`pulp-mcp` and returns structured peak/RMS, callback, clip, NaN/Inf, and device
stress counters. It is not a new MCP server and it is not an offline signal
quality analyzer; switch to Audio Doctor or a scenario render when the live
snapshot is healthy but the audio still sounds wrong.

Use `pulp_audio_scope` when an agent needs real sample-window acquisition and
measurements instead of scalar probe counters. Live target mode may open the
standalone audio device; `input_wav` mode is speakerless/offline and can also
write a PNG trace artifact for review. Both modes return `pulp.audio.scope.v1`
structured JSON.

Use `pulp_audio_plugin_inspect` first when an agent does not know a third-party
plugin's host API. It loads the bundle in a disposable worker and returns every
parameter ID, plain range, default/current value and flag, plus buses, latency,
and tail. Feed those IDs to `pulp_audio_render`, which performs a second isolated
offline run (no DAW or audio device) and returns audio-analysis metrics. It can
warm up asynchronous plugins, apply one initial parameter and settle before
capture, drive one sample-timed `param`/`midi` event, append a tail, and retain a
high-resolution WAV. Use the CLI when a scenario needs repeatable parameters or
MIDI events.

Use `pulp_audio_compare` to get an advisory before/after **judgment** between two
WAVs (measure → compare → judge) through `pulp audio compare`. It delegates to the
opt-in Audio Quality Lab tool, level-matches, runs one axis (`profile`), and
returns the typed `quality_lab.compare.v1` report (evidence envelope + a verdict
like `regression_suspected` / `material_change_detected`). It is advisory, never a
gate; set `reference_role: golden` when the reference is the known-good baseline.
Requires `pulp tool install audio-quality-lab`. For an exact pass/fail diff use
the deterministic `pulp audio validate compare` surface instead.

The inspection/render/validate workflow is built into Pulp. Audio Quality Lab is
only the richer advisory comparison step; its absence never blocks inspection or
rendering and the compare tool returns an explicit install command.

The five timeline tools are generated from the committed timeline schema
manifest. They accept a project path or inline canonical project JSON; command
application accepts only the generated typed command envelopes. Use
`pulp_timeline_render` for deterministic Float32 WAV output, then optionally pass
before/after renders to `pulp_audio_compare` for an advisory judgment.

The kit and content MCP tools mirror the CLI trust model. `pulp_kit_*` tools inspect, plan, verify, and apply local project-transforming artifacts only after review; `pulp_content_*` tools validate, preview, and install data-only packs for an explicit plugin. Curated dependency packages stay on `pulp add <name>`.

#### Setup

`tools/mcp/pulp-mcp-launcher` is a thin shell shim. It looks for the binary
in this order:

1. `<repo>/build/tools/mcp/pulp-mcp` (source-tree build) — typical Pulp
   contributor workflow.
2. `pulp-mcp` on `$PATH` (installed binary).

If neither exists, the launcher prints a readable diagnostic on stderr and
exits 127 so Claude Code surfaces the failure rather than silently no-oping
(`pulp #1821`).

The `/status` command and `pulp_status` MCP tool include the effective
`pulp import-design` defaults, so Claude Code can see whether a checkout is
using the shipped `live/js` default or a local baked IR/C++ preference.

To wire it up from a Pulp checkout:

```bash
cmake --build build --target pulp-mcp
# Then in Claude Code: "Reconnect" the pulp MCP server.
```

#### `.mcp.json` and `${CLAUDE_PLUGIN_ROOT}` (gotcha)

The Pulp repo's project-local `.mcp.json` uses a path **relative to the
project root** — `./tools/mcp/pulp-mcp-launcher` — not
`${CLAUDE_PLUGIN_ROOT}/...`. Reason: `${CLAUDE_PLUGIN_ROOT}` is set by
Claude Code only when launching a marketplace-installed plugin's MCP
server. Project-local `.mcp.json` files (loaded by Claude Code from
the working-directory project, not from a plugin install) get an empty
`${CLAUDE_PLUGIN_ROOT}`, which makes the command resolve to the literal
`/tools/mcp/pulp-mcp-launcher` — fails with `No such file or directory`,
exit 127, and Claude Code shows the server as `✘ failed`.

Until the Pulp plugin ships a separate marketplace-bundled `.mcp.json`
under `.claude-plugin/`, only project-local source-tree usage is
supported. That's intentional — `pulp-mcp` requires the binary to be
built (or installed on `$PATH`) regardless of how it's wired in, so
non-checkout users don't gain anything from the marketplace path.

## Plugin Structure

```
pulp/
├── .claude-plugin/
│   └── plugin.json          # Plugin manifest
├── .claude/
│   ├── commands/             # Slash commands
│   │   ├── build.md
│   │   ├── test.md
│   │   ├── create.md
│   │   ├── status.md
│   │   ├── validate.md
│   │   ├── design.md
│   │   ├── ship.md
│   │   ├── import-design.md
│   │   ├── kit.md
│   │   └── content.md
│   └── settings.json         # Hook configuration
├── .agents/
│   └── skills/               # Shared skills (Claude Code + Codex)
│       ├── ci/
│       ├── content/
│       ├── engine/
│       ├── import-design/
│       ├── kits/
│       └── webview-ui/
└── hooks/
    ├── hooks.json             # Hook definitions
    └── scripts/
        ├── docs-reminder.sh
        └── cli-plugin-sync.sh
```

Skills live in `.agents/skills/` so they're shared between Claude Code and Codex CLI. Both agents read from the same location, and the `/kit` and `/content` slash commands point agents at the same inspect/preview/approve/apply workflows documented here.

### Kit Manifests

The plugin includes a `kits` skill for reusable Pulp-native source, UI, template, validation, graph, and native-component artifacts.

Why it matters:

- developers can share real Pulp building blocks instead of copy-pasted examples;
- users see capabilities, licenses, files, and project changes before approval;
- agents can inspect and plan without running untrusted package code.

```bash
pulp kit validate <path>
pulp kit search <query> --root <dir> --lane kit --json
pulp kit search <query> --root <dir> --lane content --json
pulp kit inspect <path> --json
pulp kit plan <path> --json
pulp kit verify <path> --json
pulp kit verify <path> --execute-screenshots --json
pulp kit apply <path> --yes
pulp kit remove <kit-id> --yes
pulp kit pack <path> --output <file>
pulp kit publish <path> --dry-run --json
pulp kit publish <path> --dry-run --registry-manifest <file> --json
pulp kit init --kind source --id com.example.my-kit
pulp create "Kit Gain" --template <template-kit-dir> --no-build --ci
```

Keep the trust boundary simple:

- `pulp add <name>` is only for curated dependency packages from the Pulp registry.
- `pulp kit ...` is for local or external artifacts that may transform a project.
- `pulp kit search` is local discovery only. It does not fetch and does not make a result trusted.
- `content-pack` manifests can be searched, validated, and inspected for classification, but `pulp kit plan/apply/publish` rejects them. Use `pulp content ...` for data-only packs.

The agent workflow is inspect, plan, verify, approve, apply. Validate/inspect/plan/pack/publish dry-run are metadata-only: no package CMake, JavaScript, scripts, dynamic libraries, remote search, or content installers. `.pulpkit` and `.pulpcontent` archives must include `files.sha256.json`; every payload file must be listed and hash-matched before the manifest is trusted.

Validation is valuable because it catches license, SDK, C++ standard, internal-module, tampered-evidence, and missing-review problems before any project CMake changes. `pulp kit apply --yes` writes only reviewed owned files (`.pulp/kits.lock.json`, generated CMake scaffolding, and declared files under `pulp-kits/<kit-id>/`). Dependency packages declared by a kit resolve only through the existing curated `pulp add <id>` path. `pulp create --template <template-kit-dir>` validates an explicit local template kit and never installs dependencies implicitly.

Evidence rules to surface before apply:

- Agent-authored kits need `authoring.humanReview.reviewed = true` before publish dry-run can pass.
- Template kits need `validation.generatedProjectDiffs`.
- UI kits need screenshot profiles/reports; run `pulp kit verify` after plan review, and use `--execute-screenshots` only when rendered artifacts are explicitly needed.
- Graph/native kits need exported fixtures/files plus explicit realtime claims (`processSafe`, `allocatesInProcess`, `locksInProcess`); verification must not load dynamic libraries.

### Content Packs

The plugin includes a `content` skill for data-only packs installed into an existing plugin's user content directory.

Why it matters:

- plugin authors ship presets, themes, samples, and wavetables without custom installers;
- users can validate, see reload/restart expectations, and remove packs without losing their own presets;
- agents match packs to plugins through runtime capabilities before install.

```bash
pulp content validate <path> --json
pulp content preview <path> --plugin-runtime <manifest> --json
pulp content install <path> --plugin <plugin-id> --yes
pulp content update <path> --plugin <plugin-id> --yes
pulp content list --plugin <plugin-id> --json
pulp content rescan --json
pulp content reveal <package-id> --plugin <plugin-id> --version <version>
pulp content remove <package-id> --plugin <plugin-id> --version <version> --yes
```

Content packs are not curated dependency packages and do not transform projects. The workflow is validate, preview compatibility and reload policy, approve, install or update. Install/update/remove require explicit approval and never execute package CMake, JavaScript, scripts, or dynamic libraries. `.pulpcontent` archives must include `files.sha256.json`; every payload file must be listed and hash-matched before preview, install, or update.

The value for plugin authors is a standard expansion-pack path. Users see the target plugin, install location, and reload/rescan/restart expectation before approval, and removal deletes only the installed pack root. User-created presets remain in the plugin's normal user preset path.

Runtime plugins opt in through `ContentRegistry` or `PresetManager`. Prefer `pulp_add_plugin(... CONTENT_CAPABILITIES ... CONTENT_KINDS ... CONTENT_HOT_RELOAD_KINDS ... CONTENT_MANUAL_RESCAN_KINDS ...)`, which generates `pulp.plugin-runtime.json` for agents, previews, and `ValidationHarness::validate_plugin_runtime_manifest(...)`.

```cmake
pulp_add_plugin(MySynth
    ...
    CONTENT_CAPABILITIES content.presets.v1
    CONTENT_KINDS presets
    CONTENT_HOT_RELOAD_KINDS presets)
```

## Staying in Sync

The `cli-plugin-sync` hook alerts you when the CLI C++ tables, Rust front-end
command enum, or MCP server (`tools/mcp/pulp_mcp.cpp`) are modified. This is a
reminder to check whether commands, skills, or hooks need matching updates.

When adding a new CLI command, consider whether it should also become a slash command or skill.
