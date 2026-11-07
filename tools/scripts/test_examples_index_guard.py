#!/usr/bin/env python3
"""Tests for tools/scripts/examples_index_guard.py."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import examples_index_guard as guard  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]

INDEX = """# Examples

| Directory | What it shows | Run |
|---|---|---|
| `alpha` | one | `run alpha` |
| `beta`, `gamma` | two | `run` |

| Directory | What it proves | Run | Planned location |
|---|---|---|---|
| `delta` | proof | `run` | `test/consumers/delta` |
"""


def make_repo(readme: str, dirs: list[str]) -> tempfile.TemporaryDirectory:
    tmp = tempfile.TemporaryDirectory()
    root = Path(tmp.name)
    (root / "examples").mkdir()
    (root / "examples" / "README.md").write_text(readme)
    for d in dirs:
        (root / "examples" / d).mkdir()
        (root / "examples" / d / "CMakeLists.txt").write_text("")
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run(["git", "add", "-A"], cwd=root, check=True)
    return tmp


def run(readme: str, dirs: list[str]) -> list[str]:
    with make_repo(readme, dirs) as tmp:
        return guard.check(Path(tmp))


class ExamplesIndexGuardTest(unittest.TestCase):
    ALL = ["alpha", "beta", "gamma", "delta"]

    def test_complete_index_passes(self):
        self.assertEqual(run(INDEX, self.ALL), [])

    def test_unlisted_directory_fails(self):
        problems = run(INDEX, self.ALL + ["epsilon"])
        self.assertEqual(len(problems), 1)
        self.assertIn("epsilon", problems[0])

    def test_listed_but_missing_directory_fails(self):
        problems = run(INDEX, ["alpha", "beta", "gamma"])
        self.assertTrue(any("'delta'" in p for p in problems))

    def test_duplicate_row_fails(self):
        problems = run(INDEX + "| `alpha` | again | `run` |\n", self.ALL)
        self.assertTrue(any("2 times" in p for p in problems))

    def test_planned_location_must_repeat_the_name(self):
        bad = INDEX.replace("`test/consumers/delta`", "`test/consumers/other`")
        self.assertTrue(any("planned location" in p for p in run(bad, self.ALL)))

    def test_missing_readme_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(["git", "init", "-q"], cwd=tmp, check=True)
            self.assertTrue(guard.check(Path(tmp)))

    def test_repository_index_is_complete(self):
        self.assertEqual(guard.check(REPO_ROOT), [])


if __name__ == "__main__":
    unittest.main()
