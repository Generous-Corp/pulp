#!/usr/bin/env python3
"""Tests for generic desktop support facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("desktop_support_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSupportBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_support_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_ARTIFACT_EXPORTS,
            *self.mod.DESKTOP_DOCTOR_EXPORTS,
            *self.mod.DESKTOP_ACTION_SUPPORT_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_SUPPORT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_support_helpers_routes_each_group_and_unknown_exports(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_artifact_helpers") as install_artifact,
            mock.patch.object(self.mod, "install_desktop_doctor_helpers") as install_doctor,
            mock.patch.object(self.mod, "install_desktop_action_support_helpers") as install_action,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_support_helpers(
                bindings,
                ("desktop_artifact_root", "webdriver_status_url", "default_desktop_label", "custom"),
            )

        install_artifact.assert_called_once_with(bindings, ("desktop_artifact_root",))
        install_doctor.assert_called_once_with(bindings, ("webdriver_status_url",))
        install_action.assert_called_once_with(bindings, ("default_desktop_label",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
