#!/usr/bin/env python3
"""Structural tests for codecov.yml.

Regression coverage for the Codex post-merge sweep wave 3 findings on
PR #578 (Codecov wiring, Phase 1 PR 2):

- P2: `ignore:` must be a TOP-LEVEL key in the Codecov YAML schema.
  Nesting it under `coverage:` silently no-ops, which would pollute
  the baseline coverage numbers with FetchContent sources and tests.
- P1: Path-based coverage slicing across subsystem/platform/surface
  axes must be declared via `component_management`, not only via
  flag-level `paths:` on a multi-flag single upload. Components are
  Codecov's canonical path-slicing mechanism and trend-analysis
  consumer.

The test is structural only — no network call to Codecov. PyYAML is
already pulled in by other Pulp tooling.

Run:
    python3 tools/scripts/test_codecov_config.py
"""

from __future__ import annotations

import pathlib
import unittest

import yaml

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
CODECOV = REPO_ROOT / "codecov.yml"


# Canonical 13 core subsystems + 4 platforms + 3 surfaces = 20.
# Kept in lockstep with codecov.yml `flags:` and `component_management:
# individual_components` blocks; drift here is a real bug.
EXPECTED_IDS = {
    # subsystems (13)
    "audio", "canvas", "events", "format", "host", "midi", "osc",
    "platform", "render", "runtime", "signal", "state", "view",
    # platforms (4)
    "android", "apple", "linux", "windows",
    # surfaces (3)
    "cli", "ship", "tools",
}


class CodecovYamlStructure(unittest.TestCase):

    @classmethod
    def setUpClass(cls) -> None:
        with CODECOV.open("r", encoding="utf-8") as fh:
            cls.doc = yaml.safe_load(fh)

    def test_ignore_is_top_level(self):
        # Codecov config schema: ignore MUST be a top-level key. If it
        # sits under `coverage:` the exclusions silently no-op.
        self.assertIn("ignore", self.doc, "`ignore` missing at top level")
        coverage = self.doc.get("coverage", {})
        self.assertNotIn(
            "ignore",
            coverage,
            "`ignore` must NOT be nested under `coverage:` (silently no-ops)",
        )

    def test_ignore_covers_external_and_test_sources(self):
        # Regression: the baseline metrics in Phase 1 must not count
        # FetchContent-ed sources or our test trees.
        patterns = set(self.doc["ignore"])
        required = {
            "external/**",
            "_deps/**",
            "test/**",
            "examples/**",
            "build/**",
            "build-coverage/**",
            "**/Catch2/**",
            "**/catch2/**",
        }
        missing = required - patterns
        self.assertFalse(
            missing,
            f"codecov.yml ignore missing required patterns: {sorted(missing)}",
        )

    def test_component_management_defines_all_axes(self):
        # Path-based slicing lives on components, not only on flags.
        # Keep the 20-component carve-up explicit and checked.
        self.assertIn(
            "component_management",
            self.doc,
            "component_management missing — flags alone are ambiguous for "
            "Codecov's trend analysis on a single-upload Cobertura XML.",
        )
        ids = {
            c["component_id"]
            for c in self.doc["component_management"]["individual_components"]
        }
        self.assertEqual(
            ids,
            EXPECTED_IDS,
            f"component set drifted from canonical 20 (missing={EXPECTED_IDS - ids}, "
            f"extra={ids - EXPECTED_IDS})",
        )

    def test_flags_and_components_stay_aligned(self):
        # Flag names must match component ids so dashboard filters
        # read consistently regardless of which dimension is selected.
        flag_names = set(self.doc.get("flags", {}).keys())
        comp_ids = {
            c["component_id"]
            for c in self.doc["component_management"]["individual_components"]
        }
        self.assertEqual(
            flag_names,
            comp_ids,
            "flags and component_ids drifted — cross-filter queries on the "
            "Codecov dashboard will be inconsistent.",
        )


if __name__ == "__main__":
    unittest.main()
