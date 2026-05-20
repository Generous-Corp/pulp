#!/usr/bin/env python3
"""Tests for classify_changes.py.

The classifier is safety-critical: a wrong "skip" lets a real
regression merge without a native build. These tests pin the
fail-closed contract — especially that anything NOT explicitly
skip-safe forces the native build.

Run:
    python3 tools/scripts/test_classify_changes.py
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

THIS_DIR = Path(__file__).parent
SCRIPT = THIS_DIR / "classify_changes.py"

_spec = importlib.util.spec_from_file_location("classify_changes", SCRIPT)
classify = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(classify)


class SkipSafeTests(unittest.TestCase):
    def test_markdown_anywhere_is_skip_safe(self) -> None:
        for path in ("README.md", "docs/guide.md", "core/view/NOTES.md",
                     ".agents/skills/ci/SKILL.md", "CHANGELOG.md"):
            self.assertTrue(classify.is_skip_safe(path), path)

    def test_skip_safe_prefixes(self) -> None:
        for path in ("docs/reference/cli.md", "docs/assets/logo.png",
                     "planning/STATUS.md", ".githooks/pre-push",
                     ".shipyard/config.toml", ".shipyard.local/config.toml"):
            self.assertTrue(classify.is_skip_safe(path), path)

    def test_skip_safe_exact(self) -> None:
        for path in (".gitignore", ".gitattributes", "CODEOWNERS"):
            self.assertTrue(classify.is_skip_safe(path), path)

    def test_docs_migrations_md_is_NOT_skip_safe(self) -> None:
        # docs/migrations/*.md is globbed with CONFIGURE_DEPENDS into the
        # generated migration_index.cpp by tools/cli/CMakeLists.txt — the
        # deny-list must override the .md and docs/ skip-safe rules.
        for path in ("docs/migrations/2026-05-01-release.md",
                     "docs/migrations/v2-upgrade.md"):
            self.assertFalse(classify.is_skip_safe(path), path)

    def test_docs_migrations_prefix_boundary(self) -> None:
        # Only the real docs/migrations/ tree is generated into C++.
        self.assertTrue(classify.is_skip_safe("docs/migrations-guide.md"))
        self.assertTrue(classify.is_skip_safe("docs/migrations_old/foo.md"))
        self.assertFalse(classify.is_skip_safe("docs/migrations/foo.md"))

    def test_build_inputs_are_NOT_skip_safe(self) -> None:
        for path in ("core/signal/src/fft.cpp",
                     "core/view/include/pulp/view/view.hpp",
                     "CMakeLists.txt", "core/audio/CMakeLists.txt",
                     "tools/cmake/PulpUtils.cmake", "apple/Sources/x.swift",
                     "examples/pulp-gain/main.cpp", "test/test_state.cpp",
                     "setup.sh", "Package.swift",
                     "core/view/js/web-compat-element.js",
                     ".github/workflows/build.yml",
                     "tools/scripts/classify_changes.py",
                     "tools/scripts/resolve_runs_on.py",
                     ".shipyard.toml", "compat.json"):
            self.assertFalse(classify.is_skip_safe(path), path)

    def test_empty_path_is_not_skip_safe(self) -> None:
        self.assertFalse(classify.is_skip_safe(""))


class NativeBuildRequiredTests(unittest.TestCase):
    def test_empty_diff_forces_build(self) -> None:
        # Fail-closed: unknown diff -> build.
        self.assertTrue(classify.native_build_required([]))

    def test_all_docs_skips_build(self) -> None:
        self.assertFalse(classify.native_build_required(
            ["README.md", "docs/guide.md", "CHANGELOG.md"]))

    def test_migration_doc_forces_build(self) -> None:
        # A migrations-only PR changes generated C++ — must build.
        self.assertTrue(classify.native_build_required(
            ["docs/migrations/2026-05-19-foo.md"]))

    def test_migration_doc_among_plain_docs_forces_build(self) -> None:
        # One migration doc among ordinary docs still forces the build.
        self.assertTrue(classify.native_build_required(
            ["README.md", "docs/guide.md",
             "docs/migrations/2026-05-19-foo.md"]))

    def test_any_code_file_forces_build(self) -> None:
        # One code file among many docs still forces the build.
        self.assertTrue(classify.native_build_required(
            ["README.md", "docs/guide.md", "core/signal/src/fft.cpp"]))

    def test_pure_code_forces_build(self) -> None:
        self.assertTrue(classify.native_build_required(
            ["core/midi/src/mpe_voice_tracker.cpp"]))

    def test_skill_only_pr_skips_build(self) -> None:
        # A skill-doc-only PR (markdown) is skip-safe.
        self.assertFalse(classify.native_build_required(
            [".agents/skills/ci/SKILL.md"]))

    def test_workflow_change_forces_build(self) -> None:
        # Changing build.yml itself must get a real run to validate.
        self.assertTrue(classify.native_build_required(
            [".github/workflows/build.yml"]))

    def test_classifier_change_forces_build(self) -> None:
        # Changing the classifier must get a real run.
        self.assertTrue(classify.native_build_required(
            ["tools/scripts/classify_changes.py"]))

    def test_tools_scripts_not_skip_safe(self) -> None:
        # tools/scripts/** is intentionally NOT skip-safe in v1 — some
        # scripts are build-coupled. Conservative > clever.
        self.assertTrue(classify.native_build_required(
            ["tools/scripts/source_tree_pollution_check.py"]))


class DiffModeTests(unittest.TestCase):
    def test_changed_files_from_diff_disables_rename_detection(self) -> None:
        completed = subprocess.CompletedProcess(
            ["git"],
            0,
            stdout="\nREADME.md\n core/signal/src/fft.cpp \n\n",
            stderr="",
        )

        with mock.patch.object(
            classify.subprocess, "run", return_value=completed
        ) as run:
            files = classify._changed_files_from_diff("origin/main")

        self.assertEqual(files, ["README.md", "core/signal/src/fft.cpp"])
        run.assert_called_once_with(
            ["git", "diff", "--no-renames", "--name-only", "origin/main...HEAD"],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_changed_files_from_diff_returns_none_on_git_failure(self) -> None:
        completed = subprocess.CompletedProcess(
            ["git"],
            128,
            stdout="",
            stderr="fatal: bad revision 'missing...HEAD'",
        )
        stderr = io.StringIO()

        with mock.patch.object(classify.subprocess, "run", return_value=completed):
            with contextlib.redirect_stderr(stderr):
                files = classify._changed_files_from_diff("missing")

        self.assertIsNone(files)
        self.assertIn("[classify] git diff failed (exit 128)", stderr.getvalue())

    def test_main_diff_empty_output_is_fail_closed_json(self) -> None:
        completed = subprocess.CompletedProcess(["git"], 0, stdout="\n\n", stderr="")
        stdout = io.StringIO()
        stderr = io.StringIO()

        with mock.patch.object(classify.subprocess, "run", return_value=completed):
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = classify.main(["--mode=diff", "--json"])

        self.assertEqual(rc, 0)
        payload = json.loads(stdout.getvalue())
        self.assertTrue(payload["native_build_required"])
        self.assertEqual(payload["changed_file_count"], 0)
        self.assertIn("no changed files", payload["reason"])
        self.assertIn("native_build_required=true", stderr.getvalue())

    def test_main_diff_failure_is_json_fail_closed(self) -> None:
        completed = subprocess.CompletedProcess(
            ["git"],
            128,
            stdout="",
            stderr="fatal: bad revision 'missing...HEAD'",
        )
        stdout = io.StringIO()
        stderr = io.StringIO()

        with mock.patch.object(classify.subprocess, "run", return_value=completed):
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = classify.main(["--mode=diff", "--base", "missing", "--json"])

        self.assertEqual(rc, 0)
        payload = json.loads(stdout.getvalue())
        self.assertTrue(payload["native_build_required"])
        self.assertEqual(payload["changed_file_count"], 0)
        self.assertIn("fail-closed", payload["reason"])
        self.assertIn("native_build_required=true", stderr.getvalue())


class CliTests(unittest.TestCase):
    def _run(self, *args: str, env_extra: dict | None = None
             ) -> subprocess.CompletedProcess:
        env = os.environ.copy()
        if env_extra:
            env.update(env_extra)
        return subprocess.run(
            [sys.executable, str(SCRIPT), *args],
            capture_output=True, text=True, check=False, env=env,
        )

    def test_files_mode_docs_only(self) -> None:
        r = self._run("--mode=files", "--json", "README.md", "docs/x.md")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('"native_build_required": false', r.stdout)

    def test_files_mode_docs_only_reports_reason_and_count(self) -> None:
        r = self._run("--mode=files", "--json", "README.md", "docs/x.md")
        payload = json.loads(r.stdout)
        self.assertEqual(payload["changed_file_count"], 2)
        self.assertFalse(payload["native_build_required"])
        self.assertIn("skip-safe", payload["reason"])
        self.assertIn("native_build_required=false", r.stderr)

    def test_files_mode_with_code(self) -> None:
        r = self._run("--mode=files", "--json", "core/signal/src/fft.cpp")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('"native_build_required": true', r.stdout)

    def test_files_mode_truncates_long_native_input_reason(self) -> None:
        files = [f"core/signal/src/file_{i}.cpp" for i in range(10)]
        r = self._run("--mode=files", "--json", *files)
        self.assertEqual(r.returncode, 0, r.stderr)
        payload = json.loads(r.stdout)
        self.assertTrue(payload["native_build_required"])
        self.assertIn("file_0.cpp", payload["reason"])
        self.assertIn("(+2 more)", payload["reason"])

    def test_files_mode_empty_is_failclosed(self) -> None:
        # --mode=files with no files -> empty -> fail-closed -> true.
        r = self._run("--mode=files", "--json")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('"native_build_required": true', r.stdout)

    def test_writes_github_output(self) -> None:
        import tempfile
        with tempfile.NamedTemporaryFile("w+", suffix=".txt",
                                         delete=False) as fh:
            out_path = fh.name
        try:
            r = self._run("--mode=files", "README.md",
                           env_extra={"GITHUB_OUTPUT": out_path})
            self.assertEqual(r.returncode, 0, r.stderr)
            content = Path(out_path).read_text()
            self.assertIn("native_build_required=false", content)
        finally:
            os.unlink(out_path)

    def test_writes_github_output_appends_true_for_code(self) -> None:
        import tempfile
        with tempfile.NamedTemporaryFile("w+", suffix=".txt",
                                         delete=False) as fh:
            fh.write("existing=1\n")
            out_path = fh.name
        try:
            r = self._run("--mode=files", "core/view/src/widget.cpp",
                          env_extra={"GITHUB_OUTPUT": out_path})
            self.assertEqual(r.returncode, 0, r.stderr)
            content = Path(out_path).read_text()
            self.assertIn("existing=1\n", content)
            self.assertTrue(content.endswith("native_build_required=true\n"))
        finally:
            os.unlink(out_path)

    def test_invalid_mode_exits_usage_error(self) -> None:
        r = self._run("--mode=bogus")
        self.assertEqual(r.returncode, 2)
        self.assertIn("invalid choice", r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
