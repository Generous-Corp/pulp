#!/usr/bin/env python3
"""Tests for cloud GitHub PR facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cloud_github_pr_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudGithubPrBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_pr_exports_match_wrappers(self):
        expected = (
            "gh_pr_create",
            "gh_pr_comment",
            "gh_pr_merge",
            "gh_pr_list_open",
            "gh_pr_head",
        )

        self.assertEqual(self.mod.CLOUD_GITHUB_PR_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_pr_helpers_delegate_to_cloud_module(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        cloud = types.SimpleNamespace(
            gh_pr_create=make_runner("gh_pr_create", 42),
            gh_pr_comment=make_runner("gh_pr_comment", True),
            gh_pr_merge=make_runner("gh_pr_merge", True),
            gh_pr_list_open=make_runner("gh_pr_list_open", [{"number": 42}]),
            gh_pr_head=make_runner("gh_pr_head", (42, "feature/x", "abc123")),
        )
        bindings = {"_cloud": cloud}

        self.assertEqual(self.mod.gh_pr_create(bindings, "feature/x", "main"), 42)
        self.assertTrue(self.mod.gh_pr_comment(bindings, 42, "body"))
        self.assertTrue(self.mod.gh_pr_merge(bindings, 42, "squash"))
        self.assertEqual(self.mod.gh_pr_list_open(bindings), [{"number": 42}])
        self.assertEqual(self.mod.gh_pr_head(bindings, "latest"), (42, "feature/x", "abc123"))

        self.assertEqual(
            calls,
            [
                ("gh_pr_create", ("feature/x", "main"), {}),
                ("gh_pr_comment", (42, "body"), {}),
                ("gh_pr_merge", (42, "squash"), {}),
                ("gh_pr_list_open", (), {}),
                ("gh_pr_head", ("latest",), {}),
            ],
        )

    def test_install_pr_helpers_wires_named_exports(self):
        calls = []
        cloud = types.SimpleNamespace(gh_pr_head=lambda pr_ref: calls.append(("gh_pr_head", pr_ref)) or (42, "feature/x", "abc123"))
        bindings = {"_cloud": cloud}

        self.mod.install_cloud_github_pr_helpers(bindings, ("gh_pr_head",))

        self.assertEqual(bindings["gh_pr_head"]("latest"), (42, "feature/x", "abc123"))
        self.assertEqual(calls, [("gh_pr_head", "latest")])

    def test_install_cloud_github_pr_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_cloud_github_pr_helper = lambda _bindings: "future"

        self.mod.install_cloud_github_pr_helpers(bindings, ("future_cloud_github_pr_helper",))

        self.assertEqual(bindings["future_cloud_github_pr_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
