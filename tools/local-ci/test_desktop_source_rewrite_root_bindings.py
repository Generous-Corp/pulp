#!/usr/bin/env python3
"""Tests for desktop source-root rewrite dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_source_rewrite_root_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSourceRewriteRootBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.root = Path("/repo")

    def test_root_rewrite_exports_match_wrappers(self):
        expected = (
            "rewrite_launch_command_for_source_root",
            "rewrite_launch_command_for_posix_root",
            "rewrite_launch_command_for_windows_root",
        )

        self.assertEqual(self.mod.DESKTOP_SOURCE_REWRITE_ROOT_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_root_rewrite_wrappers_bind_root_and_windows_helper(self):
        captured = {}

        def rewrite_source(*args, **kwargs):
            captured["source"] = (args, kwargs)
            return "source-root"

        def rewrite_posix(*args, **kwargs):
            captured["posix"] = (args, kwargs)
            return "posix-root"

        def rewrite_windows(*args, **kwargs):
            captured["windows"] = (args, kwargs)
            return "windows-root"

        source_prep = types.SimpleNamespace(
            rewrite_launch_command_for_source_root=rewrite_source,
            rewrite_launch_command_for_posix_root=rewrite_posix,
            rewrite_launch_command_for_windows_root=rewrite_windows,
        )
        bindings = {
            "_source_prep": source_prep,
            "ROOT": self.root,
            "windows_path_join": object(),
        }

        self.assertEqual(self.mod.rewrite_launch_command_for_source_root(bindings, "./tool", Path("/source")), "source-root")
        self.assertEqual(captured["source"], (("./tool", Path("/source")), {"root": self.root}))

        self.assertEqual(self.mod.rewrite_launch_command_for_posix_root(bindings, "./tool", "/remote"), "posix-root")
        self.assertEqual(captured["posix"], (("./tool", "/remote"), {"root": self.root}))

        self.assertEqual(self.mod.rewrite_launch_command_for_windows_root(bindings, r".\tool.exe", r"C:\Pulp"), "windows-root")
        self.assertEqual(captured["windows"][0], (r".\tool.exe", r"C:\Pulp"))
        self.assertEqual(captured["windows"][1]["root"], self.root)
        self.assertIs(captured["windows"][1]["windows_path_join_fn"], bindings["windows_path_join"])

    def test_root_rewrite_installer_wires_selected_helpers(self):
        source_prep = types.SimpleNamespace(
            rewrite_launch_command_for_posix_root=lambda command, remote_root, **kwargs: "posix-root",
        )
        bindings = {
            "_source_prep": source_prep,
            "ROOT": self.root,
        }

        self.mod.install_desktop_source_rewrite_root_helpers(bindings, ("rewrite_launch_command_for_posix_root",))

        self.assertEqual(bindings["rewrite_launch_command_for_posix_root"]("./tool", "/remote"), "posix-root")
        self.assertNotIn("rewrite_launch_command_for_windows_root", bindings)


if __name__ == "__main__":
    unittest.main()
