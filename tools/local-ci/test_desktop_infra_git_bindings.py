#!/usr/bin/env python3
"""Tests for desktop git infrastructure facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("desktop_infra_git_bindings.py")


class DesktopInfraGitBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_git_exports_match_wrappers(self) -> None:
        expected = (
            *self.mod.DESKTOP_INFRA_GIT_REMOTE_EXPORTS,
            *self.mod.DESKTOP_INFRA_GIT_ORIGIN_EXPORTS,
            *self.mod.DESKTOP_INFRA_GIT_RUN_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_INFRA_GIT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_desktop_infra_git_helpers_routes_named_exports_by_group_and_fallback(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_infra_git_remote_helpers") as remote,
            mock.patch.object(self.mod, "install_desktop_infra_git_origin_helpers") as origin,
            mock.patch.object(self.mod, "install_desktop_infra_git_run_helpers") as run,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_infra_git_helpers(
                bindings,
                ("normalize_git_remote_for_http", "git_origin_http_url", "run_git", "unknown_helper"),
            )

        remote.assert_called_once_with(bindings, ("normalize_git_remote_for_http",))
        origin.assert_called_once_with(bindings, ("git_origin_http_url",))
        run.assert_called_once_with(bindings, ("run_git",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
