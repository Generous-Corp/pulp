#!/usr/bin/env python3
"""Tests for desktop source request dependency bindings."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_source_request_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSourceRequestBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_source_request_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_SOURCE_REQUEST_CORE_EXPORTS,
            *self.mod.DESKTOP_SOURCE_REQUEST_PATH_EXPORTS,
            *self.mod.DESKTOP_SOURCE_REQUEST_WINDOWS_EXPORTS,
            *self.mod.DESKTOP_SOURCE_REQUEST_MANIFEST_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_SOURCE_REQUEST_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_source_request_installer_routes_selected_groups_and_unknown_fallback(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_source_request_core_helpers") as core,
            mock.patch.object(self.mod, "install_desktop_source_request_path_helpers") as path,
            mock.patch.object(self.mod, "install_desktop_source_request_windows_helpers") as windows,
            mock.patch.object(self.mod, "install_desktop_source_request_manifest_helpers") as manifest,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_source_request_helpers(
                bindings,
                (
                    "make_desktop_source_request",
                    "desktop_source_cache_key",
                    "split_windows_prepare_commands",
                    "attach_desktop_source_to_manifest",
                    "unknown_helper",
                ),
            )

        core.assert_called_once_with(bindings, ("make_desktop_source_request",))
        path.assert_called_once_with(bindings, ("desktop_source_cache_key",))
        windows.assert_called_once_with(bindings, ("split_windows_prepare_commands",))
        manifest.assert_called_once_with(bindings, ("attach_desktop_source_to_manifest",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
