#!/usr/bin/env python3
"""Tests for desktop run rollup write dependency bindings."""

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_run_rollup_write_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopRunRollupWriteBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner):
        bindings = {
            "_reporting": types.SimpleNamespace(write_desktop_run_rollups=runner),
        }
        for name in [
            "desktop_rollup_dir",
            "desktop_run_manifests",
            "desktop_run_summary",
            "desktop_proof_summaries",
            "atomic_write_text",
        ]:
            bindings[name] = object()
        return bindings

    def test_rollup_write_exports_match_wrappers(self):
        expected = ("write_desktop_run_rollups",)

        self.assertEqual(self.mod.DESKTOP_RUN_ROLLUP_WRITE_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_run_rollups_bind_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs

        bindings = self._bindings(runner)
        config = {"desktop_automation": {}}
        self.mod.write_desktop_run_rollups(bindings, config, target_name="windows")
        self.assertEqual(captured["args"], (config,))
        self.assertEqual(captured["kwargs"]["target_name"], "windows")
        for name in [
            "desktop_rollup_dir",
            "desktop_run_manifests",
            "desktop_run_summary",
            "desktop_proof_summaries",
            "atomic_write_text",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_install_desktop_run_rollup_write_helpers_wires_named_exports(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["write"] = (args, kwargs)

        bindings = self._bindings(runner)

        self.mod.install_desktop_run_rollup_write_helpers(bindings)

        self.assertIsNone(bindings["write_desktop_run_rollups"]({"desktop_automation": {}}, target_name="mac"))
        self.assertEqual(captured["write"][0], ({"desktop_automation": {}},))

    def test_install_desktop_run_rollup_write_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_desktop_run_rollup_write_helper = lambda _bindings: "future"

        self.mod.install_desktop_run_rollup_write_helpers(bindings, ("future_desktop_run_rollup_write_helper",))

        self.assertEqual(bindings["future_desktop_run_rollup_write_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
