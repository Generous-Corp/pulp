#!/usr/bin/env python3
"""Tests for cloud/GitHub facade bindings."""

import importlib.util
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cloud_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("cloud_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class CloudBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        cloud = types.SimpleNamespace(
            cmd_cloud_workflows=make_runner("cmd_cloud_workflows", 10),
            cmd_cloud_defaults=make_runner("cmd_cloud_defaults", 11),
            cmd_cloud_history=make_runner("cmd_cloud_history", 12),
            cmd_cloud_compare=make_runner("cmd_cloud_compare", 13),
            cmd_cloud_recommend=make_runner("cmd_cloud_recommend", 14),
            cmd_cloud_run=make_runner("cmd_cloud_run", 15),
            cmd_cloud_status=make_runner("cmd_cloud_status", 16),
            cmd_cloud_namespace_doctor=make_runner("cmd_cloud_namespace_doctor", 17),
            cmd_cloud_namespace_setup=make_runner("cmd_cloud_namespace_setup", 18),
            gh_available=make_runner("gh_available", True),
            gh_workflow_dispatch=make_runner("gh_workflow_dispatch", None),
            gh_run_view=make_runner("gh_run_view", {"databaseId": 7}),
            gh_pr_create=make_runner("gh_pr_create", 42),
            gh_pr_comment=make_runner("gh_pr_comment", True),
            gh_pr_merge=make_runner("gh_pr_merge", True),
            gh_pr_list_open=make_runner("gh_pr_list_open", [{"number": 42}]),
            gh_pr_head=make_runner("gh_pr_head", (42, "feature/x", "abc123")),
            list_cloud_records=make_runner("list_cloud_records", [{"dispatch_id": "abc"}]),
            cloud_record_summary=make_runner("cloud_record_summary", "summary"),
            format_ci_comment=make_runner("format_ci_comment", "comment"),
            open_pr_list_lines=make_runner("open_pr_list_lines", ["#42 feature/x"]),
        )
        return {"_cloud": cloud}, calls

    def test_cloud_commands_delegate_to_cloud_module(self):
        bindings, calls = self._bindings()
        args = object()

        cases = [
            ("cmd_cloud_workflows", 10),
            ("cmd_cloud_defaults", 11),
            ("cmd_cloud_history", 12),
            ("cmd_cloud_compare", 13),
            ("cmd_cloud_recommend", 14),
            ("cmd_cloud_run", 15),
            ("cmd_cloud_status", 16),
            ("cmd_cloud_namespace_doctor", 17),
            ("cmd_cloud_namespace_setup", 18),
        ]
        for name, expected in cases:
            with self.subTest(name=name):
                self.assertEqual(getattr(self.mod, name)(bindings, args), expected)

        self.assertEqual([call[0] for call in calls], [name for name, _ in cases])
        for _, call_args, call_kwargs in calls:
            self.assertEqual(call_args, (args,))
            self.assertEqual(call_kwargs, {})

    def test_github_helpers_delegate_to_cloud_module(self):
        bindings, calls = self._bindings()

        self.assertTrue(self.mod.gh_available(bindings))
        self.assertIsNone(self.mod.gh_workflow_dispatch(bindings, "repo", "build.yml", "main", {"k": "v"}))
        self.assertEqual(self.mod.gh_run_view(bindings, "repo", 7), {"databaseId": 7})
        self.assertEqual(self.mod.gh_pr_create(bindings, "feature/x", "main"), 42)
        self.assertTrue(self.mod.gh_pr_comment(bindings, 42, "body"))
        self.assertTrue(self.mod.gh_pr_merge(bindings, 42, "squash"))
        self.assertEqual(self.mod.gh_pr_list_open(bindings), [{"number": 42}])
        self.assertEqual(self.mod.gh_pr_head(bindings, "latest"), (42, "feature/x", "abc123"))

        self.assertEqual([call[0] for call in calls], [
            "gh_available",
            "gh_workflow_dispatch",
            "gh_run_view",
            "gh_pr_create",
            "gh_pr_comment",
            "gh_pr_merge",
            "gh_pr_list_open",
            "gh_pr_head",
        ])
        self.assertEqual(calls[1][1], ("repo", "build.yml", "main", {"k": "v"}))
        self.assertEqual(calls[3][1], ("feature/x", "main"))
        self.assertEqual(calls[5][1], (42, "squash"))

    def test_status_and_formatting_helpers_delegate_to_cloud_module(self):
        bindings, calls = self._bindings()

        self.assertEqual(self.mod.list_cloud_records(bindings, limit=5), [{"dispatch_id": "abc"}])
        self.assertEqual(self.mod.cloud_record_summary(bindings, {"id": 1}, {"cfg": True}), "summary")
        self.assertEqual(self.mod.format_ci_comment(bindings, {"overall": "pass"}), "comment")
        self.assertEqual(self.mod.open_pr_list_lines(bindings, [{"number": 42}]), ["#42 feature/x"])

        self.assertEqual([call[0] for call in calls], [
            "list_cloud_records",
            "cloud_record_summary",
            "format_ci_comment",
            "open_pr_list_lines",
        ])
        self.assertEqual(calls[0][2], {"limit": 5})
        self.assertEqual(calls[1][1], ({"id": 1}, {"cfg": True}))


if __name__ == "__main__":
    unittest.main()
