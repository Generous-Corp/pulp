#!/usr/bin/env python3
"""Tests for desktop exact-SHA source dependency bindings."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_exact_source_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopExactSourceBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exact_source_exports_compose_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_EXACT_SOURCE_LOCAL_EXPORTS,
            *self.mod.DESKTOP_EXACT_SOURCE_MACOS_EXPORTS,
            *self.mod.DESKTOP_EXACT_SOURCE_REMOTE_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_EXACT_SOURCE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_exact_source_installer_routes_selected_groups_and_unknown_exports(self):
        calls = []

        def local_install(bindings, names):
            calls.append(("local", names))

        def macos_install(bindings, names):
            calls.append(("macos", names))

        def remote_install(bindings, names):
            calls.append(("remote", names))

        def fallback_install(bindings, globals_obj, names):
            calls.append(("local_fallback", names))

        self.mod.install_desktop_exact_source_local_helpers = local_install
        self.mod.install_desktop_exact_source_macos_helpers = macos_install
        self.mod.install_desktop_exact_source_remote_helpers = remote_install
        self.mod.install_local_helpers = fallback_install

        self.mod.install_desktop_exact_source_helpers(
            {},
            (
                "local_worktree_matches",
                "prepare_macos_exact_sha_source",
                "prepare_linux_exact_sha_source",
                "custom_exact_source_export",
            ),
        )

        self.assertEqual(
            calls,
            [
                ("local", ("local_worktree_matches",)),
                ("macos", ("prepare_macos_exact_sha_source",)),
                ("remote", ("prepare_linux_exact_sha_source",)),
                ("local_fallback", ("custom_exact_source_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
