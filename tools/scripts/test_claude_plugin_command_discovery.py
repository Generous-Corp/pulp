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
FRONT_MATTER = re.compile(r"\A---\n(.*?)\n---[ \t]*(?:\n(.*))?\Z", re.S)
PULP_INVOCATION = re.compile(r"`?\bpulp ([a-z][a-z0-9-]*)(?: ([a-z][a-z0-9-]*))?")


def discover_commands(root: Path) -> dict[str, Path]:
    """Map command name to file, the way the plug-in manifest resolves them."""
    plugin = json.loads((root / ".claude-plugin/plugin.json").read_text(encoding="utf-8"))
    repo_root = root.resolve()
    command_dir = (root / plugin["commands"]).resolve()
    if not command_dir.is_relative_to(repo_root):
        raise ValueError(f"plugin commands path leaves the repository: {command_dir}")
    if not command_dir.is_dir():
        raise FileNotFoundError(f"plugin commands path is not a directory: {command_dir}")
    commands: dict[str, Path] = {}
    for path in sorted(command_dir.glob("*.md")):
        if not path.resolve().is_relative_to(repo_root):
            raise ValueError(f"command file resolves outside the repository: {path}")
        commands[path.stem] = path
    return commands


def parse_front_matter(text: str) -> tuple[dict[str, str] | None, str]:
    """Split optional front matter from the body; a file without it is all body.

    Returns None for the fields when there is no front matter block, so an
    empty block ({}) stays distinguishable from an absent one. A leading `---`
    that never closes is malformed and raises ValueError rather than reading
    as absent.
    """
    match = FRONT_MATTER.match(text)
    if not match:
        if text.startswith("---"):
            raise ValueError("front matter opens with --- but never closes")
        return None, text
    fields: dict[str, str] = {}
    for line in match.group(1).splitlines():
        key, sep, value = line.partition(":")
        if sep:
            fields[key.strip()] = value.strip()
    return fields, match.group(2) or ""


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
        try:
            fields, body = parse_front_matter(path.read_text(encoding="utf-8"))
        except ValueError as error:
            found.append(f"/{name}: malformed front matter: {error}")
            continue
        found.extend(check_invocations_and_metadata(name, fields, body, manifest))
    return found


def check_invocations_and_metadata(
    name: str, fields: dict[str, str] | None, body: str, manifest: dict[str, set[str]]
) -> list[str]:
    """Validate one command's front matter and its `pulp ...` invocations."""
    found: list[str] = []
    if fields is not None:
        if fields.get("name") != name:
            found.append(f"/{name}: front matter name is {fields.get('name')!r}")
        if not fields.get("description", "").strip():
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

    def test_unknown_subcommand_absent_everywhere_is_reported(self) -> None:
        path = self.tmp / ".claude/commands/inspect.md"
        path.write_text(
            path.read_text(encoding="utf-8") + "\nThen `pulp inspect fabricated`.\n", encoding="utf-8"
        )
        self.assertIn("`pulp inspect fabricated` is not a subcommand", " ".join(problems(self.tmp)))

    def test_empty_front_matter_is_reported(self) -> None:
        path = self.tmp / ".claude/commands/design.md"
        _, body = parse_front_matter(path.read_text(encoding="utf-8"))
        path.write_text("---\n\n---\n" + body, encoding="utf-8")
        reported = " ".join(problems(self.tmp))
        self.assertIn("/design: front matter name is None", reported)
        self.assertIn("/design: front matter needs a description", reported)

    def test_short_but_present_description_is_accepted(self) -> None:
        found = check_invocations_and_metadata(
            "design", {"name": "design", "description": "Short"}, "Run `pulp design`.", {"design": set()}
        )
        self.assertEqual(found, [])

    def test_blank_description_is_reported(self) -> None:
        found = check_invocations_and_metadata(
            "design", {"name": "design", "description": " "}, "Run `pulp design`.", {"design": set()}
        )
        self.assertEqual(found, ["/design: front matter needs a description"])

    def test_unterminated_front_matter_is_reported(self) -> None:
        path = self.tmp / ".claude/commands/design.md"
        _, body = parse_front_matter(path.read_text(encoding="utf-8"))
        path.write_text("---\nname: design\ndescription: Open the design tool\n" + body, encoding="utf-8")
        self.assertIn("/design: malformed front matter", " ".join(problems(self.tmp)))

    def test_commands_path_outside_the_repository_is_reported(self) -> None:
        outside = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, outside, ignore_errors=True)
        shutil.copytree(self.tmp / ".claude/commands", outside / "commands")
        manifest = self.tmp / ".claude-plugin/plugin.json"
        data = json.loads(manifest.read_text(encoding="utf-8"))
        for value in (str(outside / "commands"), "../" + outside.name + "/commands"):
            data["commands"] = value
            manifest.write_text(json.dumps(data), encoding="utf-8")
            self.assertIn("leaves the repository", " ".join(problems(self.tmp)), value)

    def test_symlinked_command_file_outside_the_repository_is_reported(self) -> None:
        outside = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, outside, ignore_errors=True)
        target = outside / "design.md"
        shutil.copy(self.tmp / ".claude/commands/design.md", target)
        link = self.tmp / ".claude/commands/design.md"
        link.unlink()
        link.symlink_to(target)
        self.assertIn("resolves outside the repository", " ".join(problems(self.tmp)))

    def test_missing_plugin_commands_directory_is_reported(self) -> None:
        manifest = self.tmp / ".claude-plugin/plugin.json"
        data = json.loads(manifest.read_text(encoding="utf-8"))
        data["commands"] = "./nowhere"
        manifest.write_text(json.dumps(data), encoding="utf-8")
        self.assertIn("does not resolve a command directory", " ".join(problems(self.tmp)))


if __name__ == "__main__":
    unittest.main()
