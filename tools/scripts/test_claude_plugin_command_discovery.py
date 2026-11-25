#!/usr/bin/env python3
"""Offline discovery checks for the /design, /import-design and /inspect plug-in commands.

The Claude Code plug-in advertises its slash commands from a directory named in
`.claude-plugin/plugin.json`. This suite reads only that manifest, the command
Markdown files and `docs/status/cli-commands.yaml`. It builds nothing and links
no view sources, so it holds while the UI code moves out of the tree.

A command passes when the plug-in manifest resolves to it, its front matter
names it, and every `pulp <command> [subcommand]` it tells the agent to run is
a command the CLI manifest declares.
"""

from __future__ import annotations

import json
import re
import shutil
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
REQUIRED = ("design", "import-design", "inspect")
FRONT_MATTER = re.compile(r"\A---\n(.*?)\n---\n(.*)\Z", re.S)
PULP_INVOCATION = re.compile(r"`?\bpulp ([a-z][a-z0-9-]*)(?: ([a-z][a-z0-9-]*))?")


def discover_commands(root: Path) -> dict[str, Path]:
    """Map command name to file, the way the plug-in manifest resolves them."""
    plugin = json.loads((root / ".claude-plugin/plugin.json").read_text(encoding="utf-8"))
    command_dir = (root / plugin["commands"]).resolve()
    if not command_dir.is_dir():
        raise FileNotFoundError(f"plugin commands path is not a directory: {command_dir}")
    return {path.stem: path for path in sorted(command_dir.glob("*.md"))}


def parse_front_matter(text: str) -> tuple[dict[str, str], str]:
    """Split optional front matter from the body; a file without it is all body."""
    match = FRONT_MATTER.match(text)
    if not match:
        return {}, text
    fields: dict[str, str] = {}
    for line in match.group(1).splitlines():
        key, sep, value = line.partition(":")
        if sep:
            fields[key.strip()] = value.strip()
    return fields, match.group(2)


def cli_manifest(root: Path) -> dict[str, set[str]]:
    """Top-level CLI command names mapped to their declared subcommand names."""
    commands: dict[str, set[str]] = {}
    current: str | None = None
    in_subcommands = False
    for line in (root / "docs/status/cli-commands.yaml").read_text(encoding="utf-8").splitlines():
        top = re.match(r"^  - name: (\S+)\s*$", line)
        if top:
            current = top.group(1)
            commands[current] = set()
            in_subcommands = False
        elif re.match(r"^    subcommands:\s*$", line):
            in_subcommands = True
        elif re.match(r"^    \S", line):
            in_subcommands = False
        sub = re.match(r"^      - name: (\S+)\s*$", line)
        if sub and in_subcommands and current:
            commands[current].add(sub.group(1))
    return commands


def problems(root: Path) -> list[str]:
    found: list[str] = []
    try:
        discovered = discover_commands(root)
    except (OSError, KeyError, ValueError) as error:
        return [f"plugin manifest does not resolve a command directory: {error}"]
    manifest = cli_manifest(root)
    for name in REQUIRED:
        path = discovered.get(name)
        if path is None:
            found.append(f"/{name} is not discoverable from .claude-plugin/plugin.json")
            continue
        fields, body = parse_front_matter(path.read_text(encoding="utf-8"))
        if fields and fields.get("name") != name:
            found.append(f"/{name}: front matter name is {fields.get('name')!r}")
        if fields and len(fields.get("description", "")) < 10:
            found.append(f"/{name}: front matter needs a description")
        if not body.strip():
            found.append(f"/{name}: empty body")
        invocations = PULP_INVOCATION.findall(body)
        if not any(command == name for command, _ in invocations):
            found.append(f"/{name}: never tells the agent to run `pulp {name}`")
        for command, sub in invocations:
            if command not in manifest:
                found.append(f"/{name}: `pulp {command}` is not a CLI command")
            elif sub and manifest[command] and sub not in manifest[command]:
                # Only a name the manifest knows as a sibling subcommand is a
                # claim; prose after a command word is not.
                if any(sub in subs for subs in manifest.values()):
                    found.append(f"/{name}: `pulp {command} {sub}` is not a subcommand")
    return found


class RepoDiscovery(unittest.TestCase):
    def test_required_commands_are_discoverable_and_valid(self) -> None:
        self.assertEqual(problems(REPO), [])

    def test_every_required_command_is_listed(self) -> None:
        self.assertTrue(set(REQUIRED) <= set(discover_commands(REPO)))


class NegativeControls(unittest.TestCase):
    """Each defect, planted in a copy, must be reported."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        (self.tmp / ".claude-plugin").mkdir()
        (self.tmp / "docs/status").mkdir(parents=True)
        shutil.copy(REPO / ".claude-plugin/plugin.json", self.tmp / ".claude-plugin")
        shutil.copy(REPO / "docs/status/cli-commands.yaml", self.tmp / "docs/status")
        shutil.copytree(REPO / ".claude/commands", self.tmp / ".claude/commands")
        self.assertEqual(problems(self.tmp), [])

    def test_missing_command_file_is_reported(self) -> None:
        (self.tmp / ".claude/commands/inspect.md").unlink()
        self.assertIn("/inspect is not discoverable", " ".join(problems(self.tmp)))

    def test_renamed_front_matter_is_reported(self) -> None:
        path = self.tmp / ".claude/commands/design.md"
        path.write_text(
            path.read_text(encoding="utf-8").replace("name: design\n", "name: designer\n", 1),
            encoding="utf-8",
        )
        self.assertIn("front matter name is 'designer'", " ".join(problems(self.tmp)))

    def test_unknown_cli_command_is_reported(self) -> None:
        path = self.tmp / ".claude/commands/import-design.md"
        path.write_text(
            path.read_text(encoding="utf-8") + "\nRun `pulp no-such-command` next.\n", encoding="utf-8"
        )
        self.assertIn("`pulp no-such-command` is not a CLI command", " ".join(problems(self.tmp)))

    def test_declared_sibling_subcommand_under_wrong_command_is_reported(self) -> None:
        path = self.tmp / ".claude/commands/inspect.md"
        path.write_text(
            path.read_text(encoding="utf-8") + "\nThen `pulp inspect instances`.\n", encoding="utf-8"
        )
        self.assertIn("`pulp inspect instances` is not a subcommand", " ".join(problems(self.tmp)))

    def test_missing_plugin_commands_directory_is_reported(self) -> None:
        manifest = self.tmp / ".claude-plugin/plugin.json"
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["commands"] = "./nowhere"
        manifest.write_text(json.dumps(data), encoding="utf-8")
        self.assertIn("does not resolve a command directory", " ".join(problems(self.tmp)))


if __name__ == "__main__":
    unittest.main()
