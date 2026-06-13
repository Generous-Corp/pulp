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

    def test_run_rollup_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_RUN_MANIFEST_EXPORTS,
            *self.mod.DESKTOP_RUN_ROLLUP_ACTION_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_RUN_ROLLUP_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

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

    def test_install_desktop_run_rollup_helpers_wires_named_exports(self):
        captured = {}

        def capture(name, result=None):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        bindings = {
            "_reporting": types.SimpleNamespace(
                desktop_run_manifests=capture("manifests", [{"ok": True}]),
                desktop_rollup_dir=capture("rollup_dir", Path("/tmp/rollups")),
                write_desktop_run_rollups=capture("write"),
                prune_desktop_run_manifests=capture("prune", [Path("/tmp/run")]),
            ),
            "desktop_artifact_root": object(),
            "desktop_rollup_dir": object(),
            "desktop_run_manifests": object(),
            "desktop_run_summary": object(),
            "desktop_proof_summaries": object(),
            "atomic_write_text": object(),
        }

        self.mod.install_desktop_run_rollup_helpers(bindings)

        self.assertEqual(bindings["desktop_run_manifests"]({"desktop_automation": {}}), [{"ok": True}])
        self.assertEqual(bindings["desktop_rollup_dir"]({"desktop_automation": {}}, "mac"), Path("/tmp/rollups"))
        self.assertIsNone(bindings["write_desktop_run_rollups"]({"desktop_automation": {}}))
        self.assertEqual(bindings["prune_desktop_run_manifests"]({"desktop_automation": {}}), [Path("/tmp/run")])
        self.assertEqual(captured["manifests"][0], ({"desktop_automation": {}},))
        self.assertEqual(captured["rollup_dir"][0], ({"desktop_automation": {}}, "mac"))

    def test_install_desktop_run_rollup_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_desktop_run_rollup_helper = lambda _bindings: "future"

        self.mod.install_desktop_run_rollup_helpers(bindings, ("future_desktop_run_rollup_helper",))

        self.assertEqual(bindings["future_desktop_run_rollup_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
