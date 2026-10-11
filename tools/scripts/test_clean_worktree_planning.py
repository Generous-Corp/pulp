#!/usr/bin/env python3
"""Negative-control tests for clean_worktree_planning.py."""
from __future__ import annotations
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/scripts/clean_worktree_planning.py"

class PlanningReaperTests(unittest.TestCase):
    def setUp(self) -> None:
        self.td = tempfile.TemporaryDirectory()
        self.base = Path(self.td.name)
        self.repo = self.base / "repo"
        (self.repo / "tools/scripts").mkdir(parents=True)
        shutil.copy2(SCRIPT, self.repo / "tools/scripts/clean_worktree_planning.py")
        subprocess.run(["git", "init", "-q", "-b", "main", str(self.repo)], check=True)
        subprocess.run(["git", "-C", str(self.repo), "config", "user.email", "test@example.com"], check=True)
        subprocess.run(["git", "-C", str(self.repo), "config", "user.name", "Test"], check=True)
        (self.repo / "README").write_text("base\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(self.repo), "add", "README"], check=True)
        subprocess.run(["git", "-C", str(self.repo), "commit", "-qm", "base"], check=True)
        subprocess.run(["git", "-C", str(self.repo), "update-ref", "refs/remotes/origin/main", "HEAD"], check=True)
        self.wt_clean = self.base / "clean"
        self.wt_dirty = self.base / "dirty"
        self.wt_active = self.base / "active"
        for wt, branch in ((self.wt_clean, "clean"), (self.wt_dirty, "dirty"), (self.wt_active, "active")):
            subprocess.run(["git", "-C", str(self.repo), "worktree", "add", "-q", "-b", branch, str(wt)], check=True)
        # Make a commit on each branch, then merge those commits into main. This
        # proves the reaper requires actual ancestry, not just an old directory.
        for wt, name in ((self.wt_clean, "clean"), (self.wt_dirty, "dirty"), (self.wt_active, "active")):
            (wt / f"{name}.txt").write_text(name, encoding="utf-8")
            subprocess.run(["git", "-C", str(wt), "add", "."], check=True)
            subprocess.run(["git", "-C", str(wt), "commit", "-qm", name], check=True)
            subprocess.run(["git", "-C", str(self.repo), "merge", "--no-ff", "-qm", f"merge {name}", name], check=True)
        subprocess.run(["git", "-C", str(self.repo), "update-ref", "refs/remotes/origin/main", "HEAD"], check=True)
        self.targets = {}
        common = Path(subprocess.check_output(["git", "-C", str(self.repo), "rev-parse", "--git-common-dir"], text=True, encoding="utf-8").strip()).resolve()
        for wt in (self.wt_clean, self.wt_dirty, self.wt_active):
            target = Path(subprocess.check_output(["git", "-C", str(wt), "rev-parse", "--git-path", "modules/planning"], text=True, encoding="utf-8").strip()).resolve()
            target.mkdir(parents=True)
            (target / "objects").write_text("planning", encoding="utf-8")
            old = time.time() - 4 * 3600
            os.utime(target / "objects", (old, old)); os.utime(target, (old, old))
            self.targets[wt] = target
        # Dirty means source edits in the registered worktree, regardless of
        # the fact that its planning clone is old and otherwise eligible.
        (self.wt_dirty / "uncommitted.txt").write_text("do not reap", encoding="utf-8")

    def tearDown(self) -> None:
        self.td.cleanup()

    def run_reaper(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["python3", str(self.repo / "tools/scripts/clean_worktree_planning.py"), *args],
                              cwd=self.repo, text=True, encoding="utf-8", capture_output=True, check=False)

    def test_dry_run_selects_only_clean_idle_merged_clone(self) -> None:
        result = self.run_reaper("--verbose")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("would remove", result.stdout)
        self.assertIn(str(self.targets[self.wt_clean]), result.stdout)
        self.assertIn("dirty worktree", result.stdout)
        self.assertTrue(self.targets[self.wt_dirty].is_dir())

    def test_non_idle_clone_is_never_selected(self) -> None:
        target = self.targets[self.wt_clean]
        (target / "objects").touch()
        result = self.run_reaper("--verbose")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("planning clone is not idle", result.stdout)
        self.assertNotIn(str(target), result.stdout)
        self.assertTrue(target.is_dir())

    def test_active_clone_is_never_selected(self) -> None:
        target = self.targets[self.wt_active]
        proc = subprocess.Popen(["python3", "-c", "import time; time.sleep(30)", str(target)])
        try:
            time.sleep(0.2)
            result = self.run_reaper("--verbose")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("active worktree or planning clone", result.stdout)
            self.assertTrue(target.is_dir())
        finally:
            proc.terminate(); proc.wait()

    def test_symlink_clone_is_never_selected(self) -> None:
        target = self.targets[self.wt_clean]
        source = self.repo / "source-planning"
        source.mkdir()
        (source / "keep.txt").write_text("source", encoding="utf-8")
        shutil.rmtree(target)
        target.symlink_to(source, target_is_directory=True)
        result = self.run_reaper("--yes", "--verbose")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(source.joinpath("keep.txt").exists())
        self.assertTrue(target.is_symlink())

    def test_source_tree_is_not_a_candidate(self) -> None:
        source = self.repo / "source" / "modules" / "planning"
        source.mkdir(parents=True)
        (source / "important.txt").write_text("source", encoding="utf-8")
        result = self.run_reaper("--yes")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((source / "important.txt").exists())

if __name__ == "__main__":
    unittest.main(verbosity=2)
