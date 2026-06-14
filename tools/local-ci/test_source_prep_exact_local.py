#!/usr/bin/env python3
"""No-network tests for local exact-SHA source worktree primitives."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("source_prep_exact_local.py", add_module_dir=True)


class SourcePrepExactLocalTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_local_worktree_match_and_reset_are_dependency_injected(self) -> None:
        worktree = self.root / "worktree"
        worktree.mkdir()
        self.assertFalse(
            self.mod.local_worktree_matches(
                worktree,
                "abc123",
                run_fn=lambda *_args, **_kwargs: subprocess.CompletedProcess([], 0, stdout="abc123\n", stderr=""),
            )
        )

        (worktree / ".git").write_text("gitdir: elsewhere\n")
        calls: list[tuple[list[str], dict]] = []

        def fake_run(command, **kwargs):
            calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, 0, stdout="abc123\n", stderr="")

        self.assertTrue(self.mod.local_worktree_matches(worktree, "abc123", run_fn=fake_run))

        removed: list[tuple[pathlib.Path, bool]] = []
        self.mod.reset_local_worktree(
            worktree,
            root=self.repo,
            run_fn=fake_run,
            rmtree_fn=lambda path, ignore_errors=False: removed.append((path, ignore_errors)),
        )

        self.assertEqual(calls[-2][0][:3], ["git", "worktree", "remove"])
        self.assertEqual(calls[-2][1]["cwd"], self.repo)
        self.assertEqual(removed, [(worktree, True)])
        self.assertEqual(calls[-1][0], ["git", "worktree", "prune"])


if __name__ == "__main__":
    unittest.main()
