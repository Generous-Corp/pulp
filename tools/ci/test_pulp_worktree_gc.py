#!/usr/bin/env python3
"""Contract tests for the safe `pulp-worktree.sh gc` transition.

These tests deliberately use a fake Git command and throwaway directories.
They prove that the public command has no destructive default and that merely
requesting apply mode cannot revive the legacy `[gone]` deletion heuristic.
The affirmative merged-worktree classifier is covered separately when it is
implemented; until then, apply mode must fail closed with zero candidates.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest


SCRIPT = Path(__file__).with_name("pulp-worktree.sh")
LOCAL_GATES = SCRIPT.parent.parent / "scripts" / "gates.sh"


class GcContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.git_log = self.root / "git.log"
        self.worktree_root = self.root / "worktrees"
        self.gone_worktree = self.worktree_root / "feature-gone"
        (self.gone_worktree / "build").mkdir(parents=True)
        (self.gone_worktree / "source.txt").write_text("preserve me\n")
        self.slash_worktree = self.worktree_root / "feature" / "slash-branch"
        slash_build = self.slash_worktree / "build"
        slash_build.mkdir(parents=True)
        (self.slash_worktree / "source.txt").write_text("preserve nested worktree\n")
        old = time.time() - (3 * 24 * 60 * 60)
        os.utime(slash_build, (old, old))

        fake_git = self.bin / "git"
        fake_git.write_text(
            "#!/usr/bin/env bash\n"
            "printf '%s\\n' \"$*\" >> \"$PULP_TEST_GIT_LOG\"\n"
            "case \" $* \" in\n"
            "  *' for-each-ref '*) printf 'feature/gone [gone]\\n' ;;\n"
            "  *' worktree list --porcelain -z '*)\n"
            "    printf 'worktree %s\\0HEAD %040d\\0branch refs/heads/feature/gone\\0\\0' \"$PULP_TEST_GONE_WORKTREE\" 1\n"
            "    printf 'worktree %s\\0HEAD %040d\\0branch refs/heads/feature/slash-branch\\0\\0' \"$PULP_TEST_SLASH_WORKTREE\" 2 ;;\n"
            "esac\n"
        )
        fake_git.chmod(0o755)

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def run_gc(self, *args: str) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ)
        env.update(
            PATH=f"{self.bin}:{env['PATH']}",
            PULP_WT_ROOT=str(self.worktree_root),
            PULP_TEST_GIT_LOG=str(self.git_log),
            PULP_TEST_GONE_WORKTREE=str(self.gone_worktree),
            PULP_TEST_SLASH_WORKTREE=str(self.slash_worktree),
        )
        return subprocess.run(
            ["bash", str(SCRIPT), "gc", *args],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )

    def git_calls(self) -> str:
        return self.git_log.read_text() if self.git_log.exists() else ""

    def assert_nothing_was_deleted(self) -> None:
        self.assertTrue(self.gone_worktree.is_dir())
        self.assertTrue((self.gone_worktree / "build").is_dir())
        self.assertEqual(
            (self.gone_worktree / "source.txt").read_text(), "preserve me\n"
        )
        self.assertTrue((self.slash_worktree / "build").is_dir())
        self.assertEqual(
            (self.slash_worktree / "source.txt").read_text(),
            "preserve nested worktree\n",
        )
        calls = self.git_calls()
        self.assertNotIn("worktree remove", calls)
        self.assertNotIn("branch -D", calls)
        self.assertNotIn("worktree prune", calls)

    def test_gc_is_dry_run_by_default(self) -> None:
        result = self.run_gc("--merged", "--max-age-days", "0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("dry-run", result.stderr)
        self.assert_nothing_was_deleted()

    def test_apply_is_explicit_but_incomplete_classifier_fails_closed(self) -> None:
        result = self.run_gc("--apply", "--merged", "--max-age-days", "0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("apply", result.stderr)
        self.assertIn("no deletion candidates", result.stderr)
        self.assert_nothing_was_deleted()

    def test_help_documents_dry_run_and_apply(self) -> None:
        result = self.run_gc("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("dry-run", result.stdout)
        self.assertIn("--apply", result.stdout)
        self.assert_nothing_was_deleted()

    def test_apply_without_a_value_is_a_flag(self) -> None:
        result = self.run_gc("--apply")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("apply", result.stderr)
        self.assert_nothing_was_deleted()

    def test_missing_option_values_fail(self) -> None:
        for option in ("--max-age-days", "--max-total-gb"):
            with self.subTest(option=option):
                result = self.run_gc(option)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"{option} requires a value", result.stderr)
                self.assert_nothing_was_deleted()

    def test_invalid_and_negative_option_values_fail(self) -> None:
        cases = (
            ("--max-age-days", "many"),
            ("--max-age-days", "1.5"),
            ("--max-age-days", "-1"),
            ("--max-total-gb", "many"),
            ("--max-total-gb", "-1"),
        )
        for option, value in cases:
            with self.subTest(option=option, value=value):
                result = self.run_gc(option, value)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(option, result.stderr)
                self.assert_nothing_was_deleted()

    def test_unknown_argument_fails(self) -> None:
        result = self.run_gc("--delete-everything")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown gc arg", result.stderr)
        self.assert_nothing_was_deleted()

    def test_max_total_is_a_validated_budget_signal_not_deletion_authority(self) -> None:
        result = self.run_gc(
            "--apply", "--merged", "--max-total-gb", "1.5"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("candidate budget requested: 1.5 GB", result.stderr)
        self.assertIn("no deletion candidates", result.stderr)
        self.assert_nothing_was_deleted()

    def test_stale_inventory_includes_registered_slash_branch_worktree(self) -> None:
        result = self.run_gc("--max-age-days", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(self.slash_worktree / "build"), result.stderr)
        self.assertIn("worktree list --porcelain -z", self.git_calls())
        self.assert_nothing_was_deleted()

    def test_local_gate_runs_gc_contract(self) -> None:
        self.assertIn(
            "tools/ci/test_pulp_worktree_gc.py",
            LOCAL_GATES.read_text(encoding="utf-8"),
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
