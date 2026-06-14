#!/usr/bin/env python3
"""Additional pure-helper coverage for tools/local-ci/local_ci.py."""

from __future__ import annotations

import importlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from argparse import Namespace
from contextlib import nullcontext, redirect_stdout
from datetime import datetime, timezone
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_local_ci_extra", add_module_dir=True)


class LocalCiPureHelperTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        # R2-1 (#2645): cloud helpers moved to cloud.py — patch the cloud module.
        self.cloud = importlib.import_module("cloud")
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_desktop_adapter_defaults_cover_fallbacks(self) -> None:
        self.assertEqual(self.mod.infer_desktop_adapter("mac", {"type": "local"}), "macos-local")
        self.assertEqual(self.mod.infer_desktop_adapter("custom", {"type": "local"}), "local-window")
        self.assertEqual(self.mod.infer_desktop_adapter("custom", {"type": "ssh"}), "remote-session-agent")
        self.assertEqual(self.mod.infer_desktop_adapter("custom", {}), "unknown")
        self.assertEqual(self.mod.default_desktop_bootstrap("custom"), "manual")
        self.assertEqual(self.mod.default_desktop_capability_tier("custom"), "v1")

    def test_probe_uploaded_bundle_size_handles_outputs(self) -> None:
        config = {"targets": {"windows": {"host": "win", "repo_path": r"C:\\Pulp"}}}
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="noise\n4096\n", stderr=""),
        ) as run:
            self.assertEqual(
                self.mod.probe_uploaded_bundle_size("win", "bundle.git", config=config),
                4096,
            )
            self.assertIn("cmd /V:OFF", run.call_args.args[0][-1])

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="not-a-number\n", stderr=""),
        ):
            self.assertIsNone(
                self.mod.probe_uploaded_bundle_size("ubuntu", "bundle.git", config={"targets": {}})
            )

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 1, stdout="", stderr="failed"),
        ):
            self.assertIsNone(
                self.mod.probe_uploaded_bundle_size("ubuntu", "bundle.git", config={"targets": {}})
            )

    def test_wait_for_job_and_ssh_reachability_cover_scheduler_edges(self) -> None:
        with mock.patch.object(self.mod, "load_job", return_value=None), redirect_stdout(io.StringIO()) as buf:
            self.assertEqual(self.mod.wait_for_job("missing", {}), (None, 1))
        self.assertIn("Job not found", buf.getvalue())

        with mock.patch.object(self.mod, "load_job", return_value={"status": "completed"}), \
             redirect_stdout(io.StringIO()) as buf:
            self.assertEqual(self.mod.wait_for_job("done", {}), (None, 1))
        self.assertIn("without a result file", buf.getvalue())

        completed_job = {"status": "completed", "result_file": str(self.root / "result.json")}
        failed_result = {"overall": "fail"}
        with mock.patch.object(self.mod, "load_job", return_value=completed_job), \
             mock.patch.object(self.mod, "load_result", return_value=failed_result):
            self.assertEqual(self.mod.wait_for_job("done", {}), (failed_result, 1))

        queued_job = {"status": "running"}
        passed_job = {"status": "completed", "result_file": str(self.root / "pass.json")}
        passed_result = {"overall": "pass"}
        with mock.patch.object(self.mod, "load_job", side_effect=[queued_job, passed_job]), \
             mock.patch.object(self.mod, "drain_pending_jobs", return_value=(False, False)), \
             mock.patch.object(self.mod, "current_runner_info", return_value={"active_job_id": "abc123", "active_branch": "feature/a"}), \
             mock.patch.object(self.mod, "load_result", return_value=passed_result), \
             mock.patch.object(self.mod.time, "sleep") as sleep, \
             redirect_stdout(io.StringIO()) as buf:
            self.assertEqual(self.mod.wait_for_job("queued", {}), (passed_result, 0))
        self.assertIn("[abc123] feature/a", buf.getvalue())
        sleep.assert_called_once_with(self.mod.WAIT_POLL_SECS)

        with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
            self.assertEqual(self.mod.ensure_host_reachable("ubuntu", {"host": "primary"}, {}), "primary")
        with mock.patch.object(self.mod, "ssh_reachable", side_effect=[False, True]):
            self.assertEqual(
                self.mod.ensure_host_reachable("ubuntu", {"host": "primary", "fallback_host": "fallback"}, {}),
                "fallback",
            )
        with mock.patch.object(self.mod, "ssh_reachable", return_value=False), redirect_stdout(io.StringIO()) as buf:
            self.assertIsNone(self.mod.ensure_host_reachable("ubuntu", {"host": "primary"}, {}))
        self.assertIn("no UTM fallback", buf.getvalue())

        fallback = {"vm_name": "Ubuntu", "boot_wait_secs": 0, "ssh_retry_secs": 10}
        with mock.patch.object(self.mod, "ssh_reachable", side_effect=[False, True]), \
             mock.patch.object(self.mod, "utmctl_vm_status", return_value="stopped"), \
             mock.patch.object(self.mod, "utmctl_start", return_value=True), \
             mock.patch.object(self.mod.time, "sleep"), \
             mock.patch.object(self.mod.time, "time", side_effect=[0, 1]):
            self.assertEqual(self.mod.ensure_host_reachable("ubuntu", {"host": "primary", "utm_fallback": fallback}, {}), "primary")

        with mock.patch.object(self.mod, "ssh_reachable", return_value=False), \
             mock.patch.object(self.mod, "utmctl_vm_status", return_value=None):
            self.assertIsNone(self.mod.ensure_host_reachable("ubuntu", {"host": "primary", "utm_fallback": fallback}, {}))
        with mock.patch.object(self.mod, "ssh_reachable", return_value=False), \
             mock.patch.object(self.mod, "utmctl_vm_status", return_value="stopped"), \
             mock.patch.object(self.mod, "utmctl_start", return_value=False):
            self.assertIsNone(self.mod.ensure_host_reachable("ubuntu", {"host": "primary", "utm_fallback": fallback}, {}))
        with mock.patch.object(self.mod, "ssh_reachable", return_value=False), \
             mock.patch.object(self.mod, "utmctl_vm_status", return_value="started"), \
             mock.patch.object(self.mod.time, "time", return_value=0):
            self.assertIsNone(
                self.mod.ensure_host_reachable(
                    "ubuntu",
                    {"host": "primary", "utm_fallback": {"vm_name": "Ubuntu", "ssh_retry_secs": 0}},
                    {},
                )
            )

    def test_remote_probe_wrappers_parse_mocked_outputs(self) -> None:
        win_success = subprocess.CompletedProcess(
            [],
            0,
            stdout='banner\n{"task_present": true, "interactive_user": "dev"}\n',
            stderr="",
        )
        with mock.patch.object(self.mod, "run_windows_ssh_powershell", return_value=win_success) as run_ps:
            session = self.mod.probe_windows_session_agent(
                "win",
                {
                    "task_name": "Pulp Agent",
                    "remote_root": r"%LOCALAPPDATA%\Pulp\agent",
                    "script_path": r"%LOCALAPPDATA%\Pulp\agent\agent.ps1",
                },
            )
            self.assertTrue(session["task_present"])
            self.assertEqual(session["interactive_user"], "dev")
            self.assertIn("Get-ScheduledTask", run_ps.call_args.args[1])

            tooling = self.mod.probe_windows_remote_tooling("win")
            self.assertTrue(tooling["task_present"])
            self.assertIn("Get-Command git", run_ps.call_args.args[1])

        win_failure = subprocess.CompletedProcess([], 7, stdout="", stderr="powershell failed")
        with mock.patch.object(self.mod, "run_windows_ssh_powershell", return_value=win_failure):
            with self.assertRaisesRegex(RuntimeError, "powershell failed"):
                self.mod.probe_windows_session_agent("win", {"task_name": "task", "remote_root": "root"})
            with self.assertRaisesRegex(RuntimeError, "powershell failed"):
                self.mod.probe_windows_remote_tooling("win")
            with self.assertRaisesRegex(RuntimeError, "powershell failed"):
                self.mod.install_windows_remote_tool("win", "Git.Git", timeout=1)

        linux_success = subprocess.CompletedProcess(
            [],
            0,
            stdout="ignored\nmode=display\ndisplay=:2\nxdg_runtime_dir=/run/user/501\n",
            stderr="",
        )
        with mock.patch.object(self.mod, "ssh_command_result", return_value=linux_success) as ssh_run:
            backend = self.mod.probe_linux_launch_backend("ubuntu")
            self.assertEqual(backend["mode"], "display")
            self.assertEqual(backend["display"], ":2")
            self.assertIn("xvfb-run", ssh_run.call_args.args[1])

        linux_tooling = subprocess.CompletedProcess(
            [],
            0,
            stdout="git_found=true\ngit_path=/usr/bin/git\ngit_version=git version 2.49\nwmctrl_found=false\n",
            stderr="",
        )
        with mock.patch.object(self.mod, "ssh_command_result", return_value=linux_tooling):
            probe = self.mod.probe_linux_remote_tooling("ubuntu")
            self.assertEqual(probe["git_version"], "git version 2.49")
            self.assertEqual(probe["wmctrl_found"], "false")

        linux_failure = subprocess.CompletedProcess([], 2, stdout="", stderr="ssh failed")
        with mock.patch.object(self.mod, "ssh_command_result", return_value=linux_failure):
            with self.assertRaisesRegex(RuntimeError, "ssh failed"):
                self.mod.probe_linux_launch_backend("ubuntu")
            with self.assertRaisesRegex(RuntimeError, "ssh failed"):
                self.mod.probe_linux_remote_tooling("ubuntu")

        command = self.mod.build_linux_window_driver_remote_command(
            "/repo",
            "bundle",
            "pulp-ui",
            launch_backend={"mode": "display", "display": ":2", "xdg_runtime_dir": "/run/user/501"},
            launch_cwd="$HOME/repo",
            click_point="10,20",
            capture_before=True,
            settle_secs=0.25,
        )
        self.assertIn("export DISPLAY=:2", command)
        self.assertIn("xdotool click 1", command)
        self.assertIn("sleep 0.250", command)

    def test_webdriver_probe_parses_status_shapes_and_errors(self) -> None:
        self.assertEqual(self.mod.webdriver_status_url("http://127.0.0.1:4444"), "http://127.0.0.1:4444/status")
        self.assertEqual(self.mod.webdriver_status_url("http://host/wd/hub"), "http://host/wd/hub/status")
        self.assertEqual(self.mod.webdriver_status_url("http://host/status?old=1#frag"), "http://host/status")
        with self.assertRaisesRegex(ValueError, "scheme and host"):
            self.mod.webdriver_status_url("localhost:4444")

        class FakeResponse:
            def __init__(self, payload: str) -> None:
                self.payload = payload

            def __enter__(self):
                return self

            def __exit__(self, *_exc):
                return False

            def read(self) -> bytes:
                return self.payload.encode("utf-8")

        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            return_value=FakeResponse('{"value":{"ready":true,"message":" ok "}}'),
        ) as urlopen:
            probe = self.mod.probe_webdriver_endpoint("http://driver")
            self.assertEqual(probe["status_url"], "http://driver/status")
            self.assertTrue(probe["ready"])
            self.assertEqual(probe["message"], "ok")
            self.assertEqual(urlopen.call_args.kwargs["timeout"], 5.0)

        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            return_value=FakeResponse('{"ready":false,"message":"not ready"}'),
        ):
            probe = self.mod.probe_webdriver_endpoint("http://driver/status", timeout=1.5)
            self.assertFalse(probe["ready"])
            self.assertEqual(probe["message"], "not ready")

        http_error = self.mod.urllib.error.HTTPError(
            "http://driver/status",
            500,
            "boom",
            {},
            mock.Mock(read=lambda: b"server body"),
        )
        with mock.patch.object(self.mod.urllib.request, "urlopen", side_effect=http_error):
            with self.assertRaisesRegex(RuntimeError, "HTTP 500: server body"):
                self.mod.probe_webdriver_endpoint("http://driver")
        with mock.patch.object(self.mod.urllib.request, "urlopen", return_value=FakeResponse("{bad")):
            with self.assertRaisesRegex(RuntimeError, "invalid JSON response"):
                self.mod.probe_webdriver_endpoint("http://driver")
        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            side_effect=self.mod.urllib.error.URLError("connection refused"),
        ):
            with self.assertRaisesRegex(RuntimeError, "connection refused"):
                self.mod.probe_webdriver_endpoint("http://driver")
        with mock.patch.object(self.mod.urllib.request, "urlopen", return_value=FakeResponse("[]")):
            probe = self.mod.probe_webdriver_endpoint("http://driver")
        self.assertIsNone(probe["ready"])
        self.assertEqual(probe["message"], "")
        self.assertEqual(probe["payload"], [])

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

    def test_command_bump_and_cancel_cover_queue_edges(self) -> None:
        pending = {"id": "job1", "branch": "feature/pending", "sha": "a" * 40, "priority": "normal", "targets": ["mac"], "status": "pending"}
        running = {"id": "job2", "branch": "feature/running", "sha": "b" * 40, "priority": "normal", "targets": ["mac"], "status": "running"}

        with mock.patch.object(self.mod, "file_lock", return_value=nullcontext()), \
             mock.patch.object(self.mod, "queue_lock_path", return_value=self.root / "queue.lock"), \
             mock.patch.object(self.mod, "load_queue_unlocked", return_value=[pending]), \
             mock.patch.object(self.mod, "save_queue_unlocked") as save_queue:
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_bump(Namespace(job="job1", priority="high")), 0)

        self.assertEqual(pending["priority"], "high")
        self.assertIn("bumped_at", pending)
        self.assertEqual(save_queue.call_args.args[0][0]["id"], "job1")
        self.assertIn("Updated priority", buf.getvalue())

        with mock.patch.object(self.mod, "normalize_priority", side_effect=ValueError("bad priority")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_bump(Namespace(job="job1", priority="urgent")), 1)
        self.assertIn("bad priority", buf.getvalue())

        with mock.patch.object(self.mod, "file_lock", return_value=nullcontext()), \
             mock.patch.object(self.mod, "load_queue_unlocked", return_value=[]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_bump(Namespace(job="missing", priority="low")), 1)
        self.assertIn("No active job matches", buf.getvalue())

        with mock.patch.object(self.mod, "file_lock", return_value=nullcontext()), \
             mock.patch.object(self.mod, "load_queue_unlocked", return_value=[running]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_bump(Namespace(job="job2", priority="low")), 1)
        self.assertIn("already running", buf.getvalue())

        with mock.patch.object(self.mod, "file_lock", return_value=nullcontext()), \
             mock.patch.object(self.mod, "load_queue_unlocked", return_value=[dict(running)]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_cancel(Namespace(job="job2")), 1)
        self.assertIn("only pending jobs can be canceled safely", buf.getvalue())

        cancel_job = dict(pending, status="pending")
        with mock.patch.object(self.mod, "file_lock", return_value=nullcontext()), \
             mock.patch.object(self.mod, "load_queue_unlocked", return_value=[cancel_job]), \
             mock.patch.object(self.mod, "cancel_job_unlocked") as cancel, \
             mock.patch.object(self.mod, "trim_completed_jobs", side_effect=lambda queue: queue), \
             mock.patch.object(self.mod, "save_queue_unlocked") as save_queue:
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_cancel(Namespace(job="job1")), 0)

        self.assertEqual(cancel.call_args.args[0]["id"], "job1")
        self.assertEqual(save_queue.call_args.args[0][0]["id"], "job1")
        self.assertIn("Canceled:", buf.getvalue())

    def test_command_logs_resolves_jobs_and_prints_outputs(self) -> None:
        completed = {"id": "done1", "branch": "feature/done", "sha": "c" * 40, "status": "completed"}
        running = {"id": "run1", "branch": "feature/run", "sha": "d" * 40, "status": "running"}

        with mock.patch.object(self.mod, "load_queue", return_value=[completed]), \
             mock.patch.object(self.mod, "current_runner_info", return_value=None):
            self.assertEqual(self.mod.resolve_job_for_logs(None), completed)
            self.assertEqual(self.mod.resolve_job_for_logs("done1"), completed)

        with mock.patch.object(self.mod, "load_queue", return_value=[completed, running]), \
             mock.patch.object(self.mod, "current_runner_info", return_value={"active_job_id": "run1"}):
            self.assertEqual(self.mod.resolve_job_for_logs(None), running)

        with mock.patch.object(self.mod, "load_queue", return_value=[]), \
             mock.patch.object(self.mod, "current_runner_info", return_value=None):
            self.assertIsNone(self.mod.resolve_job_for_logs(None))

        with mock.patch.object(self.mod, "resolve_job_for_logs", return_value=None):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_logs(Namespace(job=None, target=None, lines=10)), 1)
        self.assertIn("No matching job logs found", buf.getvalue())

        log_file = self.root / "mac.log"
        log_file.write_text("one\ntwo\n")
        with mock.patch.object(self.mod, "resolve_job_for_logs", return_value=completed), \
             mock.patch.object(self.mod, "target_log_path", return_value=log_file):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_logs(Namespace(job="done1", target="mac", lines=1)), 0)
        self.assertIn("Logs for [done1]", buf.getvalue())
        self.assertIn("== mac ==", buf.getvalue())
        self.assertIn("two", buf.getvalue())

        empty_dir = self.root / "logs-empty"
        empty_dir.mkdir()
        with mock.patch.object(self.mod, "resolve_job_for_logs", return_value=completed), \
             mock.patch.object(self.mod, "job_logs_dir", return_value=empty_dir):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_logs(Namespace(job="done1", target=None, lines=10)), 1)
        self.assertIn("No logs found", buf.getvalue())

    def test_command_evidence_headers_and_empty_results(self) -> None:
        with mock.patch.object(self.mod, "print_evidence_summary", return_value=True) as evidence:
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_evidence(Namespace(branch="feature/evidence", sha=None, limit=3)), 0)
        self.assertEqual(evidence.call_args.kwargs, {"branch": "feature/evidence", "sha": None, "limit": 3})
        self.assertIn("Evidence for branch `feature/evidence`", buf.getvalue())

        with mock.patch.object(self.mod, "current_branch", return_value=""), \
             mock.patch.object(self.mod, "print_evidence_summary", return_value=False):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_evidence(Namespace(branch=None, sha="f" * 40, limit=1)), 1)
        self.assertIn("Evidence for sha `ffffffffffff`", buf.getvalue())
        self.assertIn("(none)", buf.getvalue())

        with mock.patch.object(self.mod, "current_branch", return_value=""), \
             mock.patch.object(self.mod, "print_evidence_summary", return_value=False):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_evidence(Namespace(branch=None, sha=None, limit=1)), 1)
        self.assertIn("No local CI evidence recorded", buf.getvalue())

    def test_command_status_prints_live_target_submission_and_cloud_edges(self) -> None:
        result_path = self.root / "result.json"
        result_path.write_text(json.dumps({
            "overall": "pass",
            "results": [{"target": "mac", "status": "pass"}],
            "provenance": {"submitted_root": str(self.root)},
        }))
        running = {
            "id": "run123",
            "branch": "feature/run",
            "sha": "a" * 40,
            "status": "running",
            "targets": ["mac"],
            "started_at": "2026-01-01T00:00:00Z",
            "submission": {
                "submitted_root": str(self.root),
                "config_path": "local-ci.json",
                "config_source": "repo",
                "provenance": {"submitted_root": str(self.root)},
            },
        }
        pending = {
            "id": "pend456",
            "branch": "feature/pending",
            "sha": "b" * 40,
            "status": "pending",
            "priority": "low",
            "targets": ["ubuntu"],
            "queued_at": "2026-01-01T00:01:00Z",
            "last_progress_at": "2026-01-01T00:02:00Z",
            "active_targets": {
                "ubuntu": {
                    "phase": "bootstrap",
                    "validation_mode": "smoke",
                    "transport_mode": "ssh",
                    "test_policy": "smoke",
                    "prepared_state": "ready",
                    "wait_reason": "queue",
                    "cleanup_status": "done",
                    "last_output_at": "out",
                    "last_heartbeat_at": "beat",
                    "quiet_for_secs": 3,
                    "liveness": "alive",
                    "log_path": str(self.root / "ubuntu.log"),
                    "last_line": "building",
                    "cleanup_result": "removed",
                }
            },
            "submission": {"provenance": {"submitted_root": str(self.root)}},
        }
        completed = {
            "id": "done789",
            "branch": "feature/done",
            "sha": "c" * 40,
            "status": "completed",
            "targets": ["mac"],
            "result_file": str(result_path),
        }
        runner = {
            "pid": 1234,
            "active_job_id": "run123",
            "active_branch": "feature/run",
            "active_targets": {
                "mac": {
                    "phase": "test",
                    "validation_mode": "full",
                    "transport_mode": "local",
                    "last_line": "ok",
                }
            },
        }
        config = {"targets": {"ubuntu": {"type": "ssh", "host": "ubuntu.example"}}, "defaults": {}}
        cloud_settings = {"workflow": "build", "provider": "namespace"}
        with mock.patch.object(self.mod, "load_config", return_value=config), \
             mock.patch.object(self.mod, "state_dir", return_value=self.root / "state"), \
             mock.patch.object(self.mod, "config_path", return_value=self.root / "local-ci.json"), \
             mock.patch.object(self.mod, "load_queue", return_value=[running, pending, completed]), \
             mock.patch.object(self.mod, "current_runner_info", return_value=runner), \
             mock.patch.object(self.mod, "current_branch", return_value="feature/run"), \
             mock.patch.object(self.mod, "print_evidence_summary", return_value=False) as evidence, \
             mock.patch.object(self.mod, "list_cloud_records", side_effect=[[{"id": "cloud1"}], [{"id": "cloud1"}]]), \
             mock.patch.object(self.mod, "load_optional_config", return_value={"github_actions": {}}), \
             mock.patch.object(self.mod, "github_actions_settings_for_display", return_value=cloud_settings), \
             mock.patch.object(self.mod, "resolve_github_actions_settings", side_effect=ValueError("cloud config bad")), \
             mock.patch.object(self.mod, "resolve_default_provider_for_workflow", return_value=("namespace", "config")), \
             mock.patch.object(self.mod, "print_billing_period_summary") as billing, \
             mock.patch.object(self.mod, "cloud_record_summary", return_value="cloud row"), \
             mock.patch.object(self.mod, "print_local_ci_state_footprint") as footprint, \
             mock.patch.object(self.mod, "utmctl_vm_status", return_value="stopped"), \
             mock.patch.object(self.mod, "ssh_reachable", return_value=True):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_status(Namespace()), 0)

        output = buf.getvalue()
        self.assertIn("Runner: pid=1234 active=[run123] feature/run", output)
        self.assertIn("submission: root=", output)
        self.assertIn("mac: phase=test, mode=full, transport=local", output)
        self.assertIn("ubuntu: phase=bootstrap, mode=smoke, transport=ssh, tests=smoke", output)
        self.assertIn("log=ubuntu.log", output)
        self.assertIn("cleanup: removed", output)
        self.assertIn("Cloud defaults: workflow=build provider=namespace", output)
        evidence.assert_called_once_with(branch="feature/run", limit=2, indent="  ")
        self.assertEqual(billing.call_count, 1)
        self.assertEqual(footprint.call_count, 1)

    def test_desktop_config_dispatch_and_error_paths_report_context(self) -> None:
        with mock.patch.object(self.mod, "load_config", side_effect=FileNotFoundError("missing config")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_config_show(Namespace(json=False)), 1)
        self.assertIn("missing config", buf.getvalue())

        desktop_config = {
            "desktop_automation": {
                "artifact_root": "runs",
                "publish_mode": "local",
                "publish_branch": "desktop-artifacts",
                "retention_days": 7,
            }
        }
        with mock.patch.object(self.mod, "load_config", return_value=desktop_config):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_config_show(Namespace(json=False)), 0)
        self.assertIn("Desktop automation config:", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_config_set(Namespace(key="retention_days", value="-1", json=False)), 1)
        self.assertIn("retention_days must be >= 0", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_config_set(Namespace(key="target.mac.bad.field", value="1", json=False)), 1)
        self.assertIn("Target desktop config keys must look like", buf.getvalue())

        with mock.patch.object(self.mod, "cmd_desktop_config_show", return_value=7) as show:
            self.assertEqual(self.mod.cmd_desktop_config(Namespace(desktop_config_command="show")), 7)
        show.assert_called_once()

        buf = io.StringIO()
        with redirect_stdout(buf):
            self.assertEqual(self.mod.cmd_desktop_config(Namespace(desktop_config_command=None)), 1)
        self.assertIn("desktop config subcommand required", buf.getvalue())

    def test_desktop_listing_publish_cleanup_empty_and_error_paths(self) -> None:
        with mock.patch.object(self.mod, "load_config", side_effect=FileNotFoundError("missing desktop config")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_recent(Namespace(target=None, action=None, limit=5, json=False)), 1)
        self.assertIn("missing desktop config", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}), \
             mock.patch.object(self.mod, "desktop_run_manifests", return_value=[]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_recent(Namespace(target=None, action=None, limit=5, json=True)), 0)
        self.assertIn("No desktop automation runs found.", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}), \
             mock.patch.object(self.mod, "desktop_proof_summaries", side_effect=ValueError("bad source")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_proof(Namespace(target=None, action=None, source_mode="bad", sha=None, branch=None, limit=5, json=False)), 1)
        self.assertIn("bad source", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}), \
             mock.patch.object(self.mod, "desktop_proof_summaries", return_value=[]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_proof(Namespace(target="mac", action="smoke", source_mode="live", sha="a" * 40, branch="feature/a", limit=5, json=False)), 0)
        self.assertIn("No desktop proofs found", buf.getvalue())
        self.assertIn("sha=aaaaaaaaaaaa", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}), \
             mock.patch.object(self.mod, "desktop_run_manifests", return_value=[]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_publish(Namespace(target=None, action=None, limit=5, output=None, label=None, json=False)), 0)
        self.assertIn("No desktop automation runs found.", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}), \
             mock.patch.object(self.mod, "desktop_run_manifests", return_value=[{"target": "mac"}]), \
             mock.patch.object(self.mod, "stage_desktop_publish_report", side_effect=RuntimeError("publish failed")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_publish(Namespace(target=None, action=None, limit=5, output=None, label=None, json=False)), 1)
        self.assertIn("publish failed", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {"retention_days": 30}}), \
             mock.patch.object(self.mod, "prune_desktop_run_manifests", return_value=[]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_cleanup(Namespace(target=None, older_than_days=None, keep_last=1, json=False)), 0)
        self.assertIn("Desktop cleanup: nothing to remove.", buf.getvalue())

    def test_desktop_artifact_report_helpers_cover_filter_and_fallback_edges(self) -> None:
        config = {"desktop_automation": {"artifact_root": str(self.root / "artifacts"), "publish_root": str(self.root / "publish")}}
        publish_root = self.mod.desktop_publish_root(config)
        old_dir = publish_root / "2026-01-01T00-00-00Z"
        new_dir = publish_root / "2026-01-02T00-00-00Z"
        malformed_dir = publish_root / "malformed"
        missing_dir = publish_root / "missing-index"
        old_dir.mkdir(parents=True)
        new_dir.mkdir(parents=True)
        malformed_dir.mkdir(parents=True)
        missing_dir.mkdir(parents=True)
        (old_dir / "index.json").write_text(json.dumps({"generated_at": "2026-01-01T00:00:00Z", "label": "old"}))
        (new_dir / "index.json").write_text(json.dumps({"generated_at": "2026-01-02T00:00:00Z", "label": "new"}))
        (malformed_dir / "index.json").write_text("{")

        reports = self.mod.desktop_publish_reports(config)
        self.assertEqual([report["label"] for report in reports], ["new", "old"])
        self.assertEqual(reports[0]["output_dir"], str(new_dir))
        self.assertEqual(reports[0]["index_json"], str(new_dir / "index.json"))
        self.assertEqual(reports[0]["index_html"], str(new_dir / "index.html"))
        self.assertEqual(self.mod.desktop_publish_reports(config, limit=1)[0]["label"], "new")

        self.mod.write_desktop_publish_rollups(config)
        self.assertEqual(json.loads((publish_root / "latest-report.json").read_text())["label"], "new")
        self.assertEqual(len((publish_root / "reports.jsonl").read_text().splitlines()), 2)

        ready = self.root / "ready.txt"
        ready.write_text("ok")
        self.assertEqual(self.mod.wait_for_path(ready, 0.1), ready)
        with self.assertRaisesRegex(RuntimeError, "timed out waiting for artifact"):
            self.mod.wait_for_path(self.root / "missing.txt", 0.0)

        self.assertEqual(self.mod.count_view_tree_nodes("not-a-node"), 0)
        self.assertEqual(self.mod.count_view_tree_nodes({"children": "bad"}), 1)
        self.assertEqual(self.mod.count_view_tree_nodes({"children": [{"children": [{}]}, {}]}), 4)

        app_binary = self.root / "Demo.app" / "Contents" / "MacOS" / "Demo"
        app_binary.parent.mkdir(parents=True)
        app_binary.write_text("")
        self.assertIsNone(self.mod.detect_macos_app_bundle(None))
        self.assertIsNone(self.mod.detect_macos_app_bundle(""))
        self.assertEqual(self.mod.detect_macos_app_bundle(str(app_binary)), self.root / "Demo.app")
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Missing.app"))
        info_plist = self.root / "Demo.app" / "Contents" / "Info.plist"
        info_plist.write_text("not plist")
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"))
        info_plist.write_bytes(self.mod.plistlib.dumps({"CFBundleIdentifier": "com.example.demo"}))
        self.assertEqual(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"), "com.example.demo")
        info_plist.write_bytes(self.mod.plistlib.dumps({"CFBundleIdentifier": ""}))
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"))

    def test_desktop_manifest_and_window_wait_helpers_cover_edge_paths(self) -> None:
        artifact_root = self.root / "artifacts"
        config = {
            "desktop_automation": {
                "artifact_root": str(artifact_root),
                "targets": {"mac": {"adapter": "macos-local"}, "windows": {"adapter": "windows-session-agent"}},
            }
        }
        valid_bundle = artifact_root / "mac" / "smoke" / "run-new"
        old_bundle = artifact_root / "mac" / "smoke" / "run-old"
        malformed_bundle = artifact_root / "mac" / "smoke" / "bad"
        wrong_action = artifact_root / "mac" / "inspect" / "run"
        missing_manifest = artifact_root / "windows" / "smoke" / "run"
        for path in (valid_bundle, old_bundle, malformed_bundle, wrong_action, missing_manifest):
            path.mkdir(parents=True)
        (valid_bundle / "manifest.json").write_text(json.dumps({"target": "mac", "started_at": "2026-01-02T00:00:00Z"}))
        (old_bundle / "manifest.json").write_text(json.dumps({"target": "mac", "completed_at": "2026-01-01T00:00:00Z"}))
        (malformed_bundle / "manifest.json").write_text("{")
        (wrong_action / "manifest.json").write_text(json.dumps({"target": "mac", "started_at": "2026-01-03T00:00:00Z"}))

        manifests = self.mod.desktop_run_manifests(config, target_name="mac", action="smoke")
        self.assertEqual(len(manifests), 2)
        self.assertEqual(manifests[0]["target"], "mac")
        self.assertEqual(manifests[0]["artifacts"]["bundle_dir"], str(valid_bundle))
        self.assertEqual(manifests[1]["artifacts"]["bundle_dir"], str(old_bundle))
        self.assertEqual(self.mod.desktop_run_manifests(config, target_name="missing"), [])
        self.assertEqual(len(self.mod.desktop_run_manifests(config, action="inspect")), 1)

        self.assertEqual(self.mod.normalize_desktop_proof_source_mode(None), "legacy")
        self.assertEqual(self.mod.normalize_desktop_proof_source_mode(" exact_sha "), "exact-sha")
        with self.assertRaisesRegex(ValueError, "Invalid desktop proof source mode"):
            self.mod.normalize_desktop_proof_source_mode("archive")
        self.assertEqual(self.mod.desktop_manifest_adapter(config, {"adapter": "custom"}), "custom")
        self.assertEqual(self.mod.desktop_manifest_adapter(config, {"target": "mac"}), "macos-local")
        self.assertEqual(self.mod.desktop_manifest_adapter(config, {"target": "missing"}), "unknown")
        self.assertEqual(self.mod.desktop_manifest_adapter({"desktop_automation": {"targets": []}}, {"target": "mac"}), "unknown")

        with mock.patch.object(self.mod.time, "time", side_effect=[0.0, 0.0, 1.0]), \
             mock.patch.object(self.mod.time, "sleep") as sleep, \
             mock.patch.object(self.mod, "macos_window_info_for_pid", side_effect=subprocess.SubprocessError("boom")):
            with self.assertRaisesRegex(RuntimeError, "boom"):
                self.mod.wait_for_macos_window(123, 0.5)
        sleep.assert_called_once_with(0.2)

        with mock.patch.object(self.mod.time, "time", side_effect=[0.0, 0.0, 1.0]), \
             mock.patch.object(self.mod.time, "sleep") as sleep, \
             mock.patch.object(self.mod, "macos_window_info_for_bundle_id", return_value={"pid": 456, "windows": []}), \
             mock.patch.object(self.mod, "activate_macos_bundle_id", return_value={"stderr": "still hidden"}) as activate:
            with self.assertRaisesRegex(RuntimeError, "still hidden"):
                self.mod.wait_for_macos_bundle_window("com.example.demo", 0.5)
        activate.assert_called_once_with("com.example.demo")
        sleep.assert_called_once_with(0.2)

        with mock.patch.object(self.mod.time, "time", side_effect=[0.0, 0.0]), \
             mock.patch.object(self.mod, "macos_window_info_for_pid", return_value={"windows": [{"id": 7}]}):
            self.assertEqual(self.mod.wait_for_macos_window(123, 0.5), {"id": 7})
        with mock.patch.object(self.mod.time, "time", side_effect=[0.0, 0.0]), \
             mock.patch.object(self.mod, "macos_window_info_for_bundle_id", return_value={"pid": 456, "windows": [{"id": 8}]}):
            self.assertEqual(self.mod.wait_for_macos_bundle_window("com.example.demo", 0.5), (456, {"id": 8}))

    def test_desktop_action_validation_errors_and_dispatch_guards(self) -> None:
        config = {
            "desktop_automation": {
                "targets": {
                    "mac": {"adapter": "macos-local", "target_type": "local"},
                    "linux": {"adapter": "linux-xvfb", "target_type": "ssh"},
                    "windows": {"adapter": "windows-session-agent", "target_type": "ssh"},
                    "other": {"adapter": "remote-session-agent", "target_type": "ssh"},
                }
            }
        }
        with mock.patch.object(self.mod, "load_config", return_value=config), \
             mock.patch.object(self.mod, "resolve_desktop_target", side_effect=lambda _config, name: config["desktop_automation"]["targets"][name]), \
             mock.patch.object(self.mod, "make_desktop_source_request", return_value={}):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_smoke(Namespace(target="linux", launch_command=None, bundle_id=None, label=None, output=None, capture_ui_snapshot=False, click=None, click_view_id=None, click_view_type=None, click_view_text=None, click_view_label=None, capture_before=False, settle_secs=0.0, timeout=1.0, pulp_app_automation=False)), 1)
            self.assertIn("requires --command for linux-xvfb", buf.getvalue())

            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_smoke(Namespace(target="windows", launch_command="app", bundle_id=None, label=None, output=None, capture_ui_snapshot=True, click=None, click_view_id=None, click_view_type=None, click_view_text=None, click_view_label=None, capture_before=False, settle_secs=0.0, timeout=1.0, pulp_app_automation=False)), 1)
            self.assertIn("supports --capture-ui-snapshot only with --pulp-app-automation", buf.getvalue())

            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_click(Namespace(target="windows", launch_command="app", bundle_id=None, label=None, output=None, capture_ui_snapshot=False, click=None, click_view_id="root", click_view_type=None, click_view_text=None, click_view_label=None, settle_secs=0.0, timeout=1.0, pulp_app_automation=False)), 1)
            self.assertIn("supports view-target selectors only with --pulp-app-automation", buf.getvalue())

            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_inspect(Namespace(target="other", launch_command="app", bundle_id=None, label=None, output=None, timeout=1.0, pulp_app_automation=False)), 1)
            self.assertIn("desktop inspect is not implemented", buf.getvalue())

            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop(Namespace(desktop_command=None)), 1)
            self.assertIn("desktop subcommand required", buf.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
