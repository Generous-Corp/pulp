#!/usr/bin/env python3
"""Facade-level run, ship, check, and list command integration tests."""

from __future__ import annotations

import io
import pathlib
import subprocess
import tempfile
import unittest
from argparse import Namespace
from contextlib import redirect_stdout
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module(
        "local_ci.py",
        module_name="pulp_local_ci_command_workflows_integration",
        add_module_dir=True,
    )


class CommandWorkflowsIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_command_run_handles_failover_local_and_error_paths(self) -> None:
        args = Namespace(branch="feature/run", sha=None, targets=None, priority=None, smoke=False)
        config = {"github_actions": {"repository": "owner/repo"}}
        submission = {"namespace_failover_targets": ["windows"]}
        job = {"id": "job-run", "branch": "feature/run", "sha": "a" * 40, "priority": "normal", "targets": ["mac"]}
        result = {"job_id": "job-run", "branch": "feature/run", "results": [], "overall": "pass"}

        with mock.patch.object(
            self.mod,
            "resolve_submission_options",
            return_value=(config, "feature/run", "a" * 40, ["mac", "windows"], "normal", "full", submission),
        ), mock.patch.object(self.mod, "print_submission_metadata") as print_meta, \
             mock.patch.object(self.mod, "gh_workflow_dispatch") as dispatch, \
             mock.patch.object(self.mod, "enqueue_job", return_value=(job, True)) as enqueue, \
             mock.patch.object(self.mod, "wait_for_job", return_value=(result, 0)), \
             mock.patch.object(self.mod, "load_job", return_value={"result_file": str(self.root / "result.json")}), \
             mock.patch.object(self.mod, "print_result") as print_result, \
             mock.patch.object(self.mod, "notify") as notify:
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_run(args), 0)

        self.assertIn("Namespace failover", buf.getvalue())
        self.assertEqual(dispatch.call_args.args, ("owner/repo", "build.yml", "feature/run", {"runner_provider": "namespace"}))
        self.assertEqual(enqueue.call_args.args[3], ["mac"])
        self.assertEqual(print_meta.call_count, 1)
        self.assertEqual(print_result.call_args.args[0], result)
        self.assertIn("PASSED", notify.call_args.args[0])

        with mock.patch.object(
            self.mod,
            "resolve_submission_options",
            return_value=(config, "feature/run", "a" * 40, ["windows"], "normal", "full", submission),
        ), mock.patch.object(self.mod, "print_submission_metadata"), \
             mock.patch.object(self.mod, "gh_workflow_dispatch", side_effect=RuntimeError("no quota")), \
             mock.patch.object(self.mod, "enqueue_job") as enqueue, \
             mock.patch.object(self.mod, "notify") as notify:
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_run(args), 0)

        self.assertIn("Namespace dispatch failed", buf.getvalue())
        self.assertIn("no local work", buf.getvalue())
        enqueue.assert_not_called()
        self.assertIn("PASSED", notify.call_args.args[0])

        with mock.patch.object(self.mod, "resolve_submission_options", side_effect=ValueError("bad run")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_run(args), 1)
        self.assertIn("bad run", buf.getvalue())

    def test_command_ship_covers_guards_and_result_outcomes(self) -> None:
        args = Namespace(base="main")
        config = {"targets": {}}
        submission = {
            "branch": "feature/ship",
            "sha": "b" * 40,
            "priority": "normal",
            "targets": ["mac"],
            "validation": "full",
            "target_hosts": {},
            "submitted_root": str(self.root),
            "cwd": str(self.root),
            "config_path": str(self.root / "local-ci.json"),
            "config_source": "test",
        }
        resolved = (config, "feature/ship", "b" * 40, ["mac"], "normal", "full", submission)

        with mock.patch.object(
            self.mod,
            "resolve_submission_options",
            return_value=(config, "feature/ship", "b" * 40, ["mac"], "normal", "smoke", submission),
        ):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_ship(args), 1)
        self.assertIn("ship only supports full validation", buf.getvalue())

        with mock.patch.object(
            self.mod,
            "resolve_submission_options",
            return_value=(config, "main", "b" * 40, ["mac"], "normal", "full", submission),
        ):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_ship(args), 1)
        self.assertIn("cannot ship main to itself", buf.getvalue())

        with mock.patch.object(self.mod, "resolve_submission_options", return_value=resolved), \
             mock.patch.object(self.mod, "gh_available", return_value=False):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_ship(args), 1)
        self.assertIn("gh CLI not available", buf.getvalue())

        with mock.patch.object(self.mod, "resolve_submission_options", return_value=resolved), \
             mock.patch.object(self.mod, "gh_available", return_value=True), \
             mock.patch.object(self.mod.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, stderr="denied")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_ship(args), 1)
        self.assertIn("Push failed: denied", buf.getvalue())

        pass_result = {"overall": "pass", "results": [{"target": "mac", "status": "pass"}]}
        fail_result = {"overall": "fail", "results": [{"target": "mac", "status": "fail"}]}
        job = {"id": "job-ship", "branch": "feature/ship", "sha": "b" * 40, "priority": "normal", "targets": ["mac"]}
        with mock.patch.object(self.mod, "resolve_submission_options", return_value=resolved), \
             mock.patch.object(self.mod, "gh_available", return_value=True), \
             mock.patch.object(self.mod.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, stderr="")), \
             mock.patch.object(self.mod, "gh_pr_create", return_value=123), \
             mock.patch.object(self.mod, "enqueue_job", return_value=(job, True)), \
             mock.patch.object(self.mod, "wait_for_job", return_value=(pass_result, 0)), \
             mock.patch.object(self.mod, "gh_pr_comment") as comment, \
             mock.patch.object(self.mod, "gh_pr_merge", return_value=True) as merge, \
             mock.patch.object(self.mod, "notify") as notify:
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_ship(args), 0)
        self.assertEqual(comment.call_args.args[0], 123)
        self.assertEqual(merge.call_args.args[0], 123)
        self.assertIn("shipped to main", notify.call_args.args[0])

        with mock.patch.object(self.mod, "resolve_submission_options", return_value=resolved), \
             mock.patch.object(self.mod, "gh_available", return_value=True), \
             mock.patch.object(self.mod.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, stderr="")), \
             mock.patch.object(self.mod, "gh_pr_create", return_value=124), \
             mock.patch.object(self.mod, "enqueue_job", return_value=(job, True)), \
             mock.patch.object(self.mod, "wait_for_job", return_value=(fail_result, 7)), \
             mock.patch.object(self.mod, "gh_pr_comment"), \
             mock.patch.object(self.mod, "notify") as notify:
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_ship(args), 7)
        self.assertIn("CI failed", buf.getvalue())
        self.assertIn("CI failed", notify.call_args.args[0])

    def test_command_check_and_list_cover_github_cli_edges(self) -> None:
        check_args = Namespace(pr=42, targets=None, priority=None, smoke=True, allow_root_mismatch=True, allow_unreachable_targets=False)
        with mock.patch.object(self.mod, "gh_available", return_value=False):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_check(check_args), 1)
        self.assertIn("gh CLI not available", buf.getvalue())

        with mock.patch.object(self.mod, "gh_available", return_value=True), \
             mock.patch.object(self.mod, "gh_pr_head", return_value=None):
            self.assertEqual(self.mod.cmd_check(check_args), 1)

        config = {"targets": {"mac": {"enabled": True}}, "defaults": {}}
        result = {"overall": "fail", "results": [{"target": "mac", "status": "fail"}]}
        with mock.patch.object(self.mod, "gh_available", return_value=True), \
             mock.patch.object(self.mod, "gh_pr_head", return_value=(42, "feature/check", "c" * 40)), \
             mock.patch.object(self.mod, "load_config", return_value=config), \
             mock.patch.object(self.mod, "resolve_targets", return_value=["mac"]), \
             mock.patch.object(self.mod, "default_priority_for", return_value="low"), \
             mock.patch.object(self.mod, "build_submission_metadata", return_value={"target_hosts": {}}), \
             mock.patch.object(self.mod, "print_submission_metadata") as print_meta, \
             mock.patch.object(
                 self.mod,
                 "enqueue_job",
                 return_value=({"id": "job-check", "branch": "feature/check", "sha": "c" * 40, "priority": "low", "targets": ["mac"]}, True),
             ), \
             mock.patch.object(self.mod, "wait_for_job", return_value=(result, 5)), \
             mock.patch.object(self.mod, "gh_pr_comment") as comment, \
             mock.patch.object(self.mod, "notify") as notify:
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_check(check_args), 5)

        self.assertIn("PR #42 -> branch: feature/check", buf.getvalue())
        self.assertEqual(print_meta.call_count, 1)
        self.assertEqual(comment.call_args.args[0], 42)
        self.assertIn("FAILED", notify.call_args.args[0])

        with mock.patch.object(self.mod, "gh_available", return_value=False):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_list(Namespace()), 1)
        self.assertIn("gh CLI not available", buf.getvalue())

        with mock.patch.object(self.mod, "gh_available", return_value=True), \
             mock.patch.object(self.mod, "gh_pr_list_open", return_value=[]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_list(Namespace()), 0)
        self.assertIn("No open PRs", buf.getvalue())

        prs = [{"number": 7, "title": "T", "headRefName": "feature/t", "author": {"login": "dev"}, "labels": [{"name": "ci"}]}]
        with mock.patch.object(self.mod, "gh_available", return_value=True), \
             mock.patch.object(self.mod, "gh_pr_list_open", return_value=prs):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_list(Namespace()), 0)
        self.assertIn("Open PRs (1)", buf.getvalue())
        self.assertIn("#   7  T", buf.getvalue())
        self.assertIn("feature/t by dev [ci]", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
