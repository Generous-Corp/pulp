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

import run_python_coverage
import yaml

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
CODECOV = REPO_ROOT / "codecov.yml"
CORE_DIR = REPO_ROOT / "core"
COVERAGE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "coverage.yml"
PULP_REACT_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "pulp-react-build.yml"


EXPECTED_NON_CORE_COMPONENT_IDS = {
    # platforms
    "android", "apple", "linux", "windows",
    # surfaces
    "cli", "inspect", "ship", "tools",
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

    def component_entries(self):
        return self.doc["component_management"]["individual_components"]

    def components_by_id(self):
        return {entry["component_id"]: entry for entry in self.component_entries()}

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

    def test_ignore_covers_python_coverage_test_harness_omits(self):
        # tools/sandbox-e2e is a test harness in both Codecov and the
        # local Python coverage lane. Keep the broader Codecov glob in
        # place when the coverage runner omits its Python files.
        python_omits = {
            omit
            for surface in run_python_coverage.COVERAGE_SURFACES
            for omit in surface.resolved_omit_globs()
        }
        self.assertIn("tools/sandbox-e2e/*.py", python_omits)
        self.assertIn("tools/sandbox-e2e/**/*.py", python_omits)
        self.assertIn("tools/sandbox-e2e/**", set(self.doc["ignore"]))
        self.assertIn("tools/harness/visual/tests/*.py", python_omits)
        self.assertIn("tools/harness/visual/tests/**/*.py", python_omits)
        self.assertIn("tools/harness/visual/tests/**", set(self.doc["ignore"]))
        self.assertIn("tools/test_*.py", python_omits)
        self.assertIn("tools/test_*.py", set(self.doc["ignore"]))
        self.assertIn("tools/ci/test_*.py", python_omits)
        self.assertIn("tools/ci/test_*.py", set(self.doc["ignore"]))
        self.assertIn("tools/motion/visual/test_*.py", python_omits)
        self.assertIn("tools/motion/visual/test_*.py", set(self.doc["ignore"]))

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

    def test_component_ids_are_unique_named_and_path_scoped(self):
        # Duplicate ids are hard to spot in YAML review and would make
        # one component shadow another in downstream consumers.
        entries = self.component_entries()
        ids = [entry.get("component_id") for entry in entries]
        self.assertEqual(
            len(ids),
            len(set(ids)),
            f"component_id values must be unique: {ids}",
        )

        for entry in entries:
            component_id = entry["component_id"]
            self.assertEqual(
                entry.get("name"),
                component_id,
                f"{component_id} component name must mirror component_id",
            )
            paths = entry.get("paths")
            self.assertIsInstance(
                paths,
                list,
                f"{component_id} component must declare a paths list",
            )
            self.assertGreater(
                len(paths),
                0,
                f"{component_id} component must declare at least one path",
            )
            for path in paths:
                self.assertIsInstance(
                    path,
                    str,
                    f"{component_id} component path must be a string",
                )
                self.assertTrue(
                    path.endswith("/**"),
                    f"{component_id} component path must recurse with /**: {path}",
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

    def test_flags_have_expected_path_and_carryforward_shape(self):
        flags = self.doc["flags"]
        comp_ids = expected_component_ids()

        for flag_name in sorted(comp_ids):
            with self.subTest(flag=flag_name):
                flag = flags[flag_name]
                self.assertIs(
                    flag.get("carryforward"),
                    True,
                    f"{flag_name} flag must carry forward coverage",
                )
                paths = flag.get("paths")
                self.assertIsInstance(
                    paths,
                    list,
                    f"{flag_name} flag must declare a paths list",
                )
                self.assertGreater(
                    len(paths),
                    0,
                    f"{flag_name} flag must declare at least one path",
                )
                for path in paths:
                    self.assertIsInstance(
                        path,
                        str,
                        f"{flag_name} flag path must be a string",
                    )
                    self.assertTrue(
                        path.endswith("/"),
                        f"{flag_name} flag path must remain directory-scoped: {path}",
                    )

        for flag_name in sorted(EXPECTED_UPLOAD_ONLY_FLAGS):
            with self.subTest(flag=flag_name):
                flag = flags[flag_name]
                self.assertIs(
                    flag.get("carryforward"),
                    True,
                    f"{flag_name} upload flag must carry forward coverage",
                )
                self.assertNotIn(
                    "paths",
                    flag,
                    f"{flag_name} upload flag must not be path-scoped",
                )

    def test_notify_count_matches_pr_coverage_upload_contract(self):
        # Ordinary PR coverage is macOS-only. Android Kotlin and other
        # cross-platform uploads run on main/manual/nightly lanes, so the
        # Codecov bot must not wait for a second PR upload that never comes.
        self.assertEqual(self.doc["codecov"]["notify"]["after_n_builds"], 1)
        coverage = COVERAGE_WORKFLOW.read_text(encoding="utf-8")
        self.assertIn(
            "github.event_name != 'pull_request' && needs.classify.outputs.native_build_required == 'true'",
            coverage,
        )

    def test_pulp_react_main_upload_is_centralized_in_coverage_workflow(self):
        coverage = COVERAGE_WORKFLOW.read_text(encoding="utf-8")
        react = PULP_REACT_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("pulp-react-coverage:", coverage)
        self.assertIn("name: Coverage report (@pulp/react, Vitest)", coverage)
        self.assertIn("if: github.event_name != 'pull_request'", coverage)
        self.assertIn("packages/pulp-react/coverage/cobertura-coverage.xml", coverage)
        self.assertIn("flags: pulp-react", coverage)

        self.assertIn(
            "if: always() && github.event_name == 'pull_request'",
            react,
            "pulp-react-build.yml must not upload Codecov data on push/main; "
            "main uploads belong to coverage.yml so cancelled native Coverage "
            "runs cannot leave Codecov on a mixed React+stale-native snapshot.",
        )

    def test_coverage_watchdog_waits_until_native_legs_exist(self):
        # Regression: the watchdog can start after `classify` but before
        # `matrix-config` has materialized the native coverage matrix.
        # Seeing zero coverage legs in that window must not be treated as
        # "all coverage legs have left queued", or a later macOS leg can
        # sit queued forever while main Codecov records stay stale.
        coverage = COVERAGE_WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("observed_coverage_legs=0", coverage)
        self.assertIn("coverage_legs_this_poll=0", coverage)
        self.assertIn("observed_coverage_legs=1", coverage)
        self.assertIn("no native coverage legs observed yet", coverage)
        self.assertIn(
            'if [ "${observed_coverage_legs}" -eq 1 ]; then',
            coverage,
        )

    def test_core_axes_use_canonical_directory_mappings(self):
        # Core subsystem ids come directly from core/* and should map to
        # exactly that directory in both flag and component declarations.
        # Components MAY additionally declare `!`-prefix exclude patterns
        # to push platform-specific subtrees into the matching platform
        # component (#1055); the first include path must still be the
        # canonical `core/<name>/**` directory glob.
        flags = self.doc["flags"]
        components = self.components_by_id()

        for entry in CORE_DIR.iterdir():
            if not entry.is_dir():
                continue
            expected_flag_paths = [f"core/{entry.name}/"]
            expected_canonical = f"core/{entry.name}/**"
            with self.subTest(component=entry.name):
                self.assertEqual(flags[entry.name]["paths"], expected_flag_paths)
                paths = components[entry.name]["paths"]
                include_paths = [p for p in paths if not p.startswith("!")]
                self.assertEqual(
                    include_paths,
                    [expected_canonical],
                    f"{entry.name} component must declare exactly one include "
                    "path mapped to its core/<name>/** directory; additional "
                    "entries must be `!`-prefix exclusions.",
                )

    def test_status_policy_matches_enforcement_model(self):
        # Project-wide Codecov remains advisory, but patch coverage should
        # report a real failure below 75%. Branch protection decides whether
        # that status is a hard merge gate.
        coverage_status = self.doc["coverage"]["status"]
        self.assertIs(coverage_status["project"]["default"]["informational"], True)
        self.assertEqual(coverage_status["project"]["default"]["target"], "auto")
        self.assertEqual(coverage_status["project"]["default"]["threshold"], "1%")
        self.assertEqual(coverage_status["patch"]["default"]["target"], "75%")
        self.assertNotIn("informational", coverage_status["patch"]["default"])

        component_statuses = (
            self.doc["component_management"]["default_rules"]["statuses"]
        )
        self.assertEqual(len(component_statuses), 1)
        self.assertEqual(
            component_statuses[0],
            {
                "type": "project",
                "target": "auto",
                "threshold": "1%",
                "informational": True,
            },
        )

    def test_platform_axes_keep_live_repo_path_conventions(self):
        # Regression: the repo's Windows sources live under `win/`,
        # not `windows/`, and the platform axes need the live naming.
        flags = self.doc["flags"]
        components = {
            component_id: set(entry.get("paths", []))
            for component_id, entry in self.components_by_id().items()
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

    def test_inspect_surface_is_first_class(self):
        # inspect/ is a first-party C++ surface and is included in the
        # native coverage object list, so it must not fall through as an
        # uncategorized Codecov path.
        flags = self.doc["flags"]
        components = {
            component_id: set(entry.get("paths", []))
            for component_id, entry in self.components_by_id().items()
        }

        self.assertEqual(set(flags["inspect"]["paths"]), {"inspect/"})
        self.assertEqual(components["inspect"], {"inspect/**"})

    def test_specific_tools_surface_precedes_broad_tools_surface(self):
        # The CLI slice is more specific than tools/. Keep it before the
        # broad tools slice anywhere YAML order affects dashboard grouping.
        flag_order = list(self.doc["flags"].keys())
        component_order = [
            entry["component_id"]
            for entry in self.component_entries()
        ]

        self.assertLess(flag_order.index("cli"), flag_order.index("tools"))
        self.assertLess(component_order.index("cli"), component_order.index("tools"))

    def test_comment_policy_requires_coverage_changes(self):
        comment = self.doc["comment"]
        self.assertEqual(comment["layout"], "reach, diff, flags, tree")
        self.assertEqual(comment["behavior"], "default")
        self.assertIs(comment["require_changes"], True)

    def test_ignore_aligned_with_diff_cover_excludes(self):
        """Codecov bot's `ignore` MUST mirror coverage_config.json's
        `diff_cover_excludes`. Drift causes the `codecov/patch` check to
        scream below-threshold on PRs whose diff is mostly in files the
        Pulp gate (`Diff coverage required`) silently excludes — which is
        what happened on PR #1984 (8.64% bot vs Pulp gate happy). Both
        sides MUST count the same set of files.
        """
        import json

        repo_root = pathlib.Path(__file__).resolve().parent.parent.parent
        gate_config = json.loads(
            (repo_root / "tools/scripts/coverage_config.json").read_text()
        )
        gate_excludes = set(gate_config["diff_cover_excludes"])
        bot_ignore = set(self.doc["ignore"])

        missing = gate_excludes - bot_ignore
        self.assertFalse(
            missing,
            f"codecov.yml `ignore:` is missing entries from "
            f"coverage_config.json `diff_cover_excludes`: {sorted(missing)}\n"
            f"Add them to codecov.yml's ignore list (under the "
            f"'Aligned with...' comment) or both gates will count the "
            f"same lines differently.",
        )


if __name__ == "__main__":
    unittest.main()
