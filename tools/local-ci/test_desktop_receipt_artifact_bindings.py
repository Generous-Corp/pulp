#!/usr/bin/env python3
"""Tests for desktop receipt artifact dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_receipt_artifact_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopReceiptArtifactBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_receipt_exports_match_wrappers(self) -> None:
        expected = (
            "desktop_target_receipt_path",
            "desktop_receipt_for",
        )

        self.assertEqual(self.mod.DESKTOP_RECEIPT_ARTIFACT_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_receipt_wrappers_bind_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        artifacts = types.SimpleNamespace(
            desktop_target_receipt_path=capture("receipt_path", Path("/receipts/mac.json")),
            desktop_receipt_for=capture("receipt", {"installed": True}),
        )
        bindings = {
            "_desktop_artifacts": artifacts,
            "desktop_receipts_dir": object(),
            "desktop_target_receipt_path": object(),
        }

        self.assertEqual(self.mod.desktop_target_receipt_path(bindings, "mac"), Path("/receipts/mac.json"))
        self.assertIs(captured["receipt_path"][1]["desktop_receipts_dir_fn"], bindings["desktop_receipts_dir"])
        self.assertEqual(self.mod.desktop_receipt_for(bindings, "mac"), {"installed": True})
        self.assertIs(captured["receipt"][1]["desktop_target_receipt_path_fn"], bindings["desktop_target_receipt_path"])

    def test_receipt_installer_wires_named_export(self) -> None:
        bindings = {
            "_desktop_artifacts": types.SimpleNamespace(
                desktop_target_receipt_path=lambda target_name, **kwargs: Path(f"/receipts/{target_name}.json"),
            ),
            "desktop_receipts_dir": object(),
        }

        self.mod.install_desktop_receipt_artifact_helpers(bindings, ("desktop_target_receipt_path",))

        self.assertEqual(bindings["desktop_target_receipt_path"]("mac"), Path("/receipts/mac.json"))
        self.assertNotIn("desktop_receipt_for", bindings)

    def test_install_desktop_receipt_artifact_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_desktop_receipt_artifact_helper = lambda _bindings: "future"

        self.mod.install_desktop_receipt_artifact_helpers(bindings, ("future_desktop_receipt_artifact_helper",))

        self.assertEqual(bindings["future_desktop_receipt_artifact_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
