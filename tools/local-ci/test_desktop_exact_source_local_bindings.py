#!/usr/bin/env python3
"""Tests for local exact-source worktree dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_exact_source_local_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopExactSourceLocalBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.root = Path("/repo")
        self.run_fn = object()

    def test_local_worktree_helpers_bind_root_and_subprocess(self):
        captured = {}

        def local_worktree_matches(path, sha, **kwargs):
            captured["matches"] = (path, sha, kwargs)
            return True

        def reset_local_worktree(path, **kwargs):
            captured["reset"] = (path, kwargs)

        source_prep = types.SimpleNamespace(
            local_worktree_matches=local_worktree_matches,
            reset_local_worktree=reset_local_worktree,
        )
        bindings = {
            "_source_prep": source_prep,
            "ROOT": self.root,
            "subprocess": types.SimpleNamespace(run=self.run_fn),
        }

        self.assertTrue(self.mod.local_worktree_matches(bindings, Path("/tmp/wt"), "abc123"))
        self.mod.reset_local_worktree(bindings, Path("/tmp/wt"))

        self.assertEqual(captured["matches"][0], Path("/tmp/wt"))
        self.assertEqual(captured["matches"][1], "abc123")
        self.assertIs(captured["matches"][2]["run_fn"], self.run_fn)
        self.assertEqual(captured["reset"][0], Path("/tmp/wt"))
        self.assertIs(captured["reset"][1]["run_fn"], self.run_fn)
        self.assertEqual(captured["reset"][1]["root"], self.root)

    def test_local_exports_and_installer_wire_selected_helpers(self):
        expected = (
            "local_worktree_matches",
            "reset_local_worktree",
        )
        self.assertEqual(self.mod.DESKTOP_EXACT_SOURCE_LOCAL_EXPORTS, expected)

        source_prep = types.SimpleNamespace(local_worktree_matches=lambda path, sha, **kwargs: True)
        bindings = {
            "_source_prep": source_prep,
            "subprocess": types.SimpleNamespace(run=self.run_fn),
        }

        self.mod.install_desktop_exact_source_local_helpers(bindings, ("local_worktree_matches",))

        self.assertTrue(bindings["local_worktree_matches"](Path("/tmp/wt"), "abc123"))
        self.assertNotIn("reset_local_worktree", bindings)


if __name__ == "__main__":
    unittest.main()
