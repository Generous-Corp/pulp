#!/usr/bin/env python3
"""Tests for docs_noise_lint.py.

Run via:
    python3 tools/scripts/test_docs_noise_lint.py
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

THIS_DIR = Path(__file__).parent
SCRIPT = THIS_DIR / "docs_noise_lint.py"


class DocsNoiseLintTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = Path(tempfile.mkdtemp(prefix="pulp-docs-noise-test-"))
        (self.tmpdir / "docs" / "reference").mkdir(parents=True)
        (self.tmpdir / ".agents" / "skills" / "demo").mkdir(parents=True)
        (self.tmpdir / ".agents" / "skills" / "ci").mkdir(parents=True)

    def tearDown(self) -> None:
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _write(self, rel: str, text: str) -> Path:
        path = self.tmpdir / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def _run(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(self.tmpdir), *args],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_clean_default_scope_passes(self) -> None:
        self._write("docs/reference/clean.md", "# Reference\nCurrent behavior only.\n")
        self._write(".agents/skills/demo/SKILL.md", "# Demo\nStable workflow.\n")
        result = self._run("--mode=report")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_positive_hit_per_denied_category(self) -> None:
        self._write(
            "docs/reference/noisy.md",
            "# Wave 4\n"
            "Agent A status note\n"
            "slice 3 of the migration\n"
            "sub-agent #24 draft\n"
            "audit-2026-05-14\n"
            "## Cleanup (2026-05-14)\n"
            "learned 2026-05-14\n"
            "See #123 for context\n"
            "Current behavior (#456)\n"
            "TODO: add test for #789\n"
            "planning artifact retained here\n",
        )
        result = self._run("--mode=report")
        self.assertEqual(result.returncode, 1)
        for rule in (
            "planning-wave-label",
            "planning-agent-label",
            "planning-slice-label",
            "sub-agent-label",
            "dated-audit-tag",
            "dated-heading-tag",
            "dated-cleanup-note",
            "issue-cite-phrase",
            "issue-parenthetical",
            "issue-only-todo",
            "workflow-artifact-phrase",
        ):
            self.assertIn(rule, result.stderr)

    def test_external_spec_refs_are_allowed(self) -> None:
        self._write(
            "docs/reference/specs.md",
            "CSSWG issue #123 defines the behavior.\n"
            "RFC 9110 section references are stable.\n"
            "CVE-2024-12345 is a stable security identifier.\n"
            "Skia bug (#456) documents the vendor quirk.\n",
        )
        result = self._run("--mode=report")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_skips_fenced_code_blocks_and_inline_backticks(self) -> None:
        self._write(
            "docs/reference/code.md",
            "```\n"
            "See #123 inside code.\n"
            "Wave 9 inside code.\n"
            "```\n"
            "Run `see #456` in the tracker export.\n"
            "Use `TODO: fix #789` only as a literal example.\n",
        )
        result = self._run("--mode=report")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_yaml_scans_description_shaped_lines_only(self) -> None:
        self._write(
            "docs/reference/noisy.yaml",
            "# See #123 in a YAML comment should not matter.\n"
            "key_without_value:\n"
            "description: See #456 in prose.\n",
        )
        result = self._run("--mode=report")
        self.assertEqual(result.returncode, 1)
        self.assertIn("See #456", result.stderr)
        self.assertNotIn("See #123", result.stderr)

    def test_per_line_skip_marker_allows_legitimate_internal_reference(self) -> None:
        self._write(
            "docs/reference/skip.md",
            "Regression note (#123) <!-- docs-noise-lint: skip — retained regression identifier -->\n",
        )
        result = self._run("--mode=report")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_ci_skill_is_file_allowlisted(self) -> None:
        self._write(".agents/skills/ci/SKILL.md", "# CI\nSlice 6 (#551).\n")
        result = self._run("--mode=report")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_explicit_allowlisted_paths_are_skipped(self) -> None:
        self._write("docs/reports/noisy.md", "See #123.\n")
        result = self._run("--mode=report", "docs/reports/noisy.md")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_hint_mode_reports_but_exits_zero(self) -> None:
        self._write("docs/reference/noisy.md", "See #123.\n")
        result = self._run("--mode=hint")
        self.assertEqual(result.returncode, 0)
        self.assertIn("HINT", result.stderr)
        self.assertIn("issue-cite-phrase", result.stderr)

    def test_report_mode_exits_one_on_findings(self) -> None:
        self._write("docs/reference/noisy.md", "See #123.\n")
        result = self._run("--mode=report")
        self.assertEqual(result.returncode, 1)
        self.assertIn("BLOCKED", result.stderr)

    def _git(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", "-C", str(self.tmpdir), *args],
            capture_output=True,
            text=True,
            check=False,
        )

    def _init_git_repo(self) -> None:
        self.assertEqual(self._git("init").returncode, 0)
        self.assertEqual(self._git("config", "user.email", "test@example.com").returncode, 0)
        self.assertEqual(self._git("config", "user.name", "Docs Noise Test").returncode, 0)

    def test_git_default_scans_only_changed_lines(self) -> None:
        self._init_git_repo()
        self._write("docs/reference/page.md", "# Page\nHistorical note: See #123.\n")
        self.assertEqual(self._git("add", "docs/reference/page.md").returncode, 0)
        self.assertEqual(self._git("commit", "-m", "seed").returncode, 0)

        with (self.tmpdir / "docs/reference/page.md").open("a", encoding="utf-8") as handle:
            handle.write("Current behavior stays stable.\n")
        clean_result = self._run("--mode=report", "--base", "HEAD", "--head", "HEAD")
        self.assertEqual(clean_result.returncode, 0, msg=clean_result.stderr)

        with (self.tmpdir / "docs/reference/page.md").open("a", encoding="utf-8") as handle:
            handle.write("New breadcrumb: See #456.\n")
        noisy_result = self._run("--mode=report", "--base", "HEAD", "--head", "HEAD")
        self.assertEqual(noisy_result.returncode, 1)
        self.assertIn("See #456", noisy_result.stderr)
        self.assertNotIn("See #123", noisy_result.stderr)

    def test_git_changed_line_inside_existing_fence_is_skipped(self) -> None:
        self._init_git_repo()
        self._write("docs/reference/code.md", "# Code\n```\nclean();\n```\n")
        self.assertEqual(self._git("add", "docs/reference/code.md").returncode, 0)
        self.assertEqual(self._git("commit", "-m", "seed").returncode, 0)
        self._write("docs/reference/code.md", "# Code\n```\nclean();\nSee #456 inside code.\n```\n")
        result = self._run("--mode=report", "--base", "HEAD", "--head", "HEAD")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_invocation_error_exits_two(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--mode=invalid"],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
