#!/usr/bin/env python3
"""Tests for cloud GitHub facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cloud_github_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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
        calls = []

        def workflow_install(bindings, names):
            calls.append(("workflow", names))

        def pr_install(bindings, names):
            calls.append(("pr", names))

        self.mod.install_cloud_github_workflow_helpers = workflow_install
        self.mod.install_cloud_github_pr_helpers = pr_install

        self.mod.install_cloud_github_helpers({}, ("gh_available", "gh_pr_head"))

        self.assertEqual(
            calls,
            [
                ("workflow", ("gh_available",)),
                ("pr", ("gh_pr_head",)),
            ],
        )

    def test_github_helpers_delegate_to_cloud_module(self):
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
            gh_pr_create=make_runner("gh_pr_create", 42),
            gh_pr_comment=make_runner("gh_pr_comment", True),
            gh_pr_merge=make_runner("gh_pr_merge", True),
            gh_pr_list_open=make_runner("gh_pr_list_open", [{"number": 42}]),
            gh_pr_head=make_runner("gh_pr_head", (42, "feature/x", "abc123")),
        )
        bindings = {"_cloud": cloud}

        self.assertTrue(self.mod.gh_available(bindings))
        self.assertIsNone(self.mod.gh_workflow_dispatch(bindings, "repo", "build.yml", "main", {"k": "v"}))
        self.assertEqual(self.mod.gh_run_view(bindings, "repo", 7), {"databaseId": 7})
        self.assertEqual(self.mod.gh_pr_create(bindings, "feature/x", "main"), 42)
        self.assertTrue(self.mod.gh_pr_comment(bindings, 42, "body"))
        self.assertTrue(self.mod.gh_pr_merge(bindings, 42, "squash"))
        self.assertEqual(self.mod.gh_pr_list_open(bindings), [{"number": 42}])
        self.assertEqual(self.mod.gh_pr_head(bindings, "latest"), (42, "feature/x", "abc123"))

        self.assertEqual(
            [call[0] for call in calls],
            [
                "gh_available",
                "gh_workflow_dispatch",
                "gh_run_view",
                "gh_pr_create",
                "gh_pr_comment",
                "gh_pr_merge",
                "gh_pr_list_open",
                "gh_pr_head",
            ],
        )
        self.assertEqual(calls[1][1], ("repo", "build.yml", "main", {"k": "v"}))
        self.assertEqual(calls[3][1], ("feature/x", "main"))
        self.assertEqual(calls[5][1], (42, "squash"))

    def test_install_cloud_github_helpers_wires_named_exports(self):
        calls = []
        cloud = types.SimpleNamespace(gh_available=lambda: calls.append(("gh_available",)) or True)
        bindings = {"_cloud": cloud}

        self.mod.install_cloud_github_helpers(bindings, ("gh_available",))

        self.assertTrue(bindings["gh_available"]())
        self.assertEqual(calls, [("gh_available",)])

    def test_install_cloud_github_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_cloud_github_helper = lambda _bindings: "future"

        self.mod.install_cloud_github_helpers(bindings, ("future_cloud_github_helper",))

        self.assertEqual(bindings["future_cloud_github_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
