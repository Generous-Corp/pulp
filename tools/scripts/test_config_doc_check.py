#!/usr/bin/env python3
"""Unit tests for config_doc_check.py.

The gate's git access goes through gate_common.git_diff_names /
git_range_trailers, which are imported into the gate's namespace — so the tests
patch those two names directly and never touch a real repo. The real
config_doc_map.json shipped in this directory is used as the map, so the tests
also assert the seeded entries behave as intended.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "config_doc_check", HERE / "config_doc_check.py")
gate = importlib.util.module_from_spec(_spec)
# Register before exec: dataclasses resolves the module's string annotations
# (from `from __future__ import annotations`) via sys.modules[cls.__module__].
sys.modules["config_doc_check"] = gate
_spec.loader.exec_module(gate)


def run(changed, trailers=None, mode="report"):
    with mock.patch.object(gate, "git_diff_names", return_value=changed), \
         mock.patch.object(gate, "git_range_trailers", return_value=trailers or {}):
        # --repo-root avoids a git rev-parse call; the map path is resolved
        # relative to it, and the real map lives next to this test.
        return gate.main(
            ["--base", "origin/main", "--mode", mode,
             "--repo-root", str(HERE.parents[1])])


class ConfigDocCheckTest(unittest.TestCase):
    # ── config-without-doc → fail ──────────────────────────────────────
    def test_config_without_doc_fails(self):
        self.assertEqual(run([".shipyard/config.toml"]), 1)

    def test_workflow_config_without_doc_fails(self):
        self.assertEqual(run([".github/workflows/auto-release.yml"]), 1)

    # ── config-with-doc → pass ─────────────────────────────────────────
    def test_config_with_matching_doc_passes(self):
        self.assertEqual(
            run([".shipyard/config.toml", "docs/guides/versioning.md"]), 0)

    def test_config_with_other_doc_in_same_entry_passes(self):
        # .shipyard/config.toml maps to versioning.md OR local-ci.md — either
        # one satisfies the entry.
        self.assertEqual(
            run([".shipyard/config.toml", "docs/guides/local-ci.md"]), 0)

    def test_release_trio_with_watchdog_doc_passes(self):
        self.assertEqual(
            run([".github/workflows/release-cadence-check.yml",
                 "docs/guides/release-watchdog.md"]), 0)

    # ── doc-only → pass ────────────────────────────────────────────────
    def test_doc_only_passes(self):
        self.assertEqual(run(["docs/guides/versioning.md"]), 0)

    # ── bypass trailer → pass ──────────────────────────────────────────
    def test_bypass_trailer_passes(self):
        trailers = {"config-doc": ['skip reason="workflow comment typo only"']}
        self.assertEqual(run([".shipyard/config.toml"], trailers), 0)

    def test_bypass_trailer_case_insensitive_and_no_reason(self):
        # gate_common lowercases keys; a reason-less skip still bypasses.
        self.assertEqual(run([".github/workflows/build.yml"],
                             {"config-doc": ["skip"]}), 0)

    def test_unrelated_trailer_does_not_bypass(self):
        self.assertEqual(run([".shipyard/config.toml"],
                             {"version-bump": ["minor"]}), 1)

    # ── unmapped file → pass ───────────────────────────────────────────
    def test_unmapped_file_passes(self):
        self.assertEqual(run(["core/view/src/x.cpp", "README.md"]), 0)

    def test_no_changes_passes(self):
        self.assertEqual(run([]), 0)

    # ── mode semantics ─────────────────────────────────────────────────
    def test_hint_mode_never_fails(self):
        self.assertEqual(run([".shipyard/config.toml"], mode="hint"), 0)

    # ── cross-entry independence: one satisfied doc doesn't cover another ─
    def test_one_entry_satisfied_other_unsatisfied_still_fails(self):
        # versioning.md satisfies the shipyard entry, but build.yml's entry
        # (local-ci.md) is still unsatisfied → overall fail.
        self.assertEqual(
            run([".shipyard/config.toml", "docs/guides/versioning.md",
                 ".github/workflows/build.yml"]), 1)


class ConfigDocMapIntegrityTest(unittest.TestCase):
    def test_map_loads_and_has_entries(self):
        cfg_map = gate.load_map(HERE / "config_doc_map.json")
        self.assertTrue(cfg_map.entries)
        for e in cfg_map.entries:
            self.assertTrue(e.paths, "entry has no paths")
            self.assertTrue(e.docs, "entry has no docs")
            self.assertTrue(e.why, "entry has no why")

    def test_mapped_paths_and_docs_exist_on_disk(self):
        root = HERE.parents[1]
        cfg_map = gate.load_map(HERE / "config_doc_map.json")
        for e in cfg_map.entries:
            for p in e.paths:
                self.assertTrue((root / p).exists(),
                                f"mapped config path missing: {p}")
            for d in e.docs:
                self.assertTrue((root / d).exists(),
                                f"mapped doc missing: {d}")


if __name__ == "__main__":
    unittest.main()
