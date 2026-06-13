#!/usr/bin/env python3
"""Tests for desktop run rollup write/prune dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_run_rollup_action_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopRunRollupActionBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        bindings = {
            "_reporting": types.SimpleNamespace(**{runner_name: runner}),
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

    def test_rollup_action_exports_match_wrappers(self):
        expected = (
            *self.mod.DESKTOP_RUN_ROLLUP_WRITE_EXPORTS,
            *self.mod.DESKTOP_RUN_ROLLUP_PRUNE_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_RUN_ROLLUP_ACTION_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_run_rollups_bind_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs

        bindings = self._bindings("write_desktop_run_rollups", runner)
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

    def test_prune_desktop_run_manifests_binds_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return [Path("/tmp/run")]

        bindings = self._bindings("prune_desktop_run_manifests", runner)
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

    def test_install_desktop_run_rollup_action_helpers_wires_named_exports(self):
        captured = {}

        def write_runner(*args, **kwargs):
            captured["write"] = (args, kwargs)

        def prune_runner(*args, **kwargs):
            captured["prune"] = (args, kwargs)
            return [Path("/tmp/run")]

        bindings = self._bindings("write_desktop_run_rollups", write_runner)
        bindings["_reporting"].prune_desktop_run_manifests = prune_runner

        self.mod.install_desktop_run_rollup_action_helpers(bindings)

        self.assertIsNone(bindings["write_desktop_run_rollups"]({"desktop_automation": {}}, target_name="mac"))
        self.assertEqual(bindings["prune_desktop_run_manifests"]({"desktop_automation": {}}, keep_last=1), [Path("/tmp/run")])
        self.assertEqual(captured["write"][0], ({"desktop_automation": {}},))
        self.assertEqual(captured["prune"][0], ({"desktop_automation": {}},))

    def test_install_desktop_run_rollup_action_helpers_routes_selected_groups(self):
        captured = {}

        def write_runner(*args, **kwargs):
            captured["write"] = (args, kwargs)

        def prune_runner(*args, **kwargs):
            captured["prune"] = (args, kwargs)
            return [Path("/tmp/run")]

        bindings = self._bindings("write_desktop_run_rollups", write_runner)
        bindings["_reporting"].prune_desktop_run_manifests = prune_runner

        self.mod.install_desktop_run_rollup_action_helpers(bindings, ("prune_desktop_run_manifests",))

        self.assertNotIn("write_desktop_run_rollups", bindings)
        self.assertEqual(bindings["prune_desktop_run_manifests"]({"desktop_automation": {}}, keep_last=1), [Path("/tmp/run")])
        self.assertNotIn("write", captured)
        self.assertEqual(captured["prune"][0], ({"desktop_automation": {}},))

    def test_install_desktop_run_rollup_action_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_desktop_run_rollup_action_helper = lambda _bindings: "future"

        self.mod.install_desktop_run_rollup_action_helpers(bindings, ("future_desktop_run_rollup_action_helper",))

        self.assertEqual(bindings["future_desktop_run_rollup_action_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
