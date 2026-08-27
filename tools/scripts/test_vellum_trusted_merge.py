#!/usr/bin/env python3
"""Focused positive and negative tests for the trusted Vellum merge builder."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("vellum_trusted_merge.py")
SPEC = importlib.util.spec_from_file_location("vellum_trusted_merge", SCRIPT)
assert SPEC and SPEC.loader
MERGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MERGE)


def git(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    ).stdout.strip()


class VellumTrustedMergeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name)
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.name", "Test")
        git(self.repo, "config", "user.email", "test@example.invalid")
        (self.repo / "shared.txt").write_text("base\n", encoding="utf-8")
        git(self.repo, "add", "shared.txt")
        git(self.repo, "commit", "-qm", "base")
        self.base = git(self.repo, "rev-parse", "HEAD")
        self.base_branch = git(self.repo, "branch", "--show-current")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def commit(self, name: str, content: str, message: str) -> str:
        (self.repo / name).write_text(content, encoding="utf-8")
        git(self.repo, "add", name)
        git(self.repo, "commit", "-qm", message)
        return git(self.repo, "rev-parse", "HEAD")

    def test_clean_merge_has_exact_parents_and_combined_tree(self) -> None:
        git(self.repo, "switch", "-qc", "feature", self.base)
        head = self.commit("head.txt", "from head\n", "head")
        git(self.repo, "switch", "-q", self.base_branch)
        base = self.commit("base.txt", "from base\n", "new base")

        candidate = MERGE.build_trusted_merge(self.repo, base, head)

        self.assertEqual(
            git(self.repo, "rev-list", "--parents", "-n", "1", candidate).split(),
            [candidate, base, head],
        )
        self.assertEqual(git(self.repo, "show", f"{candidate}:base.txt"), "from base")
        self.assertEqual(git(self.repo, "show", f"{candidate}:head.txt"), "from head")

    def test_conflict_fails_closed_without_a_candidate_commit(self) -> None:
        git(self.repo, "switch", "-qc", "feature", self.base)
        head = self.commit("shared.txt", "from head\n", "head conflict")
        git(self.repo, "switch", "-q", self.base_branch)
        base = self.commit("shared.txt", "from base tip\n", "base conflict")

        with self.assertRaisesRegex(MERGE.TrustedMergeError, "do not merge cleanly"):
            MERGE.build_trusted_merge(self.repo, base, head)

    def test_non_exact_sha_is_rejected(self) -> None:
        with self.assertRaisesRegex(MERGE.TrustedMergeError, "full lowercase"):
            MERGE.build_trusted_merge(self.repo, self.base[:12], self.base)


if __name__ == "__main__":
    unittest.main(verbosity=2)
