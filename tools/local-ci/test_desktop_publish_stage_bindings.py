#!/usr/bin/env python3
"""Tests for desktop publish staging dependency bindings."""

from module_test_utils import load_local_ci_module
from pathlib import Path
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_publish_stage_bindings.py")


class DesktopPublishStageBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner):
        bindings = {"_reporting": types.SimpleNamespace(stage_desktop_publish_report=runner)}
        for name in [
            "create_desktop_publish_bundle",
            "now_iso",
            "atomic_write_text",
            "write_desktop_publish_rollups",
            "publish_report_to_branch",
        ]:
            bindings[name] = object()
        return bindings

    def test_stage_exports_match_wrappers(self):
        expected = ("stage_desktop_publish_report",)

        self.assertEqual(self.mod.DESKTOP_PUBLISH_STAGE_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_stage_desktop_publish_report_binds_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"run_count": 1}

        bindings = self._bindings(runner)
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

if __name__ == "__main__":
    unittest.main()
