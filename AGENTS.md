# AGENTS.md

Shared instructions for all AI coding agents working on Pulp (Codex, Claude Code, Cursor, etc.).

Detailed guidance lives in `CLAUDE.md` — treat it as the single source of truth for build commands, architecture, clean-room rules, commit standards, and testing requirements.

## Key Rules

- **Never push directly to main. No exceptions.** Every change — no matter how small, even docs-only — must go through: branch → PR → CI green → merge. Use the `ci` skill or create a PR manually. If cloud CI minutes are exhausted, run local CI (`python3 tools/local-ci/local_ci.py run`) and wait for green before merging.
- **Clean-room discipline.** Never reference JUCE source code. See CLAUDE.md.
- **License policy.** Only MIT, BSD, Apache 2.0, ISC, zlib, BSL-1.0, public domain. No copyleft.
- **Tests required.** If it's not tested, it doesn't work.

## Claude Code Plugin

Pulp ships with a Claude Code plugin for the full development lifecycle. See [docs/guides/claude-code-plugin.md](docs/guides/claude-code-plugin.md) for installation and usage.

### Commands

| Command | Purpose |
|---------|---------|
| `/build` | Build the project |
| `/test [pattern]` | Run tests, optionally filtered |
| `/create <name>` | Scaffold a new plugin project |
| `/status` | Show project status and configuration |
| `/validate` | Run plugin format validators |
| `/design [style]` | AI-driven design session |
| `/ship` | Sign, package, and distribute |
| `/import-design` | Import from Figma, Stitch, v0, Pencil |

### Skills

| Skill | Purpose |
|-------|---------|
| `ci` | Create PRs, run local/cloud CI, merge on green |
| `engine` | Query, recommend, and switch JS engine backend |
| `import-design` | Import designs with automated visual validation |
| `webview-ui` | Build WebView UIs with native bridge |

### Hooks

| Hook | Trigger | Purpose |
|------|---------|---------|
| `docs-reminder` | Edit/Write to `core/`, `examples/`, `tools/cli/` | Remind to update docs |
| `cli-plugin-sync` | Edit/Write to CLI or MCP server | Remind to update plugin |

## CI Workflow

```bash
# Validate current branch locally
python3 tools/local-ci/local_ci.py run

# Ship: PR + CI + merge on green
# Use the ci skill — say "ship this" or "create a PR and push to main"
```

Setup: `docs/guides/local-ci.md`

## Quick Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(sysctl -n hw.ncpu)
ctest --test-dir build --output-on-failure
```
