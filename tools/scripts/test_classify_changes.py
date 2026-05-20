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

import importlib.util
import os
import subprocess
import sys
import unittest
from pathlib import Path

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

    def test_files_mode_with_code(self) -> None:
        r = self._run("--mode=files", "--json", "core/signal/src/fft.cpp")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('"native_build_required": true', r.stdout)

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
