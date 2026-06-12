#!/usr/bin/env python3
"""Tests for desktop run manifest and rollup dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_run_rollup_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopRunRollupBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        bindings = {
            "_reporting": types.SimpleNamespace(**{runner_name: runner}),
        }
        for name in [
            "desktop_artifact_root",
            "desktop_rollup_dir",
            "desktop_run_manifests",
            "desktop_run_summary",
            "desktop_proof_summaries",
            "atomic_write_text",
        ]:
            bindings[name] = object()
        return bindings

    def test_run_manifest_and_rollup_dir_bind_artifact_root(self):
        cases = [
            (
                "desktop_run_manifests",
                self.mod.desktop_run_manifests,
                {"target_name": "mac", "action": "smoke"},
                {"desktop_artifact_root_fn": "desktop_artifact_root"},
                [{"ok": True}],
            ),
            (
                "desktop_rollup_dir",
                self.mod.desktop_rollup_dir,
                {"target_name": "mac"},
                {"desktop_artifact_root_fn": "desktop_artifact_root"},
                Path("/tmp/rollups"),
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
                if runner_name == "desktop_rollup_dir":
                    result = wrapper(bindings, config, kwargs["target_name"])
                    self.assertEqual(captured["args"], (config, kwargs["target_name"]))
                else:
                    result = wrapper(bindings, config, **kwargs)
                    self.assertEqual(captured["args"], (config,))
                    for key, value in kwargs.items():
                        self.assertEqual(captured["kwargs"][key], value)
                self.assertEqual(result, expected)
                for kwarg, binding_name in expected_bindings.items():
                    self.assertIs(captured["kwargs"][kwarg], bindings[binding_name])

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


if __name__ == "__main__":
    unittest.main()
