# Claude Code Plugin

Pulp includes a Claude Code plugin that provides commands, skills, and hooks for the full audio plugin development lifecycle. If you're using Claude Code, we highly recommend installing it.

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
| `/version` | Show, bump, or check version consistency |
| `/import-design` | Import from Figma, Stitch, v0, or Pencil |

### Skills

Skills activate automatically based on context. You don't need to invoke them explicitly — just describe what you want.

| Skill | Activates when you... |
|-------|-----------------------|
| **ci** | Say "ship this", "create a PR", "run CI", "merge to main" |
| **engine** | Ask about JS engines, Three.js performance, switching to V8/JSC |
| **import-design** | Want to import a design from Figma or other tools |
| **webview-ui** | Want to build a WebView-based UI panel |
| **cli-maintenance** | Add, modify, or remove CLI commands — keeps source, docs, and plugin in sync |

### Hooks

The plugin includes two hooks that run automatically:

- **docs-reminder** — When you modify files in `core/`, `examples/`, or `tools/cli/`, reminds you to update documentation manifests
- **cli-plugin-sync** — When you modify the CLI or MCP server, reminds you to check if the plugin commands and skills need matching updates

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
