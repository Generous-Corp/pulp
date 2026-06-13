#!/usr/bin/env python3
"""Tests for desktop run manifest lookup dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_run_manifest_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopRunManifestBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        return {
            "_reporting": types.SimpleNamespace(**{runner_name: runner}),
            "desktop_artifact_root": object(),
        }

    def test_manifest_exports_match_wrappers(self):
        expected = (
            "desktop_run_manifests",
            "desktop_rollup_dir",
        )

        self.assertEqual(self.mod.DESKTOP_RUN_MANIFEST_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_run_manifest_and_rollup_dir_bind_artifact_root(self):
        cases = [
            (
                "desktop_run_manifests",
                self.mod.desktop_run_manifests,
                {"target_name": "mac", "action": "smoke"},
                [{"ok": True}],
            ),
            (
                "desktop_rollup_dir",
                self.mod.desktop_rollup_dir,
                {"target_name": "mac"},
                Path("/tmp/rollups"),
            ),
        ]
        for runner_name, wrapper, kwargs, expected in cases:
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
                self.assertIs(captured["kwargs"]["desktop_artifact_root_fn"], bindings["desktop_artifact_root"])

    def test_install_desktop_run_manifest_helpers_wires_named_exports(self):
        bindings = {
            "_reporting": types.SimpleNamespace(
                desktop_run_manifests=lambda config, **kwargs: [{"config": config, **kwargs}],
                desktop_rollup_dir=lambda config, target_name=None, **_kwargs: Path(f"/rollups/{target_name or 'all'}"),
            ),
            "desktop_artifact_root": object(),
        }

        self.mod.install_desktop_run_manifest_helpers(bindings)

        self.assertEqual(
            bindings["desktop_run_manifests"]({"desktop_automation": {}}, target_name="mac"),
            [{"config": {"desktop_automation": {}}, "target_name": "mac", "action": None, "desktop_artifact_root_fn": bindings["desktop_artifact_root"]}],
        )
        self.assertEqual(bindings["desktop_rollup_dir"]({"desktop_automation": {}}, "mac"), Path("/rollups/mac"))

    def test_install_desktop_run_manifest_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_desktop_run_manifest_helper = lambda _bindings: "future"

        self.mod.install_desktop_run_manifest_helpers(bindings, ("future_desktop_run_manifest_helper",))

        self.assertEqual(bindings["future_desktop_run_manifest_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
