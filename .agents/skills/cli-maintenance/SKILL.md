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
audio, cache, clean, upgrade, export-tokens, ci-local, design-debug, help

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
```

## Modifying a CLI Command

Same as above, focus on steps 2, 4, 5, 6, 7. Key risks:
- Changed args not reflected in `cli-commands.yaml`
- Changed behavior not reflected in slash command `.md`
- Skills calling the old invocation syntax

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
