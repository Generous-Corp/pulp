# Agent integrations

Pulp is designed to work with any AI coding agent. Nothing about the
SDK or CLI assumes a specific agent. Editor integrations are additive.

## Layers

| Layer | What it is | Who needs it |
|---|---|---|
| **Pulp CLI + SDK** | The framework itself: `pulp` binary + libraries | **Everyone.** Universal foundation. |
| **Skills** (`.agents/skills/`) | Markdown SKILL.md files that document subsystems | Auto-loaded by both Claude Code and Codex |
| **Slash commands** (`.claude/commands/`) | Claude Code shortcuts like `/build`, `/ship` | Claude Code users only |
| **MCP server** (`tools/mcp/pulp-mcp`) | Native tools (build/test/inspect) for MCP-aware agents | Currently Claude Code; extensible |

## What each agent gets out of the box

### Bare CLI (no agent, or any agent)

```bash
curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
pulp create my-plugin && cd my-plugin && pulp run
```

Everything works — `pulp build`, `pulp test`, `pulp ship`, the lot.
Type-completion and AI assistance are whatever your editor provides.

### Codex

Codex automatically reads `AGENTS.md` (which redirects to `CLAUDE.md`)
and discovers `.agents/skills/` for subsystem-specific guidance —
audio formats, view system, CI flow, hosting other plugins. Same
skills the Claude Code plugin exposes; no separate install.

```bash
# Same install as bare CLI — Codex picks up the skills automatically
curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
```

### Claude Code (with the optional plugin)

Install Pulp CLI first (above), then add the plugin for slash-command
shortcuts and the native MCP server:

```bash
claude plugin marketplace add danielraffel/pulp
claude plugin install pulp
```

The plugin extends Claude Code with:

- **Slash commands**: `/build`, `/test`, `/create`, `/design`, `/ship`,
  `/import-design`, `/version`, `/upgrade` — convenience wrappers over
  the CLI.
- **MCP server**: Claude can call build/test/inspect tools as MCP
  tool calls instead of shell-and-parse. Highest value for the
  inspector tools (`pulp_inspect_dom`, `pulp_inspect_evaluate`,
  etc.) which wrap the running-plugin inspector socket protocol.
- **Setup hook**: when a Claude Code session starts in a project that
  has the plugin installed but `pulp` is not on PATH, prints a
  one-time install banner. Informational, never blocks the session.

If `pulp` is missing when a slash command is invoked, the command
itself also prints the install command before failing. Pulp CLI is
always the dependency the plugin sits on top of.

## Why the split

The Claude plugin is a **convenience layer**, not the primary install.
Splitting it from the CLI:

- Means Codex / Cursor / bare-editor users aren't blocked on a
  Claude-specific install.
- Lets the CLI ship updates on its own cadence (`pulp upgrade`)
  independent of the plugin's release cycle (`plugin-vX.Y.Z` tags).
- Keeps the marketplace listing honest — the plugin is what it says
  it is (commands + skills + MCP), not a CLI installer in disguise.

If you have feedback on integrations for an agent we haven't covered
yet, open an issue with the `agent-integration` label.
