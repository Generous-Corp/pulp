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

### From a local clone (current method)

Since Pulp is not yet a public repository, install the plugin from your local clone:

```bash
# Clone the repository
git clone https://github.com/danielraffel/pulp.git
cd pulp

# Install the plugin into Claude Code
claude plugin add /path/to/pulp
```

Then restart Claude Code or run `/reload-plugins` to activate.

### From the marketplace (when public)

Once Pulp is a public repository, you'll be able to install from the Claude Code plugin marketplace:

```bash
# 1. Add the Pulp marketplace (one time)
/plugin marketplace add danielraffel/pulp

# 2. Install the plugin
/plugin install pulp@danielraffel/pulp

# 3. Restart Claude Code to load the plugin
```

This is not yet available because the Pulp repository is currently private. These instructions will work once the repository is public.

## What You Get

### Slash Commands

| Command | Description |
|---------|-------------|
| `/build` | Build the project (configure + compile) |
| `/test [pattern]` | Run tests, optionally filtered by pattern |
| `/create <name>` | Scaffold a new plugin or app project |
| `/status` | Show project status, build state, configuration |
| `/validate` | Run plugin format validators (auval, clap-validator) |
| `/design [style]` | AI-driven design session with natural language |
| `/ship` | Sign, notarize, and package for distribution |
| `/import-design` | Import from Figma, Stitch, v0, or Pencil |
| `/version` | Show, bump, or check version consistency |

### Skills

Skills activate automatically based on context. You don't need to invoke them explicitly — just describe what you want.

| Skill | Activates when you... |
|-------|-----------------------|
| **ci** | Say "ship this", "create a PR", "run CI", "merge to main" — uses `shipyard pr` for PR creation, tracking, cross-platform validation, and merge-on-green |
| **engine** | Ask about JS engines, Three.js performance, switching to V8/JSC |
| **import-design** | Want to import a design from Figma or other tools |
| **webview-ui** | Want to build a WebView-based UI panel |

### Hooks

The plugin includes two hooks that run automatically:

- **docs-reminder** — When you modify files in `core/`, `examples/`, or `tools/cli/`, reminds you to update documentation manifests
- **cli-plugin-sync** — When you modify the CLI or MCP server, reminds you to check if the plugin commands and skills need matching updates

### MCP server

The plugin ships an MCP (Model Context Protocol) server — `pulp-mcp` — that
exposes 18 Pulp operations as callable tools, so Claude Code (and other MCP
clients) can drive them in one turn instead of multiple shell calls.

| Category | Tools |
|---|---|
| Build / test / status | `pulp_build`, `pulp_test`, `pulp_status`, `pulp_validate`, `pulp_create`, `pulp_docs_check`, `pulp_docs_search` |
| UI rendering + interaction | `pulp_screenshot` (render JS UI to PNG), `pulp_simulate_click`, `pulp_get_view_tree` |
| Live plugin inspection (inspector protocol) | `pulp_inspect_dom`, `pulp_inspect_params`, `pulp_inspect_screenshot`, `pulp_inspect_evaluate` (JS expr), `pulp_inspect_performance`, `pulp_inspect_audio` |
| Audio model / WAV-first excerpt-find | `pulp_audio_model_list`, `pulp_audio_model_status`, `pulp_audio_model_activate`, `pulp_audio_excerpt_find`, `pulp_audio_read_bundle` |

#### Setup

`tools/mcp/pulp-mcp-launcher` is a thin shell shim. It looks for the binary
in this order:

1. `<repo>/build/tools/mcp/pulp-mcp` (source-tree build) — typical Pulp
   contributor workflow.
2. `pulp-mcp` on `$PATH` (installed binary).

If neither exists, the launcher prints a readable diagnostic on stderr and
exits 127 so Claude Code surfaces the failure rather than silently no-oping
(`pulp #1821`).

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
│   │   └── import-design.md
│   └── settings.json         # Hook configuration
├── .agents/
│   └── skills/               # Shared skills (Claude Code + Codex)
│       ├── ci/
│       ├── engine/
│       ├── import-design/
│       └── webview-ui/
└── hooks/
    ├── hooks.json             # Hook definitions
    └── scripts/
        ├── docs-reminder.sh
        └── cli-plugin-sync.sh
```

Skills live in `.agents/skills/` so they're shared between Claude Code and Codex CLI. Both agents read from the same location.

## Staying in Sync

The `cli-plugin-sync` hook alerts you when the CLI (`tools/cli/pulp_cli.cpp`) or MCP server (`tools/mcp/pulp_mcp.cpp`) are modified. This is a reminder to check whether commands, skills, or hooks need matching updates.

When adding a new CLI command, consider whether it should also become a slash command or skill.
