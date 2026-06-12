#!/usr/bin/env python3
"""Tests for desktop artifact dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_artifact_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopArtifactBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, artifacts=None):
        return {
            "_desktop_artifacts": artifacts or types.SimpleNamespace(),
            "desktop_receipts_dir": object(),
            "desktop_target_receipt_path": object(),
        }

    def test_artifact_wrappers_bind_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        artifacts = types.SimpleNamespace(
            desktop_target_receipt_path=capture("receipt_path", Path("/receipts/mac.json")),
            desktop_receipt_for=capture("receipt", {"installed": True}),
            desktop_artifact_root=capture("artifact_root", Path("/artifacts")),
            create_desktop_run_bundle=capture("run_bundle", Path("/artifacts/mac/smoke/run")),
            desktop_publish_root=capture("publish_root", Path("/artifacts/_published")),
            create_desktop_publish_bundle=capture("publish_bundle", Path("/artifacts/_published/run")),
        )
        bindings = self._bindings(artifacts=artifacts)
        config = {"desktop_automation": {"artifact_root": "/artifacts"}}

        self.assertEqual(self.mod.desktop_target_receipt_path(bindings, "mac"), Path("/receipts/mac.json"))
        self.assertIs(captured["receipt_path"][1]["desktop_receipts_dir_fn"], bindings["desktop_receipts_dir"])
        self.assertEqual(self.mod.desktop_receipt_for(bindings, "mac"), {"installed": True})
        self.assertIs(captured["receipt"][1]["desktop_target_receipt_path_fn"], bindings["desktop_target_receipt_path"])
        self.assertEqual(self.mod.desktop_artifact_root(bindings, config), Path("/artifacts"))
        self.assertEqual(captured["artifact_root"][0], (config,))
        self.assertEqual(self.mod.create_desktop_run_bundle(bindings, config, "mac", "smoke"), Path("/artifacts/mac/smoke/run"))
        self.assertEqual(captured["run_bundle"][0], (config, "mac", "smoke"))
        self.assertEqual(self.mod.desktop_publish_root(bindings, config), Path("/artifacts/_published"))
        self.assertEqual(self.mod.create_desktop_publish_bundle(bindings, config), Path("/artifacts/_published/run"))


if __name__ == "__main__":
    unittest.main()
