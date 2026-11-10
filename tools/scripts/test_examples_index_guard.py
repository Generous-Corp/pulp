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
    (root / "examples" / "README.md").write_text(readme, encoding="utf-8")
    for d in dirs:
        (root / "examples" / d).mkdir()
        (root / "examples" / d / "CMakeLists.txt").write_text("", encoding="utf-8")
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


def guard_fixture(test: unittest.TestCase, guard_name: str, plugin: str = "Widget"):
    tmp = tempfile.TemporaryDirectory()
    test.addCleanup(tmp.cleanup)
    root = Path(tmp.name)
    (root / "examples" / "widget").mkdir(parents=True)
    (root / "examples" / "widget" / "CMakeLists.txt").write_text(
        f"pulp_add_plugin({plugin}\n    FORMATS Standalone\n)\n", encoding="utf-8")
    top = root / "examples" / "CMakeLists.txt"
    top.write_text(f"if(APPLE AND NOT TARGET {guard_name})\n    add_subdirectory(widget)\nendif()\n",
                   encoding="utf-8")
    return root, [top]


class ExampleSubdirectoryGuardTest(unittest.TestCase):
    """A `NOT TARGET` guard around an example's add_subdirectory must name a
    target the example creates; pulp_add_plugin(Foo) never creates a bare Foo."""

    def test_repository_guards_name_targets_the_examples_create(self):
        problems, _ = guard.guard_problems(REPO_ROOT)
        self.assertEqual(problems, [])

    def test_the_scan_sees_the_guards_it_is_meant_to_check(self):
        # Control: an empty scan would report no problems over zero coverage.
        # agent-toolkit-synth and sample-region-allpass are each guarded at both
        # of their add sites (test tree and examples/CMakeLists.txt).
        _, examined = guard.guard_problems(REPO_ROOT)
        self.assertGreaterEqual(examined, 4)

    def test_a_planted_bare_product_name_guard_fails(self):
        root, files = guard_fixture(self, "Widget")
        problems, examined = guard.guard_problems(root, files)
        self.assertEqual(examined, 1)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("NOT TARGET Widget", problems[0])
        self.assertIn("plug-ins: ['Widget']", problems[0])

    def test_a_suffixed_target_the_plugin_creates_passes(self):
        for name in ("Widget_Core", "Widget_Standalone"):
            root, files = guard_fixture(self, name)
            self.assertEqual(guard.guard_problems(root, files)[0], [], name)

    def test_a_target_the_directory_declares_by_name_passes(self):
        root, files = guard_fixture(self, "widget-core")
        (root / "examples" / "widget" / "CMakeLists.txt").write_text(
            "add_library(widget-core STATIC a.cpp)\n", encoding="utf-8")
        self.assertEqual(guard.guard_problems(root, files)[0], [])

    def test_a_guard_that_does_not_wrap_an_example_is_ignored(self):
        root, files = guard_fixture(self, "Widget_Core")
        files[0].write_text("if(NOT TARGET Unrelated)\n    message(STATUS x)\nendif()\n",
                            encoding="utf-8")
        self.assertEqual(guard.guard_problems(root, files), ([], 0))

    def test_a_guard_in_a_nested_if_is_attributed_to_the_right_block(self):
        root, files = guard_fixture(self, "Widget")
        files[0].write_text(
            "if(APPLE)\n    if(GPU AND NOT TARGET Widget)\n        add_subdirectory(widget)\n"
            "    endif()\nendif()\nadd_subdirectory(widget)\n", encoding="utf-8")
        self.assertEqual(len(guard.guard_problems(root, files)[0]), 1)


if __name__ == "__main__":
    unittest.main()
