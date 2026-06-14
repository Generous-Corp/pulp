#!/usr/bin/env python3
"""Tests for source-prep facade bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock
from pathlib import Path



def load_module():
    return load_local_ci_module("source_prep_bindings.py")


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

    def test_install_source_prep_helpers_routes_each_group_and_fallback(self):
        bindings = {"ROOT": self.root}

        with (
            mock.patch.object(self.mod, "install_desktop_source_request_helpers") as request,
            mock.patch.object(self.mod, "install_desktop_source_rewrite_helpers") as rewrite,
            mock.patch.object(self.mod, "install_desktop_exact_source_helpers") as exact_source,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_source_prep_helpers(
                bindings,
                (
                    "desktop_source_cache_key",
                    "command_path_rewrite_candidate",
                    "local_worktree_matches",
                    "unknown_helper",
                ),
            )

        request.assert_called_once_with(bindings, ("desktop_source_cache_key",))
        rewrite.assert_called_once_with(bindings, ("command_path_rewrite_candidate",))
        exact_source.assert_called_once_with(bindings, ("local_worktree_matches",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
