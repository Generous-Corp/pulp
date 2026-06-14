#!/usr/bin/env python3
"""Tests for desktop source command rewrite dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_source_rewrite_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSourceRewriteBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.root = Path("/repo")

    def test_source_rewrite_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_SOURCE_REWRITE_COMMAND_EXPORTS,
            *self.mod.DESKTOP_SOURCE_REWRITE_ROOT_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_SOURCE_REWRITE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_source_rewrite_installer_routes_selected_groups(self):
        source_prep = types.SimpleNamespace(
            command_path_rewrite_candidate=lambda token, **kwargs: Path("/repo/tool"),
            rewrite_launch_command_for_posix_root=lambda command, remote_root, **kwargs: "posix-root",
        )
        bindings = {
            "_source_prep": source_prep,
            "ROOT": self.root,
        }

        self.mod.install_desktop_source_rewrite_helpers(
            bindings,
            ("command_path_rewrite_candidate", "rewrite_launch_command_for_posix_root"),
        )

        self.assertEqual(bindings["command_path_rewrite_candidate"]("./tool"), Path("/repo/tool"))
        self.assertEqual(bindings["rewrite_launch_command_for_posix_root"]("./tool", "/remote"), "posix-root")
        self.assertNotIn("rewrite_launch_command_for_mapper", bindings)
        self.assertNotIn("rewrite_launch_command_for_windows_root", bindings)

    def test_source_rewrite_installer_preserves_unknown_fallback(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_desktop_source_rewrite_helpers(bindings, ("unknown_helper",))

        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
