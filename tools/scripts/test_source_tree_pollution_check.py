#!/usr/bin/env python3
"""Tests for source_tree_pollution_check.py.

Run via:
    python3 tools/scripts/test_source_tree_pollution_check.py
"""
from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

THIS_DIR = Path(__file__).parent
SCRIPT = THIS_DIR / "source_tree_pollution_check.py"


def load_module():
    spec = importlib.util.spec_from_file_location("stpc", str(SCRIPT))
    stpc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stpc)
    return stpc


class SourceTreePollutionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = tempfile.mkdtemp(prefix="pulp-pollution-test-")
        self.cwd = os.getcwd()
        os.chdir(self.tmpdir)

    def tearDown(self) -> None:
        os.chdir(self.cwd)
        import shutil
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _run(self, *files: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--mode=files", *files],
            capture_output=True, text=True, check=False,
        )

    def test_clean_repo_passes(self) -> None:
        """No pollution patterns → exit 0, no stderr."""
        result = self._run()
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_blocks_pulp_toml_at_repo_root(self) -> None:
        """pulp.toml at repo root is always rejected, regardless of content."""
        Path("pulp.toml").write_text(
            "[pulp]\nsdk_version = \"0.1.0\"\nsdk_path = \"/anywhere\"\n"
        )
        result = self._run("pulp.toml")
        self.assertEqual(result.returncode, 1)
        self.assertIn("pulp.toml must never be in the source tree",
                      result.stderr)

    def test_blocks_cmakelists_with_clock_fixture(self) -> None:
        """CMakeLists.txt with project(Clock VERSION 1.0.0 → blocked."""
        Path("CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(Clock VERSION 1.0.0 LANGUAGES CXX)\n"
            "find_package(Pulp 0.1.0 REQUIRED)\n"
        )
        result = self._run("CMakeLists.txt")
        self.assertEqual(result.returncode, 1)
        self.assertIn("contains `project(Clock VERSION 1.0.0`",
                      result.stderr)
        self.assertIn("test_cli_shellout.cpp", result.stderr)

    def test_passes_real_pulp_cmakelists(self) -> None:
        """Real Pulp CMakeLists.txt (project(Pulp ...) → not blocked."""
        Path("CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(Pulp VERSION 0.81.0 LANGUAGES CXX C)\n"
        )
        result = self._run("CMakeLists.txt")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_accepts_uppercase_project_keyword(self) -> None:
        """pulp #1763 followup (Codex P2) — CMake command names are
        case-insensitive. PROJECT(Pulp ...) and project (Pulp ...) with
        whitespace before ( are valid; the original substring check
        rejected both."""
        Path("CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "PROJECT(Pulp VERSION 0.81.0 LANGUAGES CXX)\n"
        )
        result = self._run("CMakeLists.txt")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_accepts_whitespace_before_project_paren(self) -> None:
        Path("CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project (Pulp VERSION 0.81.0 LANGUAGES CXX)\n"
        )
        result = self._run("CMakeLists.txt")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_fails_closed_on_invalid_git_diff_base(self) -> None:
        """pulp #1761 followup (Codex P1) — git diff failures must
        block the push, not silently succeed. Original behaviour
        returned an empty list when --base couldn't be resolved,
        defeating the hard-block contract."""
        # Use --mode=push with a base that doesn't exist in this temp
        # dir (no git repo here at all → diff fails).
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--mode=push",
             "--base", "origin/definitely-not-a-real-ref"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("git diff", result.stderr)

    def test_blocks_arbitrary_non_pulp_cmakelists(self) -> None:
        """Even a non-Clock CMakeLists.txt is blocked if it doesn't say
        project(Pulp — catches any example or fixture pasted at the root.
        This is the parallel agent's positive-assertion proposal: protects
        against future `examples/foo/CMakeLists.txt` accidents that the
        Clock-specific signature wouldn't catch."""
        Path("CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(SomeOtherExample VERSION 2.0.0 LANGUAGES CXX)\n"
        )
        result = self._run("CMakeLists.txt")
        self.assertEqual(result.returncode, 1)
        self.assertIn("first 10 lines do not contain", result.stderr)
        self.assertIn("project(Pulp", result.stderr)

    def test_empty_cmakelists_does_not_trigger_positive_assertion(self) -> None:
        """Empty file (e.g. `git rm` with no replacement) shouldn't trigger
        the positive `project(Pulp` assertion — that's a different concern
        (file deletion, not corruption). Other gates handle removed files."""
        Path("CMakeLists.txt").write_text("")
        result = self._run("CMakeLists.txt")
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_warns_on_temp_fixture_path_hint(self) -> None:
        """A file path containing /private/var/folders/ → warning, not block."""
        # The check looks at the path string, not the file content.
        # We don't need the file to exist for path-pattern check.
        result = self._run(
            "tools/something/private/var/folders/foo/leftover.txt"
        )
        # Path doesn't trigger an error (only pulp.toml + Clock CMakeLists do).
        # But the warning IS emitted to stderr.
        self.assertEqual(result.returncode, 0)
        self.assertIn("path contains", result.stderr)
        self.assertIn("/private/var/folders/", result.stderr)

    def test_blocks_both_files_in_one_invocation(self) -> None:
        """When both bad files are present, both errors are reported."""
        Path("CMakeLists.txt").write_text(
            "project(Clock VERSION 1.0.0 LANGUAGES CXX)\n"
        )
        Path("pulp.toml").write_text("[pulp]\nsdk_version = \"0.1.0\"\n")
        result = self._run("CMakeLists.txt", "pulp.toml")
        self.assertEqual(result.returncode, 1)
        self.assertIn("CMakeLists.txt", result.stderr)
        self.assertIn("pulp.toml", result.stderr)


class RootAllowlistTests(unittest.TestCase):
    """Companion-track U-1 — tests for --mode=root-allowlist."""

    def setUp(self) -> None:
        self.tmpdir = tempfile.mkdtemp(prefix="pulp-pollution-root-test-")
        self.cwd = os.getcwd()
        os.chdir(self.tmpdir)
        # Initialize a real git repo so `git ls-tree HEAD` resolves.
        subprocess.run(["git", "init", "-q", "-b", "main"], check=True)
        subprocess.run(["git", "config", "user.email", "test@example.com"], check=True)
        subprocess.run(["git", "config", "user.name", "Test"], check=True)

    def tearDown(self) -> None:
        os.chdir(self.cwd)
        import shutil
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _commit_allowlisted_only(self) -> None:
        # Create a minimal allowlisted root: just CMakeLists.txt + README.md.
        Path("CMakeLists.txt").write_text(
            "project(Pulp VERSION 0.1.0 LANGUAGES CXX)\n"
        )
        Path("README.md").write_text("# Pulp\n")
        subprocess.run(["git", "add", "."], check=True)
        subprocess.run(["git", "commit", "-q", "-m", "init"], check=True)

    def _run_root_allowlist(self) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--mode=root-allowlist",
             "--rev", "HEAD"],
            capture_output=True, text=True, check=False,
        )

    def test_allowlisted_root_passes(self) -> None:
        self._commit_allowlisted_only()
        result = self._run_root_allowlist()
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_stray_top_level_file_blocks(self) -> None:
        self._commit_allowlisted_only()
        Path("screenshot.png").write_bytes(b"\x89PNG")
        subprocess.run(["git", "add", "screenshot.png"], check=True)
        subprocess.run(["git", "commit", "-q", "-m", "add stray"], check=True)
        result = self._run_root_allowlist()
        self.assertEqual(result.returncode, 1)
        self.assertIn("screenshot.png", result.stderr)
        self.assertIn("unexpected top-level path", result.stderr)
        self.assertIn("ALLOWED_ROOT_PATHS", result.stderr)

    def test_stray_top_level_dir_blocks(self) -> None:
        self._commit_allowlisted_only()
        Path("misplaced_module").mkdir()
        Path("misplaced_module/main.cpp").write_text("int main() {}\n")
        subprocess.run(["git", "add", "misplaced_module"], check=True)
        subprocess.run(["git", "commit", "-q", "-m", "add dir"], check=True)
        result = self._run_root_allowlist()
        self.assertEqual(result.returncode, 1)
        self.assertIn("misplaced_module", result.stderr)

    def test_handles_missing_rev_with_block(self) -> None:
        """git ls-tree failure → hard block (consistent with #1761 posture)."""
        # Fresh git repo with no commits → HEAD doesn't resolve.
        result = self._run_root_allowlist()
        self.assertEqual(result.returncode, 1)
        self.assertIn("git ls-tree", result.stderr)

    def test_skips_blank_lines_in_ls_tree_output(self) -> None:
        """Empty entries in `git ls-tree` output should be ignored without
        crashing. Exercises the `if not name: continue` branch directly —
        git in practice doesn't emit blanks, but the script defends against
        it because `splitlines()` on edge inputs can.
        """
        # Import the function and stub subprocess.run to return blank lines
        # interleaved with one allowlisted entry. Confirms (a) blanks are
        # skipped without error, and (b) the allowlisted entry still passes.
        stpc = load_module()

        # Use a class with the subprocess.CompletedProcess shape.
        class FakeResult:
            returncode = 0
            stdout = "\n\nREADME.md\n\n   \nCMakeLists.txt\n"
            stderr = ""

        orig_run = stpc.subprocess.run
        stpc.subprocess.run = lambda *a, **kw: FakeResult()
        try:
            errors = stpc.check_root_allowlist("HEAD")
        finally:
            stpc.subprocess.run = orig_run
        # README.md and CMakeLists.txt are allowlisted, blanks are skipped.
        self.assertEqual(errors, [])


class HelperCoverageTests(unittest.TestCase):
    def test_git_diff_files_stage_filters_blank_lines(self) -> None:
        stpc = load_module()
        completed = subprocess.CompletedProcess(
            ["git"],
            0,
            stdout="\nCMakeLists.txt\n docs/guide.md \n\n",
            stderr="",
        )

        with mock.patch.object(stpc.subprocess, "run", return_value=completed) as run:
            files = stpc._git_diff_files(None, "stage")

        self.assertEqual(files, ["CMakeLists.txt", "docs/guide.md"])
        run.assert_called_once_with(
            ["git", "diff", "--cached", "--name-only"],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_git_diff_files_push_requires_range(self) -> None:
        stpc = load_module()

        with self.assertRaisesRegex(stpc._DiffFailure, "push mode requires"):
            stpc._git_diff_files("", "push")

    def test_git_diff_files_push_raises_on_git_error(self) -> None:
        stpc = load_module()
        completed = subprocess.CompletedProcess(
            ["git"],
            128,
            stdout="",
            stderr="fatal: bad revision 'missing...HEAD'",
        )

        with mock.patch.object(stpc.subprocess, "run", return_value=completed):
            with self.assertRaisesRegex(stpc._DiffFailure, "bad revision"):
                stpc._git_diff_files("missing...HEAD", "push")

    def test_git_diff_files_ignores_unknown_internal_mode(self) -> None:
        stpc = load_module()

        self.assertEqual(stpc._git_diff_files(None, "files"), [])

    def test_read_blob_uses_git_show_for_revision(self) -> None:
        stpc = load_module()
        completed = subprocess.CompletedProcess(
            ["git"],
            0,
            stdout="project(Pulp VERSION 0.1.0)\n",
            stderr="",
        )

        with mock.patch.object(stpc.subprocess, "run", return_value=completed) as run:
            content = stpc._read_blob("HEAD", "CMakeLists.txt")

        self.assertEqual(content, "project(Pulp VERSION 0.1.0)\n")
        run.assert_called_once_with(
            ["git", "show", "HEAD:CMakeLists.txt"],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_read_blob_returns_empty_on_git_show_failure(self) -> None:
        stpc = load_module()
        completed = subprocess.CompletedProcess(["git"], 128, stdout="", stderr="fatal")

        with mock.patch.object(stpc.subprocess, "run", return_value=completed):
            self.assertEqual(stpc._read_blob("HEAD", "missing.txt"), "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
