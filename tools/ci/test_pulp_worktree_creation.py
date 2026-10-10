#!/usr/bin/env python3
"""Tests for the pulp-worktree creation choke point."""
from __future__ import annotations
import os
import shutil
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/ci/pulp-worktree.sh"

class WorktreeCreationGuardTests(unittest.TestCase):
    def test_script_checks_location_before_git_worktree_add(self) -> None:
        text = SCRIPT.read_text(encoding="utf-8")
        guard = 'checkout_location_guard.py" "$wt"'
        self.assertIn(guard, text)
        self.assertLess(text.index(guard), text.index('git -C "$REPO_ROOT" worktree add'))

    def test_tmp_worktree_is_refused_before_creation(self) -> None:
        # Run the creation command against a throwaway Git repository. A guard
        # regression must never be able to create a branch or worktree in the
        # checkout that is running this test.
        with tempfile.TemporaryDirectory(dir="/tmp") as raw:
            root = Path(raw)
            scratch = root / "repo"
            (scratch / "tools/ci").mkdir(parents=True)
            shutil.copy2(SCRIPT, scratch / "tools/ci/pulp-worktree.sh")
            shutil.copy2(SCRIPT.parent / "checkout_location_guard.py",
                         scratch / "tools/ci/checkout_location_guard.py")
            (scratch / "README").write_text("fixture\n", encoding="utf-8")
            subprocess.run(["git", "init", "-q", "-b", "main", str(scratch)], check=True)
            subprocess.run(["git", "-C", str(scratch), "config", "user.email", "test@example.com"], check=True)
            subprocess.run(["git", "-C", str(scratch), "config", "user.name", "Test"], check=True)
            subprocess.run(["git", "-C", str(scratch), "add", "README"], check=True)
            subprocess.run(["git", "-C", str(scratch), "commit", "-qm", "fixture"], check=True)
            worktrees = root / "worktrees"
            # Keep CI exemptions and host configuration out of this probe. In
            # particular, GITHUB_ACTIONS=true intentionally bypasses the
            # production guard and would let this test create a real fixture
            # worktree instead of proving the refusal path.
            env = {
                "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                "HOME": str(root / "home"),
                "LC_ALL": "C",
                "PULP_WT_ROOT": str(worktrees),
                "TMPDIR": str(root),
                "GIT_CONFIG_NOSYSTEM": "1",
                "GIT_CONFIG_GLOBAL": os.devnull,
            }
            result = subprocess.run(
                ["bash", str(scratch / "tools/ci/pulp-worktree.sh"), "new", "w6-guard-test"],
                cwd=scratch, env=env, text=True, encoding="utf-8", capture_output=True, check=False,
            )
            self.assertEqual(result.returncode, 3, result.stderr)
            self.assertIn("temporary directory", result.stderr)
            self.assertFalse(worktrees.exists())

    def test_planning_is_skipped_by_default_and_referenceable(self) -> None:
        text = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('PULP_WORKTREE_INIT_PLANNING:-0', text)
        self.assertIn('--reference "$planning_reference"', text)

if __name__ == "__main__":
    unittest.main(verbosity=2)
