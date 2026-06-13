#!/usr/bin/env python3
"""Tests for desktop source command rewrite dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_source_rewrite_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSourceRewriteCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.root = Path("/repo")

    def test_command_rewrite_exports_match_wrappers(self):
        expected = (
            "command_path_rewrite_candidate",
            "rewrite_launch_command_for_mapper",
        )

        self.assertEqual(self.mod.DESKTOP_SOURCE_REWRITE_COMMAND_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_command_rewrite_wrappers_bind_root(self):
        captured = {}

        def command_candidate(*args, **kwargs):
            captured["candidate"] = (args, kwargs)
            return Path("/repo/tool")

        def rewrite_mapper(*args, **kwargs):
            captured["mapper"] = (args, kwargs)
            return "rewritten"

        source_prep = types.SimpleNamespace(
            command_path_rewrite_candidate=command_candidate,
            rewrite_launch_command_for_mapper=rewrite_mapper,
        )
        bindings = {
            "_source_prep": source_prep,
            "ROOT": self.root,
        }
        mapper = object()

        self.assertEqual(self.mod.command_path_rewrite_candidate(bindings, "./tool"), Path("/repo/tool"))
        self.assertEqual(captured["candidate"], (("./tool",), {"root": self.root}))

        self.assertEqual(self.mod.rewrite_launch_command_for_mapper(bindings, "./tool", mapper, windows=True), "rewritten")
        self.assertEqual(captured["mapper"][0], ("./tool", mapper))
        self.assertEqual(captured["mapper"][1]["root"], self.root)
        self.assertTrue(captured["mapper"][1]["windows"])


if __name__ == "__main__":
    unittest.main()
