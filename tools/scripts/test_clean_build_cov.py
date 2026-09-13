#!/usr/bin/env python3
"""Fixture tests for tools/scripts/clean_build_cov.sh.

Drives a throwaway COPY of the script so every root it derives stays inside the
fixture and no real worktree is touched. Verifies:

  1. Dry-run lists coverage dirs and deletes nothing (exit 0).
  2. --yes removes exact or hyphen-suffixed coverage builds and ONLY those. A
     sibling `build/` and a source dir are left intact.
  3. Idle gating: a coverage dir whose absolute path appears in a live process's
     command line (a stand-in for an in-flight coverage build) is skipped.
  4. An unreadable process list removes nothing and exits 3.
  5. The repo's own coverage dir and its in-repo agent worktrees are scanned
     too, not just PULP_WORKTREES_ROOT.
  6. Overlapping roots report a dir once, not once per root that saw it.
  7. An unknown argument exits 2; --help exits 0.

Run:
    python3 tools/scripts/test_clean_build_cov.py
"""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile
import time
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "clean_build_cov.sh"


def sandbox(td: str) -> tuple[pathlib.Path, pathlib.Path, pathlib.Path]:
    """Build a fixture repo + worktrees root and return (root, repo, script).

    The script scans its own repo and that repo's .claude/worktrees/ alongside
    PULP_WORKTREES_ROOT, deriving both from its own location. Driving the real
    checkout's copy under --yes would delete the developer's actual coverage
    dirs, so tests run a copy whose derived roots are fixtures. The repo is a
    SIBLING of the worktrees root, never under it, so a dir found through the
    repo root cannot also have been reachable through PULP_WORKTREES_ROOT.
    """
    base = pathlib.Path(td)
    root = base / "worktrees"
    repo = base / "repo"
    dest = repo / "tools" / "scripts"
    dest.mkdir(parents=True)
    root.mkdir()
    script = dest / SCRIPT.name
    shutil.copy2(SCRIPT, script)
    return root, repo, script


def run(
    root: pathlib.Path,
    script: pathlib.Path,
    *args: str,
    env_extra: dict[str, str] | None = None,
) -> subprocess.CompletedProcess:
    env = dict(os.environ, PULP_WORKTREES_ROOT=str(root))
    if env_extra:
        env.update(env_extra)
    return subprocess.run(
        ["bash", str(script), *args],
        capture_output=True, text=True, env=env,
    )


def make_layout(root: pathlib.Path) -> None:
    (root / "wt-a" / "build-cov" / "obj").mkdir(parents=True)
    (root / "wt-a" / "build-cov" / "obj" / "f.o").write_text("x")
    (root / "wt-b" / "build-coverage").mkdir(parents=True)
    (root / "wt-d" / "build-cov-phase6-gpu").mkdir(parents=True)
    (root / "wt-e" / "build-covariance-data").mkdir(parents=True)
    (root / "wt-c" / "build").mkdir(parents=True)        # primary build, keep
    (root / "wt-c" / "src").mkdir(parents=True)          # source, keep
    (root / "wt-c" / "src" / "a.cpp").write_text("int main(){}")


class CleanBuildCovTests(unittest.TestCase):
    def test_script_exists_and_executable(self) -> None:
        self.assertTrue(SCRIPT.exists(), f"missing {SCRIPT}")
        self.assertTrue(os.access(SCRIPT, os.X_OK), "script not executable")

    def test_dry_run_lists_but_deletes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, _repo, script = sandbox(td)
            make_layout(root)
            res = run(root, script)
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertIn("would remove", res.stdout)
            self.assertIn("re-run with --yes", res.stdout)
            # Nothing deleted.
            self.assertTrue((root / "wt-a" / "build-cov").is_dir())
            self.assertTrue((root / "wt-b" / "build-coverage").is_dir())
            self.assertTrue((root / "wt-d" / "build-cov-phase6-gpu").is_dir())
            self.assertTrue((root / "wt-e" / "build-covariance-data").is_dir())

    def test_apply_removes_only_coverage_dirs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, _repo, script = sandbox(td)
            make_layout(root)
            res = run(root, script, "--yes")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertFalse((root / "wt-a" / "build-cov").exists())
            self.assertFalse((root / "wt-b" / "build-coverage").exists())
            self.assertFalse((root / "wt-d" / "build-cov-phase6-gpu").exists())
            self.assertTrue((root / "wt-e" / "build-covariance-data").is_dir())
            # Primary build/ and source tree untouched.
            self.assertTrue((root / "wt-c" / "build").is_dir())
            self.assertTrue((root / "wt-c" / "src" / "a.cpp").exists())

    def test_idle_gating_skips_active_dir(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, _repo, script = sandbox(td)
            make_layout(root)
            active = root / "wt-a" / "build-cov"
            # A long-lived process whose complete command line carries the
            # coverage dir path stands in for an in-flight `cmake --build
            # <wt>/build-cov`. A padded-argv Python sleeper keeps it alive.
            proc = subprocess.Popen(
                ["python3", "-c", "import time; time.sleep(30)", "cmake", str(active)]
            )
            try:
                time.sleep(0.3)
                res = run(root, script, "--yes")
                self.assertEqual(res.returncode, 0, res.stderr)
                self.assertIn("SKIP (active build)", res.stdout)
                self.assertTrue(active.is_dir(), "active coverage dir was deleted")
                # The non-active one is still removed.
                self.assertFalse((root / "wt-b" / "build-coverage").exists())
            finally:
                proc.terminate()
                proc.wait()

    def test_unreadable_process_list_removes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, _repo, script = sandbox(td)
            make_layout(root)
            shim = root / "shim"
            shim.mkdir()
            fake_ps = shim / "ps"
            fake_ps.write_text("#!/bin/sh\necho partial-output\nexit 1\n")
            fake_ps.chmod(0o755)

            res = run(
                root,
                script,
                "--yes",
                env_extra={"PATH": f"{shim}:{os.environ['PATH']}"},
            )

            self.assertEqual(res.returncode, 3, res.stdout + res.stderr)
            self.assertIn("could not read complete process command lines", res.stderr)
            self.assertTrue((root / "wt-a" / "build-cov").is_dir())
            self.assertTrue((root / "wt-b" / "build-coverage").is_dir())

    def test_scans_repo_root_and_in_repo_worktrees(self) -> None:
        """Coverage dirs outside PULP_WORKTREES_ROOT are still found.

        A host that points PULP_WORKTREES_ROOT at a dedicated volume puts the
        repo's own build-cov and its .claude/worktrees/ agent checkouts outside
        that root entirely. Scanning one root there reports a clean pass while
        those sit invisible, which is how a disk filled behind a green reclaim.
        """
        with tempfile.TemporaryDirectory() as td:
            root, repo, script = sandbox(td)
            make_layout(root)
            own = repo / "build-cov"
            own.mkdir()
            nested = repo / ".claude" / "worktrees" / "agent-x" / "build-cov"
            nested.mkdir(parents=True)

            res = run(root, script)
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertIn(str(own), res.stdout)
            self.assertIn(str(nested), res.stdout)
            # The sibling-worktree root is still covered.
            self.assertIn(str(root / "wt-a" / "build-cov"), res.stdout)
            # Every root it scanned is named, so a reader can see the coverage.
            self.assertIn(str(repo / ".claude" / "worktrees"), res.stdout)

    def test_overlapping_roots_report_each_dir_once(self) -> None:
        """The roots may nest, so the same dir can be seen more than once.

        Pointing PULP_WORKTREES_ROOT at the repo's parent makes the repo's own
        coverage dir reachable both ways. Counting it twice would double the
        reported reclaimable total and delete-log it twice.
        """
        with tempfile.TemporaryDirectory() as td:
            _root, repo, script = sandbox(td)
            own = repo / "build-cov"
            own.mkdir()
            # Point the worktrees root at the repo's parent, so it contains repo.
            res = run(repo.parent, script)
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(res.stdout.count("would remove"), 1, res.stdout)
            self.assertIn("1 coverage dir(s)", res.stdout)
            self.assertIn(str(own), res.stdout)

    def test_unknown_arg_exits_2(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, _repo, script = sandbox(td)
            res = run(root, script, "--bogus")
            self.assertEqual(res.returncode, 2)

    def test_help_exits_0(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, _repo, script = sandbox(td)
            res = run(root, script, "--help")
            self.assertEqual(res.returncode, 0)
            self.assertIn("clean_build_cov", res.stdout)


if __name__ == "__main__":
    unittest.main()
