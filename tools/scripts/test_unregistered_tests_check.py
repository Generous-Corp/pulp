#!/usr/bin/env python3
"""Tests for unregistered_tests_check.py against scratch repositories."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import unregistered_tests_check as guard  # noqa: E402


class Repo:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.git("init", "-q")
        for path, text in {
            guard.SCRIPT_INPUTS: json.dumps({"tests": {"a": {"entry": "tools/x/test_entry.py"}}}),
            guard.SOURCE_SELFTESTS: json.dumps({"tests": [{"argv": ["{repo}/tools/x/test_selftest.py"]}]}),
            ".github/workflows/ci.yml": "jobs:\n  t:\n    steps:\n      - run: python3 -m pytest tools/suite\n",
            "test/cmake/x.cmake": "add_test(NAME w COMMAND py ${S}/tools/x/test_cmake.py)\n",
            guard.BASELINE: json.dumps({"reason": "r", "unregistered": []}),
        }.items():
            self.write(path, text)
        for name in ("test_entry", "test_selftest", "test_cmake"):
            self.write(f"tools/x/{name}.py", "")
        self.write("tools/suite/deep/test_in_dir.py", "")

    def git(self, *args: str) -> None:
        subprocess.run(["git", "-C", str(self.root), *args], check=True, capture_output=True)

    def write(self, path: str, text: str) -> None:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")
        self.git("add", path)


class UnregisteredTestsCheckTest(unittest.TestCase):
    def repo(self) -> Repo:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        return Repo(Path(directory.name))

    def test_each_kind_of_invocation_counts(self) -> None:
        repo = self.repo()
        self.assertEqual(guard.uncovered(repo.root), ([], 4))
        self.assertEqual(guard.check(repo.root, min_covered=4), [])

    def test_a_new_unregistered_test_fails(self) -> None:
        repo = self.repo()
        repo.write("tools/x/test_orphan.py", "")
        problems = guard.check(repo.root, min_covered=1)
        self.assertEqual(len(problems), 1)
        self.assertIn("tools/x/test_orphan.py: no ctest", problems[0])

    def test_a_mention_only_in_a_comment_does_not_count(self) -> None:
        repo = self.repo()
        repo.write("tools/y/test_commented.py", "")
        repo.write(".github/workflows/other.yml",
                   "# python3 tools/y/test_commented.py\njobs: {}  # tools/y/test_commented.py\n")
        self.assertIn("tools/y/test_commented.py", guard.uncovered(repo.root)[0])

    def test_the_baseline_only_shrinks(self) -> None:
        repo = self.repo()
        repo.write("tools/x/test_orphan.py", "")
        repo.write(guard.BASELINE, json.dumps({"reason": "r", "unregistered": [
            "tools/x/test_orphan.py", "tools/x/test_entry.py", "tools/x/test_gone.py"]}))
        problems = guard.check(repo.root, min_covered=1)
        self.assertEqual(sorted(problems), [
            f"tools/x/test_entry.py: is now invoked; remove it from {guard.BASELINE}",
            f"tools/x/test_gone.py: no longer exists; remove it from {guard.BASELINE}",
        ])

    def test_a_blind_instrument_fails_rather_than_passes(self) -> None:
        repo = self.repo()
        problems = guard.check(repo.root, min_covered=5)
        self.assertTrue(any("instrument blind" in problem for problem in problems), problems)

    def test_the_real_tree_is_clean(self) -> None:
        self.assertEqual(guard.check(guard.REPO_ROOT), [])


if __name__ == "__main__":
    unittest.main()
