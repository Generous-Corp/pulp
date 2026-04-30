#!/usr/bin/env python3
"""Additional edge coverage for tools/scripts/cli_sync_check.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).parent / "cli_sync_check.py"
spec = importlib.util.spec_from_file_location("cli_sync_check", SCRIPT)
assert spec and spec.loader
csc = importlib.util.module_from_spec(spec)
sys.modules["cli_sync_check"] = csc
spec.loader.exec_module(csc)


@contextlib.contextmanager
def chdir(path: pathlib.Path):
    old = pathlib.Path.cwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(old)


@contextlib.contextmanager
def argv(args: list[str]):
    old = sys.argv[:]
    sys.argv = args[:]
    try:
        yield
    finally:
        sys.argv = old


def make_repo(root: pathlib.Path) -> pathlib.Path:
    root.mkdir()
    (root / "core").mkdir()
    (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.24)\n")
    (root / "tools" / "cli").mkdir(parents=True)
    (root / "docs" / "status").mkdir(parents=True)
    (root / ".claude" / "commands").mkdir(parents=True)
    (root / ".agents" / "skills" / "ci").mkdir(parents=True)
    return root


class CommandExtractionEdgeTests(unittest.TestCase):
    def test_extract_command_table_names_includes_secondary_tables_and_manual_dispatch(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            (root / "tools" / "cli" / "pulp_cli.cpp").write_text(
                """
                static const Command commands[] = {
                    {"build", "Build", handle_build},
                \n};
                static const ScriptCommand script_commands[] = {
                    {"docs", "Docs", "tools/scripts/docs.py"},
                \n};
                static const BinaryCommand binary_commands[] = {
                    {"host", "Host", "pulp-host"},
                \n};

                if (command == "projects") return run_projects();
                if (command == "add-component") return run_legacy_add_component();
                if (command == "install") return run_hidden_install_alias();
                """,
                encoding="utf-8",
            )

            self.assertEqual(
                csc.extract_command_table_names(root),
                {"build", "docs", "host", "projects"},
            )

    def test_extract_yaml_commands_stops_at_next_top_level_block(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            (root / "docs" / "status" / "cli-commands.yaml").write_text(
                """
metadata:
  - name: ignored-before-commands
commands:
  - name: build
    summary: Build
  - name: ship
    summary: Ship
notes:
  - name: ignored-after-commands
                """,
                encoding="utf-8",
            )

            self.assertEqual(csc.extract_yaml_commands(root), {"build", "ship"})

    def test_optional_directory_extractors_return_empty_when_directories_are_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            (root / ".claude" / "commands").rmdir()
            (root / ".claude").rmdir()
            (root / ".agents" / "skills" / "ci").rmdir()
            (root / ".agents" / "skills").rmdir()

            self.assertEqual(csc.extract_slash_commands(root), set())
            self.assertEqual(csc.extract_skill_cli_refs(root), {})


class MainOutputEdgeTests(unittest.TestCase):
    def test_main_json_reports_cli_only_mismatch_without_yaml_only_branch(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            (root / "tools" / "cli" / "pulp_cli.cpp").write_text(
                """
                static const Command commands[] = {
                    {"build", "Build", handle_build},
                    {"ship", "Ship", handle_ship},
                \n};
                """,
                encoding="utf-8",
            )
            (root / "docs" / "status" / "cli-commands.yaml").write_text(
                "commands:\n  - name: build\n    summary: Build\n",
                encoding="utf-8",
            )

            out = io.StringIO()
            with chdir(root), argv(["cli_sync_check.py", "--strict", "--json"]):
                with contextlib.redirect_stdout(out):
                    rc = csc.main()

            self.assertEqual(rc, 1)
            text = out.getvalue()
            self.assertIn(
                "Commands in CLI but not in cli-commands.yaml: ship", text
            )
            self.assertNotIn("Commands in cli-commands.yaml but not in CLI", text)

    def test_main_json_reports_yaml_only_mismatch_without_cli_only_branch(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            (root / "tools" / "cli" / "pulp_cli.cpp").write_text(
                """
                static const Command commands[] = {
                    {"build", "Build", handle_build},
                \n};
                """,
                encoding="utf-8",
            )
            (root / "docs" / "status" / "cli-commands.yaml").write_text(
                """
commands:
  - name: build
    summary: Build
  - name: ship
    summary: Ship
                """,
                encoding="utf-8",
            )

            out = io.StringIO()
            with chdir(root), argv(["cli_sync_check.py", "--strict", "--json"]):
                with contextlib.redirect_stdout(out):
                    rc = csc.main()

            self.assertEqual(rc, 1)
            text = out.getvalue()
            self.assertNotIn("Commands in CLI but not in cli-commands.yaml", text)
            self.assertIn(
                "Commands in cli-commands.yaml but not in CLI: ship", text
            )

    def test_main_text_output_passes_with_allowed_skill_subcommand_refs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            (root / "tools" / "cli" / "pulp_cli.cpp").write_text(
                """
                static const Command commands[] = {
                    {"build", "Build", handle_build},
                \n};
                """,
                encoding="utf-8",
            )
            (root / "docs" / "status" / "cli-commands.yaml").write_text(
                "commands:\n  - name: build\n    summary: Build\n",
                encoding="utf-8",
            )
            (root / ".claude" / "commands" / "build.md").write_text(
                "# build\n", encoding="utf-8"
            )
            (root / ".agents" / "skills" / "ci" / "SKILL.md").write_text(
                "Use `pulp version`, `pulp bump`, and `pulp check` helpers.\n",
                encoding="utf-8",
            )

            out = io.StringIO()
            with chdir(root), argv(["cli_sync_check.py"]):
                with contextlib.redirect_stdout(out):
                    rc = csc.main()

            self.assertEqual(rc, 0)
            text = out.getvalue()
            self.assertIn("CLI Sync Check", text)
            self.assertIn("All expected commands have slash commands", text)
            self.assertIn("All skill CLI references are valid", text)
            self.assertIn("All checks passed.", text)

    def test_main_json_reports_yaml_only_help_as_success(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            (root / "tools" / "cli" / "pulp_cli.cpp").write_text(
                """
                static const Command commands[] = {
                    {"build", "Build", handle_build},
                \n};
                """,
                encoding="utf-8",
            )
            (root / "docs" / "status" / "cli-commands.yaml").write_text(
                """
commands:
  - name: build
    summary: Build
  - name: help
    summary: Help
                """,
                encoding="utf-8",
            )
            (root / ".claude" / "commands" / "build.md").write_text(
                "# build\n", encoding="utf-8"
            )

            out = io.StringIO()
            with chdir(root), argv(["cli_sync_check.py", "--strict", "--json"]):
                with contextlib.redirect_stdout(out):
                    rc = csc.main()

            self.assertEqual(rc, 0)
            self.assertIn('"issues": 0', out.getvalue())

    def test_main_text_output_renders_failures_warnings_and_issue_count(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            (root / "tools" / "cli" / "pulp_cli.cpp").write_text(
                """
                static const Command commands[] = {
                    {"build", "Build", handle_build},
                    {"ci-local", "Local CI", handle_ci_local},
                \n};
                """,
                encoding="utf-8",
            )
            (root / "docs" / "status" / "cli-commands.yaml").write_text(
                """
commands:
  - name: ci-local
    summary: Local CI
  - name: ship
    summary: Ship
                """,
                encoding="utf-8",
            )
            (root / ".agents" / "skills" / "ci" / "SKILL.md").write_text(
                "Stale reference to `pulp vanished`.\n", encoding="utf-8"
            )

            out = io.StringIO()
            with chdir(root), argv(["cli_sync_check.py"]):
                with contextlib.redirect_stdout(out):
                    rc = csc.main()

            self.assertEqual(rc, 0)
            text = out.getvalue()
            self.assertIn("Commands in CLI but not in cli-commands.yaml: build", text)
            self.assertIn("Commands in cli-commands.yaml but not in CLI: ship", text)
            self.assertIn("Missing slash commands (not in skip-list): build", text)
            self.assertIn("pulp vanished", text)
            self.assertIn("2 issue(s) found.", text)

    def test_script_entrypoint_exits_nonzero_outside_repo(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            proc = subprocess.run(
                [sys.executable, str(SCRIPT)],
                cwd=td,
                text=True,
                capture_output=True,
                check=False,
            )

        self.assertEqual(proc.returncode, 1)
        self.assertIn("not in a Pulp project", proc.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
