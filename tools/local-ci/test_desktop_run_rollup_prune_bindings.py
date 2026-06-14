#!/usr/bin/env python3
"""Tests for desktop run manifest prune dependency bindings."""

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("desktop_run_rollup_prune_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopRunRollupPruneBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner):
        return {
            "_reporting": types.SimpleNamespace(prune_desktop_run_manifests=runner),
            "desktop_run_manifests": object(),
        }

    def test_rollup_prune_exports_match_wrappers(self):
        expected = ("prune_desktop_run_manifests",)

        self.assertEqual(self.mod.DESKTOP_RUN_ROLLUP_PRUNE_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_prune_desktop_run_manifests_binds_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return [Path("/tmp/run")]

        bindings = self._bindings(runner)
        config = {"desktop_automation": {}}
        self.assertEqual(
            self.mod.prune_desktop_run_manifests(
                bindings,
                config,
                target_name="mac",
                older_than_days=7,
                keep_last=2,
            ),
            [Path("/tmp/run")],
        )
        self.assertEqual(captured["args"], (config,))
        self.assertEqual(captured["kwargs"]["target_name"], "mac")
        self.assertEqual(captured["kwargs"]["older_than_days"], 7)
        self.assertEqual(captured["kwargs"]["keep_last"], 2)
        self.assertIs(captured["kwargs"]["desktop_run_manifests_fn"], bindings["desktop_run_manifests"])

if __name__ == "__main__":
    unittest.main()
