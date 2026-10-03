#!/usr/bin/env python3
"""Tests for tools/ci/spawn_closure.py (runtime spawn edges from the codemodel).

The synthetic codemodel:

    pulp-test-t ── links ──► libx (STATIC) ── add_dependencies ──► gen (EXECUTABLE, a codegen tool)
         │
         ├── add_dependencies ──► pulp-cli (EXECUTABLE) ──► helper (EXECUTABLE)
         │                              └── links ──► liby (STATIC)
         └── add_dependencies ──► stage (UTILITY) ──► probe (MODULE_LIBRARY)

    loop-a (EXECUTABLE) ◄──► loop-b (EXECUTABLE)

What must hold:
- the closure holds every executable and module reachable through
  executables, modules and utility targets, transitively;
- it never walks through a linked library, so a codegen tool a library
  needs at build time is not a runtime spawn of the test;
- the test's own artifact is not in its closure, cycles terminate, and an
  artifact the codemodel does not describe has an empty closure;
- `<build>/` spellings and build-relative spellings are interchangeable.

Run:
    python3 tools/ci/test_spawn_closure.py
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spawn_closure as sc  # noqa: E402

TARGETS = {
    "pulp-test-t": {"type": "EXECUTABLE", "artifacts": ["<build>/test/pulp-test-t"],
                    "dependencies": ["libx", "pulp-cli", "stage"]},
    "libx": {"type": "STATIC_LIBRARY", "artifacts": ["<build>/libx.a"], "dependencies": ["gen"]},
    "gen": {"type": "EXECUTABLE", "artifacts": ["<build>/tools/gen"], "dependencies": []},
    "pulp-cli": {"type": "EXECUTABLE", "artifacts": ["<build>/tools/cli/pulp-cpp"],
                 "dependencies": ["helper", "liby"]},
    "helper": {"type": "EXECUTABLE", "artifacts": ["<build>/tools/helper"], "dependencies": []},
    "liby": {"type": "STATIC_LIBRARY", "artifacts": ["<build>/liby.a"], "dependencies": []},
    "stage": {"type": "UTILITY", "dependencies": ["probe"]},
    "probe": {"type": "MODULE_LIBRARY", "artifacts": ["<build>/test/probe.so"], "dependencies": []},
    "loop-a": {"type": "EXECUTABLE", "artifacts": ["<build>/loop-a"], "dependencies": ["loop-b"]},
    "loop-b": {"type": "EXECUTABLE", "artifacts": ["<build>/loop-b"], "dependencies": ["loop-a"]},
}


class SpawnClosureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.index = sc.SpawnIndex(TARGETS)

    def test_closure_reaches_spawned_and_loaded_targets_transitively(self) -> None:
        self.assertEqual(self.index.closure("test/pulp-test-t"),
                         {"tools/cli/pulp-cpp", "tools/helper", "test/probe.so"})

    def test_closure_never_walks_through_a_linked_library(self) -> None:
        self.assertNotIn("tools/gen", self.index.closure("test/pulp-test-t"))
        self.assertNotIn("liby.a", self.index.closure("test/pulp-test-t"))

    def test_spellings_are_interchangeable(self) -> None:
        self.assertEqual(self.index.closure("<build>/test/pulp-test-t"),
                         self.index.closure("test/pulp-test-t"))

    def test_cycles_terminate_and_exclude_the_start(self) -> None:
        self.assertEqual(self.index.closure("loop-a"), {"loop-b"})
        self.assertEqual(self.index.closure("loop-b"), {"loop-a"})

    def test_an_undescribed_artifact_has_an_empty_closure(self) -> None:
        self.assertEqual(self.index.closure("test/not-in-the-model"), frozenset())

    def test_expand_keeps_the_given_artifacts(self) -> None:
        self.assertEqual(self.index.expand(["<build>/tools/cli/pulp-cpp"]),
                         {"tools/cli/pulp-cpp", "tools/helper"})


if __name__ == "__main__":
    unittest.main()
