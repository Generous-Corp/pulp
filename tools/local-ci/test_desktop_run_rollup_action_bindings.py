#!/usr/bin/env python3
"""Tests for desktop run rollup write/prune dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("desktop_run_rollup_action_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopRunRollupActionBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_rollup_action_exports_match_wrappers(self):
        expected = (
            *self.mod.DESKTOP_RUN_ROLLUP_WRITE_EXPORTS,
            *self.mod.DESKTOP_RUN_ROLLUP_PRUNE_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_RUN_ROLLUP_ACTION_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_desktop_run_rollup_action_helpers_routes_selected_groups(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_run_rollup_write_helpers") as write,
            mock.patch.object(self.mod, "install_desktop_run_rollup_prune_helpers") as prune,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_run_rollup_action_helpers(bindings, ("prune_desktop_run_manifests",))

        write.assert_called_once_with(bindings, ())
        prune.assert_called_once_with(bindings, ("prune_desktop_run_manifests",))
        install_local.assert_not_called()

    def test_install_desktop_run_rollup_action_helpers_preserves_unknown_fallback(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_run_rollup_write_helpers") as write,
            mock.patch.object(self.mod, "install_desktop_run_rollup_prune_helpers") as prune,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_run_rollup_action_helpers(bindings, ("custom",))

        write.assert_called_once_with(bindings, ())
        prune.assert_called_once_with(bindings, ())
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
