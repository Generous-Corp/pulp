#!/usr/bin/env python3
"""Tests for desktop infrastructure facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("desktop_infra_bindings.py")


class DesktopInfraBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_desktop_infra_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.DESKTOP_INFRA_GIT_EXPORTS,
            *self.mod.DESKTOP_INFRA_REPORTING_EXPORTS,
            *self.mod.DESKTOP_INFRA_WAIT_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_INFRA_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_desktop_infra_helpers_routes_selected_groups_and_unknown_exports(self) -> None:
        bindings = {}
        names = ("normalize_git_remote_for_http", "slugify_token", "wait_for_path", "custom")

        with (
            mock.patch.object(self.mod, "install_desktop_infra_git_helpers") as git,
            mock.patch.object(self.mod, "install_desktop_infra_reporting_helpers") as reporting,
            mock.patch.object(self.mod, "install_desktop_infra_wait_helpers") as wait,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_infra_helpers(bindings, names)

        git.assert_called_once_with(bindings, ("normalize_git_remote_for_http",))
        reporting.assert_called_once_with(bindings, ("slugify_token",))
        wait.assert_called_once_with(bindings, ("wait_for_path",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
