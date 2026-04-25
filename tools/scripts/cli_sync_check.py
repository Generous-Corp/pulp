#!/usr/bin/env python3
"""CLI Sync Check — verify CLI source, slash commands, docs, and skills are consistent."""

import json
import os
import re
import sys
from pathlib import Path

# Commands that intentionally don't have slash commands
SKIP_SLASH_COMMANDS = {
    "audio", "cache", "clean", "upgrade", "config", "export-tokens",
    "ci-local", "design-debug", "help", "add", "audit",
    "inspect", "import-design", "install", "version",
    "sdk", "fetch", "list", "remove", "search", "suggest",
    "target", "update",
    # `pulp projects` is a registry-management plumbing command; it's
    # adequately documented via `--help` and the cli-maintenance
    # skill. Adding a slash command for pure plumbing would clutter
    # the slash-command surface for agents. #552
    "projects",
}


def find_repo_root():
    """Walk up from cwd to find repo root (has CMakeLists.txt + core/)."""
    d = Path.cwd()
    while d != d.parent:
        if (d / "CMakeLists.txt").exists() and (d / "core").exists():
            return d
        d = d.parent
    return None


def extract_command_table_names(root):
    """Parse user-visible command names from pulp_cli.cpp."""
    cli_path = root / "tools" / "cli" / "pulp_cli.cpp"
    if not cli_path.exists():
        return set()

    content = cli_path.read_text()
    names = set()

    def extract_table(table_name):
        table = re.search(
            rf'static const \w+ {table_name}\[\] = \{{(?P<body>.*?)\n\}};',
            content,
            re.DOTALL,
        )
        if not table:
            return
        for m in re.finditer(r'^\s*\{"(\w[\w-]*)"', table.group("body"), re.MULTILINE):
            names.add(m.group(1))

    # Structured command tables drive the main help output.
    for table_name in ("commands", "script_commands", "binary_commands"):
        extract_table(table_name)

    # A few legacy/package-manager commands are manually dispatched below
    # the tables. Count the real top-level commands, but skip hidden
    # compatibility aliases that are not advertised or documented.
    hidden_aliases = {"add-component", "install"}
    for m in re.finditer(r'if\s*\(\s*command\s*==\s*"(\w[\w-]*)"\s*\)', content):
        command = m.group(1)
        if command not in hidden_aliases:
            names.add(command)

    return names


def extract_yaml_commands(root):
    """Parse top-level command names from cli-commands.yaml."""
    yaml_path = root / "docs" / "status" / "cli-commands.yaml"
    if not yaml_path.exists():
        return set()

    content = yaml_path.read_text()
    names = set()
    # Match only top-level command entries (indented exactly 2 spaces under commands:)
    # Pattern: "  - name: <command>" at the start of a command block
    in_commands = False
    for line in content.splitlines():
        if line.strip() == "commands:":
            in_commands = True
            continue
        if in_commands and re.match(r'^  - name:\s+(\S+)', line):
            names.add(re.match(r'^  - name:\s+(\S+)', line).group(1))
        elif in_commands and line and not line.startswith(" "):
            in_commands = False  # left the commands block
    return names


def extract_slash_commands(root):
    """List .claude/commands/*.md files."""
    cmd_dir = root / ".claude" / "commands"
    if not cmd_dir.exists():
        return set()
    return {p.stem for p in cmd_dir.glob("*.md")}


def extract_skill_cli_refs(root):
    """Find 'pulp <word>' references in skills."""
    skills_dir = root / ".agents" / "skills"
    if not skills_dir.exists():
        return {}

    refs = {}
    for skill_file in skills_dir.rglob("SKILL.md"):
        content = skill_file.read_text()
        skill_name = skill_file.parent.name
        for m in re.finditer(r'pulp\s+(\w[\w-]*)', content):
            cmd = m.group(1)
            refs.setdefault(cmd, []).append(skill_name)
    return refs


def main():
    root = find_repo_root()
    if not root:
        print("Error: not in a Pulp project directory", file=sys.stderr)
        return 1

    strict = "--strict" in sys.argv
    json_mode = "--json" in sys.argv
    issues = []
    checks = []

    cli_commands = extract_command_table_names(root)
    yaml_commands = extract_yaml_commands(root)
    slash_commands = extract_slash_commands(root)
    skill_refs = extract_skill_cli_refs(root)

    # Check 1: CLI commands vs YAML manifest
    yaml_only = yaml_commands - cli_commands - {"help"}
    cli_only = cli_commands - yaml_commands - {"help"}

    if not cli_only and not yaml_only:
        checks.append(("pass", f"CLI commands ({len(cli_commands)}) match cli-commands.yaml"))
    else:
        if cli_only:
            msg = f"Commands in CLI but not in cli-commands.yaml: {', '.join(sorted(cli_only))}"
            checks.append(("fail", msg))
            issues.append(msg)
        if yaml_only:
            msg = f"Commands in cli-commands.yaml but not in CLI: {', '.join(sorted(yaml_only))}"
            checks.append(("fail", msg))
            issues.append(msg)

    # Check 2: Slash command coverage
    missing_slash = set()
    for cmd in cli_commands:
        if cmd in SKIP_SLASH_COMMANDS:
            continue
        if cmd not in slash_commands:
            missing_slash.add(cmd)

    if not missing_slash:
        checks.append(("pass", "All expected commands have slash commands"))
    else:
        msg = f"Missing slash commands (not in skip-list): {', '.join(sorted(missing_slash))}"
        checks.append(("warn", msg))

    # Check 3: Skill CLI references are valid
    invalid_refs = {}
    for cmd, skills in skill_refs.items():
        if cmd not in cli_commands and cmd not in {"version", "bump", "check"}:
            invalid_refs[cmd] = skills

    if not invalid_refs:
        checks.append(("pass", "All skill CLI references are valid"))
    else:
        for cmd, skills in invalid_refs.items():
            msg = f"Skill(s) {', '.join(skills)} reference 'pulp {cmd}' which is not a CLI command"
            checks.append(("warn", msg))

    # Output
    if json_mode:
        result = {"checks": [{"status": s, "message": m} for s, m in checks],
                  "issues": len(issues)}
        print(json.dumps(result, indent=2))
    else:
        print("CLI Sync Check")
        print("=" * 50)
        for status, msg in checks:
            if status == "pass":
                print(f"  \033[32m✓\033[0m {msg}")
            elif status == "fail":
                print(f"  \033[31m✗\033[0m {msg}")
            else:
                print(f"  \033[33m⚠\033[0m {msg}")
        print()
        if issues:
            print(f"{len(issues)} issue(s) found.")
        else:
            print("All checks passed.")

    if strict and issues:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
