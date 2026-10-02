#!/usr/bin/env python3
"""git_read_memo serves only immutable Git reads, and only successful ones."""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import git_read_memo as memo  # noqa: E402

SHA = "a" * 40


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=repo, check=True, text=True,
                          capture_output=True).stdout.strip()


class CacheableTests(unittest.TestCase):
    def test_reads_named_by_full_shas_are_cacheable(self) -> None:
        for argv in (["git", "ls-tree", SHA, "--", "a.py"],
                     ["git", "show", f"{SHA}:tools/x.py"],
                     ["git", "diff-tree", "-m", "--first-parent", "-r", SHA, "--", "p"],
                     ["git", "rev-list", "--first-parent", f"{SHA}..{'b' * 40}", "--", "p"],
                     ["git", "-C", "/repo", "merge-base", "--is-ancestor", SHA, "b" * 40]):
            with self.subTest(argv=argv):
                self.assertTrue(memo.cacheable(argv))

    def test_movable_names_and_working_tree_reads_are_not(self) -> None:
        for argv in (["git", "ls-tree", "HEAD", "--", "a.py"],
                     ["git", "merge-base", "--is-ancestor", SHA, "HEAD"],
                     ["git", "rev-list", "main..topic"],
                     ["git", "show", "abc1234:x"],
                     ["git", "hash-object", "--stdin-paths"],
                     ["git", "grep", "-l", SHA],
                     ["git", "status", "--porcelain"],
                     ["git", "rev-parse", "HEAD"],
                     ["git", "log"],
                     ["python3", "ls-tree", SHA],
                     "git ls-tree " + SHA):
            with self.subTest(argv=argv):
                self.assertFalse(memo.cacheable(argv))


class MemoTests(unittest.TestCase):
    def setUp(self) -> None:
        self.holder = tempfile.TemporaryDirectory(prefix="git-read-memo-")
        self.repo = Path(self.holder.name)
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.name", "t")
        git(self.repo, "config", "user.email", "t@example.invalid")
        git(self.repo, "config", "commit.gpgsign", "false")
        (self.repo / "a.txt").write_text("one\n", encoding="utf-8")
        git(self.repo, "add", "a.txt")
        git(self.repo, "commit", "-q", "-m", "one")
        self.first = git(self.repo, "rev-parse", "HEAD")

    def tearDown(self) -> None:
        self.holder.cleanup()

    def ls_tree(self, rev: str) -> subprocess.CompletedProcess:
        return subprocess.run(["git", "ls-tree", rev, "--", "a.txt"], cwd=self.repo,
                              check=False, capture_output=True, text=True)

    def test_a_repeated_sha_read_is_answered_once_and_identically(self) -> None:
        with memo.memoized_git_reads() as stats:
            first = self.ls_tree(self.first)
            second = self.ls_tree(self.first)
        self.assertEqual((stats.misses, stats.hits), (1, 1))
        self.assertEqual(first.stdout, second.stdout)
        self.assertIn("blob", second.stdout)

    def test_head_is_never_served_from_memory(self) -> None:
        with memo.memoized_git_reads() as stats:
            before = self.ls_tree("HEAD").stdout
            (self.repo / "a.txt").write_text("two\n", encoding="utf-8")
            git(self.repo, "commit", "-q", "-am", "two")
            after = self.ls_tree("HEAD").stdout
        self.assertNotEqual(before, after)
        self.assertEqual(stats.hits, 0)

    def test_a_failed_read_is_run_again_every_time(self) -> None:
        with memo.memoized_git_reads() as stats:
            codes = [self.ls_tree("f" * 40).returncode for _ in range(3)]
        self.assertTrue(all(code != 0 for code in codes))
        self.assertEqual((stats.misses, stats.hits), (3, 0))

    def test_two_repositories_never_share_an_answer(self) -> None:
        with tempfile.TemporaryDirectory(prefix="git-read-memo-other-") as other:
            other_repo = Path(other)
            subprocess.run(["git", "clone", "-q", str(self.repo), str(other_repo)], check=True)
            with memo.memoized_git_reads() as stats:
                self.ls_tree(self.first)
                subprocess.run(["git", "ls-tree", self.first, "--", "a.txt"], cwd=other_repo,
                               check=True, capture_output=True, text=True)
        self.assertEqual((stats.misses, stats.hits), (2, 0))

    def test_the_patch_is_removed_on_exit(self) -> None:
        original = subprocess.run
        with memo.memoized_git_reads():
            self.assertIsNot(subprocess.run, original)
        self.assertIs(subprocess.run, original)


if __name__ == "__main__":
    unittest.main()
