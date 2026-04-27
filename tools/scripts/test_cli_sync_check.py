#!/usr/bin/env python3
"""Unit tests for tools/scripts/cli_sync_check.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import pathlib
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


class RepoRootTests(unittest.TestCase):
    def test_find_repo_root_walks_up_from_nested_directory(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            nested = root / "tools" / "cli"

            with chdir(nested):
                self.assertEqual(csc.find_repo_root().resolve(), root.resolve())

    def test_find_repo_root_returns_none_outside_repo(self) -> None:
        with tempfile.TemporaryDirectory() as td, chdir(pathlib.Path(td)):
            self.assertIsNone(csc.find_repo_root())


class ExtractorTests(unittest.TestCase):
    def test_extractors_parse_cli_yaml_slash_commands_and_skill_refs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            (root / "tools" / "cli" / "pulp_cli.cpp").write_text(
                """
                static const Command commands[] = {
                    {"build", "Build project", handle_build},
                    {"ship", "Ship project", handle_ship},
                    {"ci-local", "Local CI", handle_ci_local},
                \n};
                """,
                encoding="utf-8",
            )
            (root / "docs" / "status" / "cli-commands.yaml").write_text(
                """
commands:
  - name: build
    summary: Build project
  - name: ship
    summary: Ship project
other:
  - name: not-a-command
                """,
                encoding="utf-8",
            )
            (root / ".claude" / "commands" / "build.md").write_text(
                "# build\n", encoding="utf-8"
            )
            (root / ".claude" / "commands" / "ship.md").write_text(
                "# ship\n", encoding="utf-8"
            )
            (root / ".agents" / "skills" / "ci" / "SKILL.md").write_text(
                "Use `pulp build` before `pulp ship`.\n", encoding="utf-8"
            )

            self.assertEqual(
                csc.extract_command_table_names(root), {"build", "ship", "ci-local"}
            )
            self.assertEqual(csc.extract_yaml_commands(root), {"build", "ship"})
            self.assertEqual(csc.extract_slash_commands(root), {"build", "ship"})
            self.assertEqual(csc.extract_skill_cli_refs(root), {"build": ["ci"], "ship": ["ci"]})

    def test_extractors_return_empty_results_for_missing_optional_files(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")

            self.assertEqual(csc.extract_command_table_names(root), set())
            self.assertEqual(csc.extract_yaml_commands(root), set())
            self.assertEqual(csc.extract_slash_commands(root), set())
            self.assertEqual(csc.extract_skill_cli_refs(root), {})


class MainTests(unittest.TestCase):
    def test_main_json_reports_cli_yaml_mismatch_as_strict_failure(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            (root / "tools" / "cli" / "pulp_cli.cpp").write_text(
                """
                static const Command commands[] = {
                    {"build", "Build", handle_build}
                \n};
                """,
                encoding="utf-8",
            )
            (root / "docs" / "status" / "cli-commands.yaml").write_text(
                "commands:\n  - name: ship\n    summary: Ship project\n",
                encoding="utf-8",
            )

            out = io.StringIO()
            with chdir(root), argv(["cli_sync_check.py", "--strict", "--json"]):
                with contextlib.redirect_stdout(out):
                    rc = csc.main()

            self.assertEqual(rc, 1)
            payload = json.loads(out.getvalue())
            self.assertEqual(payload["issues"], 2)
            messages = [check["message"] for check in payload["checks"]]
            self.assertIn("Commands in CLI but not in cli-commands.yaml: build", messages)
            self.assertIn("Commands in cli-commands.yaml but not in CLI: ship", messages)

    def test_main_returns_zero_when_only_warning_checks_fail(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            (root / "tools" / "cli" / "pulp_cli.cpp").write_text(
                """
                static const Command commands[] = {
                    {"build", "Build project", handle_build},
                    {"ship", "Ship project", handle_ship},
                \n};
                """,
                encoding="utf-8",
            )
            (root / "docs" / "status" / "cli-commands.yaml").write_text(
                """
commands:
  - name: build
    summary: Build project
  - name: ship
    summary: Ship project
                """,
                encoding="utf-8",
            )
            (root / ".claude" / "commands" / "build.md").write_text(
                "# build\n", encoding="utf-8"
            )
            (root / ".agents" / "skills" / "ci" / "SKILL.md").write_text(
                "Mentions `pulp build` and stale `pulp vanished`.\n",
                encoding="utf-8",
            )

            out = io.StringIO()
            with chdir(root), argv(["cli_sync_check.py", "--strict", "--json"]):
                with contextlib.redirect_stdout(out):
                    rc = csc.main()

            self.assertEqual(rc, 0)
            payload = json.loads(out.getvalue())
            self.assertEqual(payload["issues"], 0)
            self.assertTrue(
                any(
                    check["status"] == "warn" and "Missing slash commands" in check["message"]
                    for check in payload["checks"]
                )
            )
            self.assertTrue(
                any(
                    check["status"] == "warn" and "pulp vanished" in check["message"]
                    for check in payload["checks"]
                )
            )

    def test_main_reports_non_repo_as_error(self) -> None:
        with tempfile.TemporaryDirectory() as td, chdir(pathlib.Path(td)):
            err = io.StringIO()
            with argv(["cli_sync_check.py"]), contextlib.redirect_stderr(err):
                rc = csc.main()

        self.assertEqual(rc, 1)
        self.assertIn("not in a Pulp project", err.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
