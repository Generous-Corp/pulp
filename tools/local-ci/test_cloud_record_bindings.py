#!/usr/bin/env python3
"""Tests for cloud record facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("cloud_record_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudRecordBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_record_exports_match_wrappers(self):
        expected = (
            "list_cloud_records",
            "cloud_record_summary",
            "format_ci_comment",
            "open_pr_list_lines",
        )

        self.assertEqual(self.mod.CLOUD_RECORD_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_status_and_formatting_helpers_delegate_to_cloud_module(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        cloud = types.SimpleNamespace(
            list_cloud_records=make_runner("list_cloud_records", [{"dispatch_id": "abc"}]),
            cloud_record_summary=make_runner("cloud_record_summary", "summary"),
            format_ci_comment=make_runner("format_ci_comment", "comment"),
            open_pr_list_lines=make_runner("open_pr_list_lines", ["#42 feature/x"]),
        )
        bindings = {"_cloud": cloud}

        self.assertEqual(self.mod.list_cloud_records(bindings, limit=5), [{"dispatch_id": "abc"}])
        self.assertEqual(self.mod.cloud_record_summary(bindings, {"id": 1}, {"cfg": True}), "summary")
        self.assertEqual(self.mod.format_ci_comment(bindings, {"overall": "pass"}), "comment")
        self.assertEqual(self.mod.open_pr_list_lines(bindings, [{"number": 42}]), ["#42 feature/x"])

        self.assertEqual(
            [call[0] for call in calls],
            ["list_cloud_records", "cloud_record_summary", "format_ci_comment", "open_pr_list_lines"],
        )
        self.assertEqual(calls[0][2], {"limit": 5})
        self.assertEqual(calls[1][1], ({"id": 1}, {"cfg": True}))

    def test_install_cloud_record_helpers_wires_named_exports(self):
        calls = []
        cloud = types.SimpleNamespace(list_cloud_records=lambda limit=None: calls.append(("list_cloud_records", limit)) or [])
        bindings = {"_cloud": cloud}

        self.mod.install_cloud_record_helpers(bindings, ("list_cloud_records",))

        self.assertEqual(bindings["list_cloud_records"](limit=2), [])
        self.assertEqual(calls, [("list_cloud_records", 2)])

    def test_install_cloud_record_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_cloud_record_helper = lambda _bindings: "future"

        self.mod.install_cloud_record_helpers(bindings, ("future_cloud_record_helper",))

        self.assertEqual(bindings["future_cloud_record_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
