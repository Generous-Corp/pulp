#!/usr/bin/env python3
"""Prove the shared source scan never reads generated or foreign trees.

Each decoy below is a real shape a working checkout produces: a build directory,
a hyphen-suffixed build directory an exact-name skip list lets through, fetched
dependency sources, an installed node package, and an agent tool directory
holding a complete checkout of another branch.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from repo_source_scan import is_scannable_dir, iter_sources  # noqa: E402

SOURCE = {
    "core/signal/svf.hpp",
    "test/test_signal.cpp",
    "examples/gain/gain.h",
    "tools/cli/main.cc",
}

DECOYS = {
    "build/_deps/dep-src/dep.hpp",
    "build/CMakeFiles/probe.cpp",
    "build-cov/gen/covered.hpp",
    "build-tsan/gen/raced.cpp",
    "core/build-cov/nested.hpp",
    "external/choc/choc.h",
    "planning/scratch/idea.hpp",
    "node_modules/pkg/binding.cc",
    "tools/node_modules/pkg/binding.cc",
    ".git/hooks/sample.hpp",
    ".claude/worktrees/agent-0/core/signal/svf.hpp",
    ".qwen/worktrees/agent-1/core/signal/svf.hpp",
    "core/.venv/include/vendored.hpp",
}


def plant(root: Path) -> None:
    for relative in SOURCE | DECOYS:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("// planted\n", encoding="utf-8")


class RepoSourceScan(unittest.TestCase):
    def test_scan_yields_source_and_ignores_every_decoy(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            plant(root)
            found = {
                str(path.relative_to(root))
                for path in iter_sources(root, ("*.hpp", "*.cpp", "*.h", "*.cc"))
            }
            self.assertEqual(found, SOURCE)

    def test_hyphen_suffixed_build_directories_are_excluded(self) -> None:
        for name in ("build", "build-cov", "build-coverage", "build-macos"):
            with self.subTest(name=name):
                self.assertFalse(is_scannable_dir(name))

    def test_dot_directories_are_excluded_as_a_class(self) -> None:
        for name in (".git", ".claude", ".qwen", ".venv", ".some-future-tool"):
            with self.subTest(name=name):
                self.assertFalse(is_scannable_dir(name))

    def test_source_directories_are_scannable(self) -> None:
        for name in ("core", "test", "examples", "tools", "apple", "bindings"):
            with self.subTest(name=name):
                self.assertTrue(is_scannable_dir(name))

    def test_caller_supplied_exclusions_apply(self) -> None:
        self.assertTrue(is_scannable_dir("examples"))
        self.assertFalse(is_scannable_dir("examples", ("examples",)))

    def test_patterns_select_only_matching_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            plant(root)
            found = {
                str(path.relative_to(root)) for path in iter_sources(root, ("*.hpp",))
            }
            self.assertEqual(found, {"core/signal/svf.hpp"})


if __name__ == "__main__":
    unittest.main(verbosity=1)
