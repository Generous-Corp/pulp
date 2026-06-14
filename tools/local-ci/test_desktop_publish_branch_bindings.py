#!/usr/bin/env python3
"""Tests for desktop branch-publish dependency bindings."""

from module_test_utils import load_local_ci_module
from pathlib import Path
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_publish_branch_bindings.py")


class DesktopPublishBranchBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner):
        bindings = {
            "_reporting": types.SimpleNamespace(publish_report_to_branch=runner),
            "ROOT": Path("/repo"),
        }
        for name in [
            "_run_git",
            "_reset_local_worktree",
            "_clear_directory_contents",
            "git_origin_http_url",
        ]:
            bindings[name] = object()
        return bindings

    def test_branch_exports_match_wrappers(self):
        expected = ("publish_report_to_branch",)

        self.assertEqual(self.mod.DESKTOP_PUBLISH_BRANCH_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_publish_report_to_branch_binds_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"mode": "branch"}

        bindings = self._bindings(runner)
        config = {"desktop_automation": {}}
        report = {"output_dir": "/tmp/report"}

        self.assertEqual(self.mod.publish_report_to_branch(bindings, config, report), {"mode": "branch"})
        self.assertEqual(captured["args"], (config, report))
        self.assertIs(captured["kwargs"]["root"], bindings["ROOT"])
        self.assertIs(captured["kwargs"]["run_git_fn"], bindings["_run_git"])
        self.assertIs(captured["kwargs"]["reset_local_worktree_fn"], bindings["_reset_local_worktree"])
        self.assertIs(captured["kwargs"]["clear_directory_contents_fn"], bindings["_clear_directory_contents"])
        self.assertIs(captured["kwargs"]["git_origin_http_url_fn"], bindings["git_origin_http_url"])

if __name__ == "__main__":
    unittest.main()
