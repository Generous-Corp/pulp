#!/usr/bin/env python3
"""Tests for desktop reporting facade composition."""

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_reporting_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopReportingBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_reporting_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_PUBLISH_EXPORTS,
            *self.mod.DESKTOP_RUN_ROLLUP_EXPORTS,
            *self.mod.DESKTOP_PROOF_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_REPORTING_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_reporting_helpers_routes_each_group(self):
        bindings = {
            "_reporting": types.SimpleNamespace(
                desktop_publish_reports=lambda *args, **kwargs: [{"publish": kwargs}],
                desktop_run_manifests=lambda *args, **kwargs: [{"run": kwargs}],
                desktop_proof_summaries=lambda *args, **kwargs: [{"proof": kwargs}],
            ),
            "desktop_publish_root": object(),
            "desktop_artifact_root": object(),
            "desktop_run_manifests": object(),
            "desktop_run_summary": object(),
        }

        self.mod.install_desktop_reporting_helpers(
            bindings,
            ("desktop_publish_reports", "desktop_run_manifests", "desktop_proof_summaries"),
        )

        self.assertEqual(
            bindings["desktop_publish_reports"]({"desktop_automation": {}}, limit=1),
            [{"publish": {"limit": 1, "desktop_publish_root_fn": bindings["desktop_publish_root"]}}],
        )
        self.assertEqual(
            bindings["desktop_run_manifests"]({"desktop_automation": {}}, target_name="mac"),
            [{
                "run": {
                    "target_name": "mac",
                    "action": None,
                    "desktop_artifact_root_fn": bindings["desktop_artifact_root"],
                }
            }],
        )
        self.assertEqual(
            bindings["desktop_proof_summaries"]({"desktop_automation": {}}, target_name="mac"),
            [{
                "proof": {
                    "target_name": "mac",
                    "action": None,
                    "source_mode": None,
                    "sha": None,
                    "branch": None,
                    "limit": None,
                    "desktop_run_manifests_fn": bindings["desktop_run_manifests"],
                    "desktop_run_summary_fn": bindings["desktop_run_summary"],
                }
            }],
        )
        self.assertNotIn("stage_desktop_publish_report", bindings)
        self.assertNotIn("write_desktop_run_rollups", bindings)


if __name__ == "__main__":
    unittest.main()
