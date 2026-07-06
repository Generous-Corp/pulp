#!/usr/bin/env python3
"""Tests for docs_noise_lint.py.

Run via:
    python3 tools/scripts/test_docs_noise_lint.py
"""
from __future__ import annotations

import importlib.util
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

THIS_DIR = Path(__file__).parent
SCRIPT = THIS_DIR / "docs_noise_lint.py"

spec = importlib.util.spec_from_file_location("docs_noise_lint", SCRIPT)
assert spec and spec.loader
noise = importlib.util.module_from_spec(spec)
sys.modules["docs_noise_lint"] = noise
spec.loader.exec_module(noise)


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

    def test_explicit_files_accept_absolute_paths_and_skip_missing(self) -> None:
        noisy = self._write("docs/reference/noisy.md", "See #123.\n")
        self._write("docs/reports/skip.md", "See #456.\n")

        files = noise._iter_explicit_files(
            self.tmpdir,
            [str(noisy), "missing.md", "docs/reports/skip.md"],
        )

        self.assertEqual(files, [noisy])

    def test_default_files_skip_directories_and_allowlisted_paths(self) -> None:
        self._write("docs/reference/page.md", "Current behavior.\n")
        (self.tmpdir / "docs/reference/folder.md").mkdir(parents=True)
        self._write(".agents/skills/ci/SKILL.md", "See #123.\n")

        with mock.patch.object(
            noise,
            "DEFAULT_SCAN_GLOBS",
            ("docs/reference/**/*.md", "docs/reference/**/*.md", ".agents/skills/**/SKILL.md"),
        ):
            files = [
                noise._norm_path(path, self.tmpdir)
                for path in noise._iter_default_files(self.tmpdir)
            ]

        self.assertEqual(files, ["docs/reference/page.md"])

    def test_reviews_directory_is_allowlisted(self) -> None:
        self._write("docs/reviews/plan.md", "See #123 and Wave 4 inventory.\n")
        result = self._run("--mode=report", "docs/reviews/plan.md")
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

    def test_root_must_be_directory(self) -> None:
        missing = self.tmpdir / "missing-root"
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(missing)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("root is not a directory", result.stderr)

    def test_helper_edges_for_line_maps_and_paths(self) -> None:
        outside = Path(tempfile.mkdtemp(prefix="pulp-docs-noise-outside-")) / "outside.md"
        outside.write_text("See #123.\n", encoding="utf-8")
        self.addCleanup(lambda: shutil.rmtree(outside.parent, ignore_errors=True))
        self.assertEqual(noise._norm_path(outside, self.tmpdir), outside.as_posix())

        target: noise.LineMap = {"all.md": None}
        noise._merge_line_map(target, {"all.md": {1}, "new.md": None})
        self.assertIsNone(target["all.md"])
        self.assertIsNone(target["new.md"])

        parsed = noise._parse_unified_zero_diff(
            "+++ b/docs/reference/page.md\n"
            "@@ malformed hunk\n"
            "@@ -1,0 +4,0 @@\n"
            "@@ -1 +8,2 @@\n"
        )
        self.assertEqual(parsed, {"docs/reference/page.md": {8, 9}})

        failed = subprocess.CompletedProcess(["git"], 1, stdout="", stderr="bad")
        with mock.patch.object(noise.subprocess, "run", return_value=failed):
            self.assertEqual(noise._git_diff_line_map(self.tmpdir, []), {})
            self.assertEqual(noise._git_untracked_line_map(self.tmpdir), {})

    def test_scan_changed_map_filters_scope_allowlist_and_missing_paths(self) -> None:
        self._write("docs/reference/noisy.md", "See #123.\n")
        self._write(".agents/skills/ci/SKILL.md", "See #456.\n")
        self._write("docs/reports/noisy.md", "See #456.\n")
        self._write("README.md", "See #789.\n")

        with mock.patch.object(noise, "_git_changed_line_map", return_value={
            ".agents/skills/ci/SKILL.md": None,
            "README.md": None,
            "docs/reference/missing.md": None,
            "docs/reference/noisy.md": None,
            "docs/reports/noisy.md": None,
        }):
            findings = noise.scan(
                self.tmpdir,
                [],
                base="main",
                head="HEAD",
                scan_all=False,
            )

        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].path, "docs/reference/noisy.md")

    def test_scan_changed_map_respects_allowed_line_sets(self) -> None:
        self._write("docs/reference/noisy.md", "See #123.\nCurrent behavior.\n")
        with mock.patch.object(noise, "_git_changed_line_map", return_value={
            "docs/reference/noisy.md": {2},
        }):
            findings = noise.scan(
                self.tmpdir,
                [],
                base="main",
                head="HEAD",
                scan_all=False,
            )

        self.assertEqual(findings, [])

    def test_scan_falls_back_to_default_scope_outside_git(self) -> None:
        self._write("docs/reference/noisy.md", "See #123.\n")
        with mock.patch.object(noise, "_git_changed_line_map", return_value=None):
            findings = noise.scan(
                self.tmpdir,
                [],
                base="main",
                head="HEAD",
                scan_all=False,
            )

        self.assertEqual(len(findings), 1)

    def test_strip_inline_code_removes_multiple_spans(self) -> None:
        stripped = noise._strip_inline_code("Use `See #123` then ``Wave 4`` outside")
        self.assertNotIn("#123", stripped)
        self.assertNotIn("Wave 4", stripped)
        self.assertIn("outside", stripped)

    def test_format_findings_empty_is_empty(self) -> None:
        self.assertEqual(noise._format_findings([], "report"), "")

    def test_script_entrypoint_returns_zero_for_clean_root(self) -> None:
        self._write("docs/reference/clean.md", "Current behavior.\n")
        with mock.patch.object(sys, "argv", [str(SCRIPT), "--root", str(self.tmpdir), "--all"]):
            with self.assertRaises(SystemExit) as cm:
                runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(cm.exception.code, 0)

    def test_scan_file_read_errors_are_reported(self) -> None:
        directory = self.tmpdir / "docs" / "reference"
        with self.assertRaisesRegex(RuntimeError, "could not read"):
            noise.scan_file(directory, self.tmpdir)

    def test_format_findings_truncates_long_snippets(self) -> None:
        finding = noise.Finding(
            path="docs/reference/long.md",
            line_no=7,
            rule=noise.RULES[0],
            text="x" * 200,
        )
        formatted = noise._format_findings([finding], "report")
        self.assertIn("BLOCKED", formatted)
        self.assertIn("xxx...", formatted)
        self.assertLess(len(formatted.splitlines()[1]), 220)

    def test_main_reports_runtime_scan_errors(self) -> None:
        with mock.patch.object(noise, "scan", side_effect=RuntimeError("boom")), \
             mock.patch.object(noise.sys, "stderr") as stderr:
            self.assertEqual(noise.main(["--root", str(self.tmpdir)]), 2)
        stderr.write.assert_called_once()
        self.assertIn("boom", stderr.write.call_args.args[0])

    # --- source-comment scope -------------------------------------------------

    def _rules(self, *paths: str) -> set[str]:
        """Run an explicit-path scan and return the set of rule names hit."""
        abs_paths = [str(self.tmpdir / p) for p in paths]
        findings = noise.scan(
            self.tmpdir, abs_paths, base="x", head="y", scan_all=False
        )
        return {f.rule.name for f in findings}

    def test_source_comment_breadcrumbs_are_flagged(self) -> None:
        self._write("core/a.cpp", "// Phase 3 will add feedback\n")
        self._write("core/b.cpp", "/* Tier A Slice 7 groundwork */\n")
        self._write("core/c.mm", "// Codex P1: revisit later\n")
        self._write("core/d.cpp", "// Future v2 will use AES\n")
        self._write("core/e.cpp", "// see #1234 for context\n")
        self._write("tools/f.py", "# Phase 2 cleanup pending\n")
        self.assertIn("source-phase-label", self._rules("core/a.cpp"))
        self.assertIn("source-milestone-label", self._rules("core/b.cpp"))
        self.assertIn("source-agent-review-label", self._rules("core/c.mm"))
        self.assertIn("source-future-version", self._rules("core/d.cpp"))
        self.assertIn("source-issue-cite-phrase", self._rules("core/e.cpp"))
        self.assertIn("source-phase-label", self._rules("tools/f.py"))

    def test_catch2_workflow_tag_in_string_is_flagged(self) -> None:
        self._write("test/t.cpp", 'TEST_CASE("does x", "[phase3][rt-safety]") {}\n')
        self.assertIn("source-workflow-tag", self._rules("test/t.cpp"))

    def test_code_and_strings_do_not_false_positive(self) -> None:
        # `[coverage]` as an array index (not a string) is not a Catch2 tag.
        self._write("core/idx.cpp", "int y = arr[coverage];\n")
        # A `#1234` inside a string literal (URL) is not a comment cite.
        self._write("core/url.cpp", 'const char* u = "http://x/#1234";\n')
        # A `//` inside a string is not a comment.
        self._write("core/str.cpp", 'auto s = "a // Phase 3 in a string";\n')
        self.assertEqual(self._rules("core/idx.cpp"), set())
        self.assertEqual(self._rules("core/url.cpp"), set())
        self.assertEqual(self._rules("core/str.cpp"), set())

    def test_durable_source_comments_stay_clean(self) -> None:
        self._write("core/ok.mm", "// reconcile host param to avoid UI clobber\n")
        self._write("core/rt.cpp", "// no allocation on the audio thread\n")
        self.assertEqual(self._rules("core/ok.mm"), set())
        self.assertEqual(self._rules("core/rt.cpp"), set())

    def test_source_external_ref_and_skip_marker_are_exempt(self) -> None:
        self._write("core/ext.cpp", "// see WebGPU spec issue #1234 for rationale\n")
        self._write(
            "core/skip.cpp",
            "// Phase 3 name kept  docs-noise-lint: skip legacy public API name\n",
        )
        self.assertEqual(self._rules("core/ext.cpp"), set())
        self.assertEqual(self._rules("core/skip.cpp"), set())

    def test_source_is_diff_scoped_only_added_lines(self) -> None:
        self._init_git_repo()
        self._write("core/x.cpp", "// Phase 1 legacy note stays\nint x = 0;\n")
        self.assertEqual(self._git("add", "core/x.cpp").returncode, 0)
        self.assertEqual(self._git("commit", "-m", "seed").returncode, 0)
        with (self.tmpdir / "core/x.cpp").open("a", encoding="utf-8") as handle:
            handle.write("// Phase 9 new breadcrumb\n")
        result = self._run("--mode=report", "--base", "HEAD", "--head", "HEAD")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Phase 9", result.stderr)
        self.assertNotIn("Phase 1 legacy", result.stderr)

    def test_all_mode_does_not_scan_source(self) -> None:
        # --all is markdown-only; the source backlog must never block on it.
        self._write("core/legacy.cpp", "// Phase 3 legacy note\n")
        result = self._run("--mode=report", "--all")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_source_helpers_split_comments_and_strings(self) -> None:
        self.assertTrue(noise._path_is_source("core/a.cpp"))
        self.assertFalse(noise._path_is_source("external/x/a.cpp"))
        self.assertFalse(noise._path_is_source("docs/reference/a.md"))
        self.assertIsNone(noise._source_style("core/data.json"))
        comment, strings, in_block = noise._split_c_line('x(); // hi "not a string"', False)
        self.assertIn("hi", comment)
        self.assertEqual(strings, "")
        self.assertFalse(in_block)
        _, strings2, _ = noise._split_c_line('TEST_CASE("n", "[phase3]")', False)
        self.assertIn("[phase3]", strings2)
        cont, _, still = noise._split_c_line("/* opening block", False)
        self.assertIn("opening block", cont)
        self.assertTrue(still)
        self.assertEqual(noise._split_hash_line('x = "# not comment"  # real'), " real")


if __name__ == "__main__":
    unittest.main(verbosity=2)
