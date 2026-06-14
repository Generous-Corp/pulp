#!/usr/bin/env python3
"""Tests for cloud/GitHub facade composition."""

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cloud_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_cloud_helper_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.CLOUD_MODULE_ATTR_EXPORTS,
            *self.mod.CLOUD_COMMAND_EXPORTS,
            *self.mod.CLOUD_GITHUB_EXPORTS,
            *self.mod.CLOUD_RECORD_EXPORTS,
        )

        self.assertEqual(self.mod.CLOUD_HELPER_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_cloud_helpers_routes_each_group(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        bindings = {
            "_cloud": types.SimpleNamespace(
                summarize_runner_selector=make_runner("summarize_runner_selector", "linux,arm64"),
                cmd_cloud_status=make_runner("cmd_cloud_status", 16),
                gh_pr_head=make_runner("gh_pr_head", (42, "feature/x", "abc123")),
                format_ci_comment=make_runner("format_ci_comment", "comment"),
            )
        }

        self.mod.install_cloud_helpers(
            bindings,
            (
                "summarize_runner_selector",
                "cmd_cloud_status",
                "gh_pr_head",
                "format_ci_comment",
            ),
        )

        self.assertEqual(bindings["summarize_runner_selector"]("selector"), "linux,arm64")
        self.assertEqual(bindings["cmd_cloud_status"](object()), 16)
        self.assertEqual(bindings["gh_pr_head"]("latest"), (42, "feature/x", "abc123"))
        self.assertEqual(bindings["format_ci_comment"]({"overall": "pass"}), "comment")
        self.assertEqual(
            [call[0] for call in calls],
            ["summarize_runner_selector", "cmd_cloud_status", "gh_pr_head", "format_ci_comment"],
        )


if __name__ == "__main__":
    unittest.main()
