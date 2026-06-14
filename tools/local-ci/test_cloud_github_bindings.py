#!/usr/bin/env python3
"""Tests for cloud GitHub facade bindings."""

import unittest
from unittest import mock

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("cloud_github_bindings.py")


class CloudGithubBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_github_exports_match_wrappers(self):
        expected = (
            *self.mod.CLOUD_GITHUB_WORKFLOW_EXPORTS,
            *self.mod.CLOUD_GITHUB_PR_EXPORTS,
        )

        self.assertEqual(self.mod.CLOUD_GITHUB_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_cloud_github_helpers_routes_focused_groups(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_cloud_github_workflow_helpers") as install_workflow,
            mock.patch.object(self.mod, "install_cloud_github_pr_helpers") as install_pr,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_cloud_github_helpers(bindings, ("gh_available", "gh_pr_head", "custom_github_export"))

        install_workflow.assert_called_once_with(bindings, ("gh_available",))
        install_pr.assert_called_once_with(bindings, ("gh_pr_head",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_github_export",))


if __name__ == "__main__":
    unittest.main()
