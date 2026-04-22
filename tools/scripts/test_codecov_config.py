#!/usr/bin/env python3
"""Structural tests for codecov.yml.

Regression coverage for the Codex post-merge sweep wave 3 findings on
PR #578 (Codecov wiring, Phase 1 PR 2), plus the later `core/dsl`
mapping drift caught on 2026-04-22:

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
CORE_DIR = REPO_ROOT / "core"


EXPECTED_NON_CORE_COMPONENT_IDS = {
    # platforms
    "android", "apple", "linux", "windows",
    # surfaces
    "cli", "ship", "tools",
}

# Upload-axis flags are intentionally not components: they describe
# which CI leg uploaded the report, not which repo path the file lives
# under. Path slicing remains the job of component_management above.
EXPECTED_UPLOAD_ONLY_FLAGS = {
    "os-linux",
    "os-macos",
    "os-windows",
}


def expected_component_ids() -> set[str]:
    """Return the expected Codecov component ids for the live repo tree."""
    core_components = {
        entry.name
        for entry in CORE_DIR.iterdir()
        if entry.is_dir()
    }
    return core_components | EXPECTED_NON_CORE_COMPONENT_IDS


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
            "fetchcontent-src/**",
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
        # Every live top-level core/** module plus the platform/surface
        # slices must be represented.
        self.assertIn(
            "component_management",
            self.doc,
            "component_management missing — flags alone are ambiguous for "
            "Codecov's trend analysis on a single-upload Cobertura XML.",
        )
        expected = expected_component_ids()
        ids = {
            c["component_id"]
            for c in self.doc["component_management"]["individual_components"]
        }
        self.assertEqual(
            ids,
            expected,
            "component set drifted from the live core/* tree plus the "
            f"expected platform/surface slices (missing={expected - ids}, "
            f"extra={ids - expected})",
        )

    def test_flags_stay_aligned_with_components_and_upload_axes(self):
        # Path-based flags must match component ids 1:1, with only the
        # explicit upload-axis os-* flags allowed as extras.
        flag_names = set(self.doc.get("flags", {}).keys())
        comp_ids = expected_component_ids()
        self.assertEqual(
            flag_names,
            comp_ids | EXPECTED_UPLOAD_ONLY_FLAGS,
            "flags drifted from the expected component ids plus upload-only "
            "os-* flags — Codecov dashboard filters will be inconsistent.",
        )

    def test_platform_axes_keep_live_repo_path_conventions(self):
        # Regression: the repo's Windows sources live under `win/`,
        # not `windows/`, and the platform axes need the live naming.
        flags = self.doc["flags"]
        components = {
            entry["component_id"]: set(entry.get("paths", []))
            for entry in self.doc["component_management"]["individual_components"]
        }

        windows_paths = set(flags["windows"]["paths"]) | components["windows"]
        self.assertTrue(
            any("/win/" in path for path in windows_paths),
            "windows platform axis lost the repo's `win/` path mapping",
        )
        self.assertTrue(
            any("wasapi" in path for path in windows_paths),
            "windows platform axis must still cover WASAPI-specific paths",
        )

        linux_paths = set(flags["linux"]["paths"]) | components["linux"]
        self.assertTrue(
            any("/linux/" in path for path in linux_paths),
            "linux platform axis must cover `linux/` paths",
        )
        self.assertTrue(
            any("alsa" in path for path in linux_paths),
            "linux platform axis must keep the ALSA slice",
        )

        apple_paths = set(flags["apple"]["paths"]) | components["apple"]
        self.assertTrue(
            any("/mac/" in path for path in apple_paths),
            "apple platform axis must cover `mac/` paths",
        )
        self.assertTrue(
            any("/ios/" in path for path in apple_paths),
            "apple platform axis must cover `ios/` paths",
        )

        android_paths = set(flags["android"]["paths"]) | components["android"]
        self.assertTrue(
            any("/android/" in path for path in android_paths),
            "android platform axis must cover Android source paths",
        )


if __name__ == "__main__":
    unittest.main()
