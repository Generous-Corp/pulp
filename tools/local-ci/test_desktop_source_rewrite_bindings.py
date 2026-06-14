#!/usr/bin/env python3
"""Tests for desktop source command rewrite dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
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

    def test_rewrite_wrappers_bind_root_and_windows_helpers(self):
        captured = {}

        def command_candidate(*args, **kwargs):
            captured["candidate"] = (args, kwargs)
            return Path("/repo/tool")

        def rewrite_mapper(*args, **kwargs):
            captured["mapper"] = (args, kwargs)
            return "rewritten"

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
            command_path_rewrite_candidate=command_candidate,
            rewrite_launch_command_for_mapper=rewrite_mapper,
            rewrite_launch_command_for_source_root=rewrite_source,
            rewrite_launch_command_for_posix_root=rewrite_posix,
            rewrite_launch_command_for_windows_root=rewrite_windows,
        )
        bindings = {
            "_source_prep": source_prep,
            "ROOT": self.root,
            "windows_path_join": object(),
        }
        mapper = object()

        self.assertEqual(self.mod.command_path_rewrite_candidate(bindings, "./tool"), Path("/repo/tool"))
        self.assertEqual(captured["candidate"][0], ("./tool",))
        self.assertEqual(captured["candidate"][1]["root"], self.root)

        self.assertEqual(self.mod.rewrite_launch_command_for_mapper(bindings, "./tool", mapper, windows=True), "rewritten")
        self.assertEqual(captured["mapper"][0], ("./tool", mapper))
        self.assertEqual(captured["mapper"][1]["root"], self.root)
        self.assertTrue(captured["mapper"][1]["windows"])

        self.assertEqual(self.mod.rewrite_launch_command_for_source_root(bindings, "./tool", Path("/source")), "source-root")
        self.assertEqual(captured["source"][0], ("./tool", Path("/source")))
        self.assertEqual(captured["source"][1]["root"], self.root)

        self.assertEqual(self.mod.rewrite_launch_command_for_posix_root(bindings, "./tool", "/remote"), "posix-root")
        self.assertEqual(captured["posix"][0], ("./tool", "/remote"))
        self.assertEqual(captured["posix"][1]["root"], self.root)

        self.assertEqual(self.mod.rewrite_launch_command_for_windows_root(bindings, r".\tool.exe", r"C:\Pulp"), "windows-root")
        self.assertEqual(captured["windows"][0], (r".\tool.exe", r"C:\Pulp"))
        self.assertEqual(captured["windows"][1]["root"], self.root)
        self.assertIs(captured["windows"][1]["windows_path_join_fn"], bindings["windows_path_join"])

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


if __name__ == "__main__":
    unittest.main()
