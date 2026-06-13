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

    def test_artifact_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_RECEIPT_ARTIFACT_EXPORTS,
            *self.mod.DESKTOP_RUN_ARTIFACT_EXPORTS,
            *self.mod.DESKTOP_PUBLISH_ARTIFACT_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_ARTIFACT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_artifact_installer_routes_selected_groups(self) -> None:
        artifacts = types.SimpleNamespace(
            desktop_target_receipt_path=lambda target_name, **kwargs: Path(f"/receipts/{target_name}.json"),
            desktop_artifact_root=lambda config: Path("/artifacts"),
            desktop_publish_root=lambda config: Path("/published"),
        )
        bindings = self._bindings(artifacts=artifacts)

        self.mod.install_desktop_artifact_helpers(
            bindings,
            ("desktop_target_receipt_path", "desktop_artifact_root", "desktop_publish_root"),
        )

        self.assertEqual(bindings["desktop_target_receipt_path"]("mac"), Path("/receipts/mac.json"))
        self.assertEqual(bindings["desktop_artifact_root"]({}), Path("/artifacts"))
        self.assertEqual(bindings["desktop_publish_root"]({}), Path("/published"))
        self.assertNotIn("desktop_receipt_for", bindings)
        self.assertNotIn("create_desktop_run_bundle", bindings)
        self.assertNotIn("create_desktop_publish_bundle", bindings)


if __name__ == "__main__":
    unittest.main()
