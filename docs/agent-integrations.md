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

**macOS / Linux**

```bash
curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
pulp create my-plugin && cd my-plugin && pulp run
```

**Windows (PowerShell)**

```powershell
irm https://www.generouscorp.com/pulp/install.ps1 | iex
pulp create my-plugin
cd my-plugin
pulp run
```

Everything works — `pulp build`, `pulp test`, `pulp ship`, the lot.
Type-completion and AI assistance are whatever your editor provides.

### Codex

Codex automatically reads `AGENTS.md` (which redirects to `CLAUDE.md`)
and discovers `.agents/skills/` for subsystem-specific guidance —
audio formats, view system, CI flow, hosting other plugins. Same
skills the Claude Code plugin exposes; no separate install.

```bash
# Same CLI install as above — Codex picks up the skills automatically
curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
```

On Windows, use the PowerShell installer from the bare CLI section.

### Claude Code (with the optional plugin)

Install Pulp CLI first (above), then add the plugin for slash-command
shortcuts and the native MCP server:

```bash
claude plugin marketplace add Generous-Corp/pulp
claude plugin install pulp
```

The plugin extends Claude Code with:

- **Slash commands**: `/build`, `/test`, `/create`, `/design`, `/ship`,
  `/import-design`, `/version`, `/upgrade` — convenience wrappers over
  the CLI.
- **MCP server**: Claude can call build/test/inspect tools as MCP
  tool calls instead of shell-and-parse. Highest value for the
  inspector tools (`pulp_inspect_dom`, `pulp_inspect_params`,
  etc.) which wrap the running-plugin inspector socket protocol.
  For project screenshot artifacts, call `pulp_validate` with
  `screenshot=true` or run `pulp run --headless --screenshot <png>`;
  both use the same capture contract documented for the CLI. The
  `pulp_screenshot` MCP tool is a demo/script fixture renderer, not a
  live plugin capture API.
  `pulp_inspect_evaluate` and `pulp_inspect_screenshot` currently
  return explicit unavailable errors until script-engine and
  host-capture wiring lands.
  Timeline projects have five native tools:
  `pulp_timeline_project_open`, `pulp_timeline_command_apply`,
  `pulp_timeline_validate`, `pulp_timeline_explain`, and
  `pulp_timeline_render`. They share the implementation used by `pulp seq` and
  `pulp render`; the [Timeline SDK guide](guides/timeline-sdk.md) shows the
  end-to-end typed-command workflow and which realtime features remain C++ SDK
  responsibilities.
- **Timeline skill**: requests about arrangements, clips, tempo/meter,
  automation, takes and comps, freeze, capture, journals, playback, or
  DAWproject import load the subsystem playbook. The skill selects CLI, MCP, or
  C++ rather than implying that realtime capture or device I/O is an MCP
  operation.
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
