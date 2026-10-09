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

    def test_each_runner_form_collects_its_directory(self) -> None:
        repo = self.repo()
        for path in ("tools/p/test_a.py", "tools/q/sub/test_b.py", "tools/r/test_c.py",
                     "tools/r/test_d.py", "tools/s/test_e.py"):
            repo.write(path, "")
        repo.write(".github/workflows/runners.yml", "\n".join([
            "jobs:",
            "  t:",
            "    steps:",
            "      - run: .venv/bin/pytest tools/p/ -v --tb=short",
            "      - run: |",
            "          python -m pytest -q \\",
            "            tools/q",
            "      - run: python3 -m unittest discover -s tools/s -p 'test_*.py'",
        ]) + "\n")
        repo.write("test/cmake/discover.cmake", "\n".join([
            "add_test(NAME r COMMAND py -m unittest discover",
            '    -s "${CMAKE_SOURCE_DIR}/tools/r" -p test_c.py)',
        ]) + "\n")
        missing, covered = guard.uncovered(repo.root)
        self.assertEqual((missing, covered), (["tools/r/test_d.py"], 8))

    def test_a_directory_mentioned_without_a_runner_does_not_count(self) -> None:
        repo = self.repo()
        repo.write("tools/scripts/test_quiet.py", "")
        repo.write("test/cmake/cwd.cmake", "\n".join([
            "add_test(NAME q COMMAND py ${S}/tools/x/test_cmake.py)",
            "set_tests_properties(q PROPERTIES",
            '    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools/scripts")',
        ]) + "\n")
        repo.write(".github/workflows/mentions.yml", "\n".join([
            "jobs:",
            "  t:",
            "    steps:",
            "      - uses: actions/checkout@v4",
            "        with:",
            "          sparse-checkout: tools/scripts",
            "      - run: npm ci --prefix tools/scripts --no-audit",
            "      - run: python3 -m pytest tools/x/test_cmake.py; prefix=tools/scripts",
            "      - run: python3 -c 'import sys; sys.path.insert(0, \"tools/scripts\")'",
            "      - run: python3 tools/scripts/shell_portability_check.py tools/ci tools/scripts",
            "      - run: python3 -m pytest build/mytools/scripts",
            "      - run: python3 -m unittest discover -s $ROOT/mytools/ci",
        ]) + "\n")
        repo.write("tools/ci/test_quiet.py", "")
        missing = guard.uncovered(repo.root)[0]
        self.assertIn("tools/scripts/test_quiet.py", missing)
        self.assertIn("tools/ci/test_quiet.py", missing)

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

    def test_notes_cover_only_baselined_files_and_survive_a_rewrite(self) -> None:
        repo = self.repo()
        repo.write("tools/x/test_orphan.py", "")
        repo.write(guard.BASELINE, json.dumps({"reason": "r", "unregistered": ["tools/x/test_orphan.py"],
                                               "notes": {"tools/x/test_orphan.py": "needs a device",
                                                         "tools/x/test_entry.py": "stale"}}))
        self.assertEqual(guard.check(repo.root, min_covered=1), [
            f"tools/x/test_entry.py: has a note but is not baselined; remove the note from {guard.BASELINE}"])
        self.assertEqual(guard.main(["--repo-root", str(repo.root), "--write-baseline"]), 0)
        rewritten = json.loads((repo.root / guard.BASELINE).read_text(encoding="utf-8"))
        self.assertEqual(rewritten["notes"], {"tools/x/test_orphan.py": "needs a device"})
        self.assertEqual(guard.check(repo.root, min_covered=1), [])

    def test_a_blind_instrument_fails_rather_than_passes(self) -> None:
        repo = self.repo()
        problems = guard.check(repo.root, min_covered=5)
        self.assertTrue(any("instrument blind" in problem for problem in problems), problems)

    def test_the_real_tree_is_clean(self) -> None:
        self.assertEqual(guard.check(guard.REPO_ROOT), [])


if __name__ == "__main__":
    unittest.main()
