#!/usr/bin/env python3
"""Tests for desktop artifact dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("desktop_artifact_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopArtifactBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_artifact_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_RECEIPT_ARTIFACT_EXPORTS,
            *self.mod.DESKTOP_RUN_ARTIFACT_EXPORTS,
            *self.mod.DESKTOP_PUBLISH_ARTIFACT_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_ARTIFACT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_artifact_installer_routes_selected_groups(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_receipt_artifact_helpers") as install_receipt,
            mock.patch.object(self.mod, "install_desktop_run_artifact_helpers") as install_run,
            mock.patch.object(self.mod, "install_desktop_publish_artifact_helpers") as install_publish,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_artifact_helpers(
                bindings,
                ("desktop_target_receipt_path", "desktop_artifact_root", "desktop_publish_root", "custom_artifact"),
            )

        install_receipt.assert_called_once_with(bindings, ("desktop_target_receipt_path",))
        install_run.assert_called_once_with(bindings, ("desktop_artifact_root",))
        install_publish.assert_called_once_with(bindings, ("desktop_publish_root",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_artifact",))


if __name__ == "__main__":
    unittest.main()
