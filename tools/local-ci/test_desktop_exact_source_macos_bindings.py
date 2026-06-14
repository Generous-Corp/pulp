#!/usr/bin/env python3
"""Tests for macOS exact-source preparation dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_exact_source_macos_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopExactSourceMacosBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.root = Path("/repo")
        self.run_fn = object()

    def test_prepare_macos_exact_sha_source_binds_facade_dependencies(self):
        captured = {}

        def prepare(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"platform": "mac"}

        bindings = {
            "_source_prep": types.SimpleNamespace(prepare_macos_exact_sha_source=prepare),
            "ROOT": self.root,
            "subprocess": types.SimpleNamespace(run=self.run_fn),
            "desktop_source_root": object(),
            "_local_worktree_matches": object(),
            "_reset_local_worktree": object(),
            "run_logged_command": object(),
            "tail_lines": object(),
            "rewrite_launch_command_for_source_root": object(),
        }

        result = self.mod.prepare_macos_exact_sha_source(
            bindings,
            Path("/bundle"),
            "mac",
            "./tool",
            {"sha": "abc123"},
        )

        self.assertEqual(result, {"platform": "mac"})
        self.assertEqual(captured["args"], (Path("/bundle"), "mac", "./tool", {"sha": "abc123"}))
        self.assertEqual(captured["kwargs"]["root"], self.root)
        self.assertIs(captured["kwargs"]["run_fn"], self.run_fn)
        self.assertIs(captured["kwargs"]["desktop_source_root_fn"], bindings["desktop_source_root"])
        self.assertIs(captured["kwargs"]["local_worktree_matches_fn"], bindings["_local_worktree_matches"])
        self.assertIs(captured["kwargs"]["reset_local_worktree_fn"], bindings["_reset_local_worktree"])
        self.assertIs(captured["kwargs"]["rewrite_launch_command_for_source_root_fn"], bindings["rewrite_launch_command_for_source_root"])

    def test_macos_exports_match_wrappers(self):
        expected = ("prepare_macos_exact_sha_source",)
        self.assertEqual(self.mod.DESKTOP_EXACT_SOURCE_MACOS_EXPORTS, expected)
        self.assertTrue(callable(self.mod.prepare_macos_exact_sha_source))


if __name__ == "__main__":
    unittest.main()
