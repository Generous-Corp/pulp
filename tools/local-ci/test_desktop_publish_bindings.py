#!/usr/bin/env python3
"""Tests for desktop publish dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_publish_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopPublishBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        bindings = {
            "_reporting": types.SimpleNamespace(**{runner_name: runner}),
            "ROOT": Path("/repo"),
        }
        for name in [
            "_run_git",
            "_reset_local_worktree",
            "_clear_directory_contents",
            "git_origin_http_url",
            "create_desktop_publish_bundle",
            "now_iso",
            "atomic_write_text",
            "write_desktop_publish_rollups",
            "publish_report_to_branch",
            "desktop_publish_root",
            "desktop_publish_reports",
        ]:
            bindings[name] = object()
        return bindings

    def test_publish_report_to_branch_binds_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"mode": "branch"}

        bindings = self._bindings("publish_report_to_branch", runner)
        config = {"desktop_automation": {}}
        report = {"output_dir": "/tmp/report"}

        self.assertEqual(self.mod.publish_report_to_branch(bindings, config, report), {"mode": "branch"})
        self.assertEqual(captured["args"], (config, report))
        self.assertIs(captured["kwargs"]["root"], bindings["ROOT"])
        self.assertIs(captured["kwargs"]["run_git_fn"], bindings["_run_git"])
        self.assertIs(captured["kwargs"]["reset_local_worktree_fn"], bindings["_reset_local_worktree"])
        self.assertIs(captured["kwargs"]["clear_directory_contents_fn"], bindings["_clear_directory_contents"])
        self.assertIs(captured["kwargs"]["git_origin_http_url_fn"], bindings["git_origin_http_url"])

    def test_stage_publish_binds_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"run_count": 1}

        bindings = self._bindings("stage_desktop_publish_report", runner)
        config = {"desktop_automation": {}}
        manifests = [{"target": "mac"}]
        output_dir = Path("/tmp/out")

        self.assertEqual(
            self.mod.stage_desktop_publish_report(
                bindings,
                config,
                manifests,
                output_dir=output_dir,
                label="gallery",
            ),
            {"run_count": 1},
        )
        self.assertEqual(captured["args"], (config, manifests))
        self.assertIs(captured["kwargs"]["output_dir"], output_dir)
        self.assertEqual(captured["kwargs"]["label"], "gallery")
        for name in [
            "create_desktop_publish_bundle",
            "now_iso",
            "atomic_write_text",
            "write_desktop_publish_rollups",
            "publish_report_to_branch",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_report_listing_and_publish_rollups_bind_dependencies(self):
        cases = [
            (
                "desktop_publish_reports",
                self.mod.desktop_publish_reports,
                {"limit": 2},
                {"desktop_publish_root_fn": "desktop_publish_root"},
                [{"ok": True}],
            ),
            (
                "write_desktop_publish_rollups",
                self.mod.write_desktop_publish_rollups,
                {},
                {
                    "desktop_publish_root_fn": "desktop_publish_root",
                    "desktop_publish_reports_fn": "desktop_publish_reports",
                    "atomic_write_text_fn": "atomic_write_text",
                },
                None,
            ),
        ]
        for runner_name, wrapper, kwargs, expected_bindings, expected in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*args, **runner_kwargs):
                    captured["args"] = args
                    captured["kwargs"] = runner_kwargs
                    return expected

                bindings = self._bindings(runner_name, runner)
                config = {"desktop_automation": {}}
                self.assertEqual(wrapper(bindings, config, **kwargs), expected)
                self.assertEqual(captured["args"], (config,))
                for kwarg, binding_name in expected_bindings.items():
                    self.assertIs(captured["kwargs"][kwarg], bindings[binding_name])
                for key, value in kwargs.items():
                    self.assertEqual(captured["kwargs"][key], value)

    def test_publish_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_PUBLISH_BRANCH_EXPORTS,
            *self.mod.DESKTOP_PUBLISH_STAGE_EXPORTS,
            *self.mod.DESKTOP_PUBLISH_LIST_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_PUBLISH_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_publish_helpers_wires_named_exports(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return [{"target": "mac"}]

        bindings = self._bindings("desktop_publish_reports", runner)
        self.mod.install_desktop_publish_helpers(bindings, ("desktop_publish_reports",))

        self.assertEqual(bindings["desktop_publish_reports"]({"desktop_automation": {}}, limit=2), [{"target": "mac"}])
        self.assertEqual(captured["args"], ({"desktop_automation": {}},))
        self.assertEqual(captured["kwargs"]["limit"], 2)

    def test_install_desktop_publish_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_desktop_publish_helper = lambda _bindings: "future"

        self.mod.install_desktop_publish_helpers(bindings, ("future_desktop_publish_helper",))

        self.assertEqual(bindings["future_desktop_publish_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
