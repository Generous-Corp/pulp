#!/usr/bin/env python3
"""Tests for source-prep facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("source_prep_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class SourcePrepBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.root = Path("/repo")
        self.run_fn = object()

    def test_source_prep_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_SOURCE_REQUEST_EXPORTS,
            *self.mod.DESKTOP_SOURCE_REWRITE_EXPORTS,
            *self.mod.DESKTOP_EXACT_SOURCE_EXPORTS,
        )

        self.assertEqual(self.mod.SOURCE_PREP_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def _bindings(self, **overrides):
        bindings = {
            "ROOT": self.root,
            "subprocess": types.SimpleNamespace(run=self.run_fn),
        }
        bindings.update(overrides)
        return bindings

    def test_install_source_prep_helpers_routes_each_group(self):
        source_prep = types.SimpleNamespace(
            desktop_source_cache_key=lambda source_request: source_request["sha"],
            command_path_rewrite_candidate=lambda command, **kwargs: Path("/repo/tool"),
            local_worktree_matches=lambda path, sha, **kwargs: True,
        )
        bindings = self._bindings(_source_prep=source_prep)

        self.mod.install_source_prep_helpers(
            bindings,
            (
                "desktop_source_cache_key",
                "command_path_rewrite_candidate",
                "local_worktree_matches",
            ),
        )

        self.assertEqual(bindings["desktop_source_cache_key"]({"sha": "abc123"}), "abc123")
        self.assertEqual(bindings["command_path_rewrite_candidate"]("./tool"), Path("/repo/tool"))
        self.assertTrue(bindings["local_worktree_matches"](Path("/tmp/wt"), "abc123"))
        self.assertNotIn("desktop_source_root", bindings)
        self.assertNotIn("rewrite_launch_command_for_source_root", bindings)
        self.assertNotIn("prepare_macos_exact_sha_source", bindings)


if __name__ == "__main__":
    unittest.main()
