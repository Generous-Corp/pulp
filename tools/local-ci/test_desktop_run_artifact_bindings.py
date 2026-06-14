#!/usr/bin/env python3
"""Tests for desktop run artifact dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_run_artifact_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopRunArtifactBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_run_exports_match_wrappers(self) -> None:
        expected = (
            "desktop_artifact_root",
            "create_desktop_run_bundle",
        )

        self.assertEqual(self.mod.DESKTOP_RUN_ARTIFACT_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_run_wrappers_delegate_arguments(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        artifacts = types.SimpleNamespace(
            desktop_artifact_root=capture("artifact_root", Path("/artifacts")),
            create_desktop_run_bundle=capture("run_bundle", Path("/artifacts/mac/smoke/run")),
        )
        bindings = {"_desktop_artifacts": artifacts}
        config = {"desktop_automation": {"artifact_root": "/artifacts"}}

        self.assertEqual(self.mod.desktop_artifact_root(bindings, config), Path("/artifacts"))
        self.assertEqual(captured["artifact_root"][0], (config,))
        self.assertEqual(self.mod.create_desktop_run_bundle(bindings, config, "mac", "smoke"), Path("/artifacts/mac/smoke/run"))
        self.assertEqual(captured["run_bundle"][0], (config, "mac", "smoke"))

    def test_run_installer_wires_named_export(self) -> None:
        bindings = {
            "_desktop_artifacts": types.SimpleNamespace(desktop_artifact_root=lambda config: Path("/artifacts")),
        }

        self.mod.install_desktop_run_artifact_helpers(bindings, ("desktop_artifact_root",))

        self.assertEqual(bindings["desktop_artifact_root"]({}), Path("/artifacts"))
        self.assertNotIn("create_desktop_run_bundle", bindings)


if __name__ == "__main__":
    unittest.main()
