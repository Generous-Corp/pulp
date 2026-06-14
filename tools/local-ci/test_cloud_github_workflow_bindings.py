#!/usr/bin/env python3
"""Tests for cloud GitHub workflow facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cloud_github_workflow_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudGithubWorkflowBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_workflow_exports_match_wrappers(self):
        expected = (
            "gh_available",
            "gh_workflow_dispatch",
            "gh_run_view",
        )

        self.assertEqual(self.mod.CLOUD_GITHUB_WORKFLOW_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_workflow_helpers_delegate_to_cloud_module(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        cloud = types.SimpleNamespace(
            gh_available=make_runner("gh_available", True),
            gh_workflow_dispatch=make_runner("gh_workflow_dispatch", None),
            gh_run_view=make_runner("gh_run_view", {"databaseId": 7}),
        )
        bindings = {"_cloud": cloud}

        self.assertTrue(self.mod.gh_available(bindings))
        self.assertIsNone(self.mod.gh_workflow_dispatch(bindings, "repo", "build.yml", "main", {"k": "v"}))
        self.assertEqual(self.mod.gh_run_view(bindings, "repo", 7), {"databaseId": 7})

        self.assertEqual(
            calls,
            [
                ("gh_available", (), {}),
                ("gh_workflow_dispatch", ("repo", "build.yml", "main", {"k": "v"}), {}),
                ("gh_run_view", ("repo", 7), {}),
            ],
        )

    def test_install_workflow_helpers_wires_named_exports(self):
        calls = []
        cloud = types.SimpleNamespace(gh_available=lambda: calls.append(("gh_available",)) or True)
        bindings = {"_cloud": cloud}

        self.mod.install_cloud_github_workflow_helpers(bindings, ("gh_available",))

        self.assertTrue(bindings["gh_available"]())
        self.assertEqual(calls, [("gh_available",)])


if __name__ == "__main__":
    unittest.main()
