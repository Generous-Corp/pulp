#!/usr/bin/env python3
"""Tests for the Pulp tooling-disposition completeness gate."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("pulp_tooling_disposition.py")
SPEC = importlib.util.spec_from_file_location("pulp_tooling_disposition", MODULE_PATH)
assert SPEC and SPEC.loader
tooling = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tooling)


class ToolingDispositionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "core").mkdir()
        (self.root / "CMakeLists.txt").write_text("project(Fixture)\n")
        (self.root / "tools/cli").mkdir(parents=True)
        (self.root / "tools/cli/pulp_cli.cpp").write_text(
            'static const Command commands[] = {\n  {"build", "Build", cmd_build},\n};\n'
        )
        (self.root / "docs/status").mkdir(parents=True)
        (self.root / "docs/status/cli-commands.yaml").write_text(
            "commands:\n"
            "  - name: build\n"
            "    args:\n"
            "      - name: --release\n"
            "        kind: flag\n"
        )
        (self.root / ".claude/commands").mkdir(parents=True)
        (self.root / ".claude/commands/build.md").write_text("Build.\n")
        (self.root / ".agents/skills/build").mkdir(parents=True)
        (self.root / ".agents/skills/build/SKILL.md").write_text(
            "---\nname: build\ndescription: Build fixture.\n---\n"
        )
        (self.root / "tools/mcp").mkdir(parents=True)
        (self.root / "tools/mcp/pulp_mcp.cpp").write_text(
            'const char* tools = R"({"name":"pulp_build"})";\n'
        )
        (self.root / ".claude-plugin").mkdir(parents=True)
        (self.root / ".claude-plugin/plugin.json").write_text(json.dumps({
            "name": "pulp", "version": "1.0.0",
            "commands": "./.claude/commands", "skills": "./.agents/skills",
        }))
        (self.root / ".claude-plugin/marketplace.json").write_text(json.dumps({
            "plugins": [{
                "name": "pulp", "version": "1.0.0", "source": "./",
                "min_cli_version": "1.0.0",
            }]
        }))
        (self.root / ".mcp.json").write_text(json.dumps({
            "mcpServers": {"pulp": {"command": "./pulp-mcp", "env": {}}}
        }))
        self.map_path = self.root / "map.json"
        inventory, issues = tooling.collect_inventory(self.root)
        self.assertEqual([], issues)
        document = tooling.render_map(inventory)
        for rows in document["entries"].values():
            for row in rows:
                row["disposition"] = "pulp-owned"
        self.map_path.write_text(json.dumps(document, indent=2) + "\n")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def assert_stale_after(self, mutate) -> None:
        self.assertEqual([], tooling.validate_map(self.root, self.map_path))
        mutate()
        issues = tooling.validate_map(self.root, self.map_path)
        self.assertTrue(any("inventory is stale" in issue for issue in issues), issues)

    def test_new_cli_command_is_detected(self) -> None:
        def mutate() -> None:
            path = self.root / "tools/cli/pulp_cli.cpp"
            path.write_text(path.read_text().replace(
                '  {"build", "Build", cmd_build},',
                '  {"build", "Build", cmd_build},\n  {"run", "Run", cmd_run},',
            ))
        self.assert_stale_after(mutate)

    def test_new_cli_flag_is_detected(self) -> None:
        def mutate() -> None:
            path = self.root / "docs/status/cli-commands.yaml"
            path.write_text(path.read_text() + "      - name: --debug\n        kind: flag\n")
        self.assert_stale_after(mutate)

    def test_new_claude_command_is_detected(self) -> None:
        self.assert_stale_after(
            lambda: (self.root / ".claude/commands/run.md").write_text("Run.\n")
        )

    def test_new_agent_skill_is_detected(self) -> None:
        def mutate() -> None:
            path = self.root / ".agents/skills/run/SKILL.md"
            path.parent.mkdir(parents=True)
            path.write_text("---\nname: run\ndescription: Run fixture.\n---\n")
        self.assert_stale_after(mutate)

    def test_new_mcp_tool_is_detected(self) -> None:
        def mutate() -> None:
            path = self.root / "tools/mcp/pulp_mcp.cpp"
            path.write_text(path.read_text() + 'const char* more = R"({"name":"pulp_run"})";\n')
        self.assert_stale_after(mutate)

    def test_new_surface_cannot_be_silently_classified(self) -> None:
        inventory, _ = tooling.collect_inventory(self.root)
        inventory["mcp_tools"].append({"name": "pulp_future"})
        document = tooling.render_map(inventory, json.loads(self.map_path.read_text()))
        future = next(row for row in document["entries"]["mcp_tools"] if row["name"] == "pulp_future")
        self.assertEqual("UNCLASSIFIED", future["disposition"])

    def test_cli_argument_provenance_is_enforced(self) -> None:
        document = json.loads(self.map_path.read_text())
        document["cli_argument_provenance"]["limitation"] = "claims parser parity"
        self.map_path.write_text(json.dumps(document, indent=2) + "\n")
        issues = tooling.validate_map(self.root, self.map_path)
        self.assertTrue(any("inventory is stale" in issue for issue in issues), issues)

    def test_new_plugin_registration_is_detected(self) -> None:
        def mutate() -> None:
            path = self.root / ".mcp.json"
            document = json.loads(path.read_text())
            document["mcpServers"]["future"] = {
                "command": "./future-mcp", "env": {"MODE": "test"}
            }
            path.write_text(json.dumps(document))
        self.assert_stale_after(mutate)

    def test_plugin_manifest_registration_change_is_detected(self) -> None:
        def mutate() -> None:
            path = self.root / ".claude-plugin/plugin.json"
            document = json.loads(path.read_text())
            document["commands"] = "./commands-v2"
            path.write_text(json.dumps(document))
        self.assert_stale_after(mutate)

    def test_plugin_manifest_new_affordance_is_detected(self) -> None:
        def mutate() -> None:
            path = self.root / ".claude-plugin/plugin.json"
            document = json.loads(path.read_text())
            document["hooks"] = "./hooks/hooks.json"
            path.write_text(json.dumps(document))
        self.assert_stale_after(mutate)

    def test_marketplace_registration_change_is_detected(self) -> None:
        def mutate() -> None:
            path = self.root / ".claude-plugin/marketplace.json"
            document = json.loads(path.read_text())
            document["version"] = "1.0.1"
            document["plugins"][0]["version"] = "1.0.1"
            path.write_text(json.dumps(document))
        self.assert_stale_after(mutate)

    def test_mcp_registration_configuration_change_is_detected(self) -> None:
        def mutate() -> None:
            path = self.root / ".mcp.json"
            document = json.loads(path.read_text())
            document["mcpServers"]["pulp"]["args"] = ["--stdio"]
            path.write_text(json.dumps(document))
        self.assert_stale_after(mutate)


if __name__ == "__main__":
    unittest.main()
