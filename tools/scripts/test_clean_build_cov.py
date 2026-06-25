#!/usr/bin/env python3
"""Fixture tests for tools/scripts/clean_build_cov.sh.

Drives the script over a throwaway PULP_WORKTREES_ROOT so no real worktree is
touched. Verifies:

  1. Dry-run lists coverage dirs and deletes nothing (exit 0).
  2. --yes removes `build-cov` / `build-coverage` and ONLY those — a sibling
     `build/` and a source dir are left intact.
  3. Idle gating: a coverage dir whose absolute path appears in a live process's
     command line (a stand-in for an in-flight coverage build) is skipped.
  4. An unknown argument exits 2; --help exits 0.

Run:
    python3 tools/scripts/test_clean_build_cov.py
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import time
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "clean_build_cov.sh"


def run(root: pathlib.Path, *args: str) -> subprocess.CompletedProcess:
    env = dict(os.environ, PULP_WORKTREES_ROOT=str(root))
    return subprocess.run(
        ["bash", str(SCRIPT), *args],
        capture_output=True, text=True, env=env,
    )


def make_layout(root: pathlib.Path) -> None:
    (root / "wt-a" / "build-cov" / "obj").mkdir(parents=True)
    (root / "wt-a" / "build-cov" / "obj" / "f.o").write_text("x")
    (root / "wt-b" / "build-coverage").mkdir(parents=True)
    (root / "wt-c" / "build").mkdir(parents=True)        # primary build — keep
    (root / "wt-c" / "src").mkdir(parents=True)          # source — keep
    (root / "wt-c" / "src" / "a.cpp").write_text("int main(){}")


class CleanBuildCovTests(unittest.TestCase):
    def test_script_exists_and_executable(self) -> None:
        self.assertTrue(SCRIPT.exists(), f"missing {SCRIPT}")
        self.assertTrue(os.access(SCRIPT, os.X_OK), "script not executable")

    def test_dry_run_lists_but_deletes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            make_layout(root)
            res = run(root)
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertIn("would remove", res.stdout)
            self.assertIn("re-run with --yes", res.stdout)
            # Nothing deleted.
            self.assertTrue((root / "wt-a" / "build-cov").is_dir())
            self.assertTrue((root / "wt-b" / "build-coverage").is_dir())

    def test_apply_removes_only_coverage_dirs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            make_layout(root)
            res = run(root, "--yes")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertFalse((root / "wt-a" / "build-cov").exists())
            self.assertFalse((root / "wt-b" / "build-coverage").exists())
            # Primary build/ and source tree untouched.
            self.assertTrue((root / "wt-c" / "build").is_dir())
            self.assertTrue((root / "wt-c" / "src" / "a.cpp").exists())

    def test_idle_gating_skips_active_dir(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            make_layout(root)
            active = root / "wt-a" / "build-cov"
            # A long-lived process whose command line carries a build-tool name
            # (so the script's pgrep catches it) AND the coverage dir path (so it
            # matches that dir) stands in for an in-flight `cmake --build
            # <wt>/build-cov`. A padded-argv Python sleeper keeps it alive.
            proc = subprocess.Popen(
                ["python3", "-c", "import time; time.sleep(30)", "cmake", str(active)]
            )
            try:
                time.sleep(0.3)
                res = run(root, "--yes")
                self.assertEqual(res.returncode, 0, res.stderr)
                self.assertIn("SKIP (active build)", res.stdout)
                self.assertTrue(active.is_dir(), "active coverage dir was deleted")
                # The non-active one is still removed.
                self.assertFalse((root / "wt-b" / "build-coverage").exists())
            finally:
                proc.terminate()
                proc.wait()

    def test_unknown_arg_exits_2(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            res = run(pathlib.Path(td), "--bogus")
            self.assertEqual(res.returncode, 2)

    def test_help_exits_0(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            res = run(pathlib.Path(td), "--help")
            self.assertEqual(res.returncode, 0)
            self.assertIn("clean_build_cov", res.stdout)


if __name__ == "__main__":
    unittest.main()
