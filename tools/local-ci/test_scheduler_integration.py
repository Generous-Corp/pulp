#!/usr/bin/env python3
"""Facade-level scheduler integration tests."""

from __future__ import annotations

import io
import json
import pathlib
import tempfile
import unittest
from argparse import Namespace
from contextlib import redirect_stdout
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module(
        "local_ci.py",
        module_name="pulp_local_ci_scheduler_integration",
        add_module_dir=True,
    )


class SchedulerIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_scheduler_submission_and_target_resolution_edges(self) -> None:
        config = {"targets": {}, "defaults": {}}
        self.assertIs(self.mod.config_for_job_execution({"submission": {}}, config), config)

        missing_config = self.root / "missing.json"
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.assertIs(
                self.mod.config_for_job_execution({"submission": {"config_path": str(missing_config)}}, config),
                config,
            )
        self.assertIn("failed to load submission config", buf.getvalue())

        submitted_config = self.root / "submitted.json"
        submitted_config.write_text(json.dumps({"targets": {"mac": {"enabled": True}}, "defaults": {"targets": "mac"}}))
        loaded = self.mod.config_for_job_execution({"submission": {"config_path": str(submitted_config)}}, config)
        self.assertEqual(loaded["defaults"]["targets"], "mac")
        self.assertEqual(loaded["targets"]["mac"]["enabled"], True)

        self.assertEqual(self.mod.submission_target_state({"submission": {"target_hosts": {"mac": "bad"}}}, "mac"), {})
        self.assertEqual(
            self.mod.submission_target_state({"submission": {"target_hosts": {"mac": {"status": "primary-up"}}}}, "mac"),
            {"status": "primary-up"},
        )

        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="live-host") as ensure:
            self.assertEqual(
                self.mod.resolve_ssh_target_execution(
                    {"submission": {"target_hosts": {"ubuntu": {"status": "primary-up", "resolved_host": "u1"}}}},
                    "ubuntu",
                    {"host": "u0", "repo_path": "/repo"},
                    {},
                ),
                ("u1", "/repo"),
            )
            self.assertEqual(
                self.mod.resolve_ssh_target_execution(
                    {"submission": {"target_hosts": {"ubuntu": {"status": "unreachable", "repo_path": "/submitted"}}}},
                    "ubuntu",
                    {"host": "u0", "repo_path": "/repo"},
                    {},
                ),
                (None, "/submitted"),
            )
            self.assertEqual(
                self.mod.resolve_ssh_target_execution(
                    {"submission": {"target_hosts": {"ubuntu": {"status": "utm-fallback-pending", "configured_host": "fallback"}}}},
                    "ubuntu",
                    {"host": "u0", "repo_path": "/repo"},
                    {"ssh_timeout": 3},
                ),
                ("live-host", "/repo"),
            )
            self.assertEqual(ensure.call_args.args[1]["host"], "fallback")
            self.assertEqual(
                self.mod.resolve_ssh_target_execution({}, "ubuntu", {"host": "u0", "repo_path": "/repo"}, {}),
                ("live-host", "/repo"),
            )

    def test_scheduler_builds_unreachable_and_no_target_results(self) -> None:
        config = {
            "targets": {
                "mac": {"enabled": False},
                "ubuntu": {"enabled": True, "host": "ubuntu", "repo_path": "/repo"},
                "windows": {"enabled": True, "host": "win", "repo_path": r"C:\Repo"},
            },
            "defaults": {},
        }
        job = self.mod.make_job("feature/offline", "d" * 40, "normal", ["mac", "ubuntu", "windows"], "run", "full")

        reporters: dict[str, object] = {}
        with mock.patch.object(self.mod, "resolve_ssh_target_execution", return_value=(None, "/repo")):
            tasks = self.mod._build_target_tasks(job, config, progress_factory=lambda name: reporters.setdefault(name, name))

        self.assertEqual([name for name, _fn in tasks], ["ubuntu", "windows"])
        ubuntu_result = tasks[0][1]()
        windows_result = tasks[1][1]()
        self.assertEqual(ubuntu_result["status"], "unreachable")
        self.assertEqual(ubuntu_result["stderr_tail"], "Host unreachable")
        self.assertEqual(windows_result["target"], "windows")
        self.assertEqual(windows_result["exit_code"], -1)
        self.assertEqual(reporters, {})

        no_targets_job = self.mod.make_job("feature/none", "e" * 40, "low", ["mac"], "run", "smoke")
        result = self.mod.process_job(no_targets_job, {"targets": {"mac": {"enabled": False}}, "defaults": {}})
        self.assertEqual(result["overall"], "pass")
        self.assertEqual(result["results"], [])
        self.assertNotIn("validation", result)
        self.assertEqual(result["targets"], ["mac"])
        self.assertIsInstance(result["provenance"], dict)

    def test_scheduler_process_job_and_drain_failure_edges(self) -> None:
        job = self.mod.make_job("feature/error", "f" * 40, "normal", ["mac"], "run", "full")
        failing_task = [("mac", lambda: (_ for _ in ()).throw(RuntimeError("boom")))]
        progress_snapshots = []

        with mock.patch.object(self.mod, "_build_target_tasks", return_value=failing_task), \
             mock.patch.object(self.mod, "update_runner_active_targets", side_effect=lambda _job_id, state: progress_snapshots.append(state)), \
             mock.patch.object(self.mod, "update_job_active_targets"):
            result = self.mod.process_job(job, {"targets": {"mac": {"enabled": True}}, "defaults": {}})

        self.assertEqual(result["overall"], "fail")
        self.assertEqual(result["results"][0]["target"], "mac")
        self.assertEqual(result["results"][0]["status"], "error")
        self.assertIn("boom", result["results"][0]["stderr_tail"])
        self.assertEqual(progress_snapshots[0]["mac"]["phase"], "starting")
        self.assertEqual(progress_snapshots[-1]["mac"]["status"], "error")

        with mock.patch.object(self.mod, "file_lock", side_effect=self.mod.LockBusyError("busy")):
            self.assertEqual(self.mod.drain_pending_jobs({"targets": {}}, blocking=False), (False, False))

        queue = [self.mod.make_job("feature/boom", "a" * 40, "normal", ["mac"], "run", "full")]
        queue[0]["id"] = "job-boom"
        saved_results = []
        with mock.patch.object(self.mod, "claim_next_job", side_effect=[queue[0], None]), \
             mock.patch.object(self.mod, "process_job", side_effect=RuntimeError("explode")), \
             mock.patch.object(self.mod, "save_result", side_effect=lambda result: saved_results.append(result) or (self.root / "result.json")), \
             mock.patch.object(self.mod, "finalize_job") as finalize, \
             mock.patch.object(self.mod, "write_runner_info"), \
             mock.patch.object(self.mod, "clear_runner_info"), \
             mock.patch.object(self.mod, "reclaim_stale_remote_validators"), \
             mock.patch.object(self.mod, "print_result"):
            self.assertEqual(self.mod.drain_pending_jobs({"targets": {}}, blocking=True), (True, True))

        self.assertEqual(saved_results[0]["overall"], "fail")
        self.assertEqual(saved_results[0]["results"][0]["target"], "scheduler")
        self.assertIn("explode", saved_results[0]["results"][0]["stderr_tail"])
        self.assertEqual(finalize.call_args.args[0], "job-boom")

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
            self.assertEqual(
                self.mod.ensure_host_reachable("ubuntu", {"host": "primary", "utm_fallback": fallback}, {}),
                "primary",
            )

        with mock.patch.object(self.mod, "ssh_reachable", return_value=False), \
             mock.patch.object(self.mod, "utmctl_vm_status", return_value=None):
            self.assertIsNone(
                self.mod.ensure_host_reachable("ubuntu", {"host": "primary", "utm_fallback": fallback}, {})
            )
        with mock.patch.object(self.mod, "ssh_reachable", return_value=False), \
             mock.patch.object(self.mod, "utmctl_vm_status", return_value="stopped"), \
             mock.patch.object(self.mod, "utmctl_start", return_value=False):
            self.assertIsNone(
                self.mod.ensure_host_reachable("ubuntu", {"host": "primary", "utm_fallback": fallback}, {})
            )
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

    def test_scheduler_cli_error_paths_report_context(self) -> None:
        with mock.patch.object(self.mod, "load_config", side_effect=FileNotFoundError("missing config")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_drain(Namespace()), 1)
        self.assertIn("missing config", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"targets": {}}), \
             mock.patch.object(self.mod, "drain_pending_jobs", return_value=(False, False)), \
             mock.patch.object(self.mod, "current_runner_info", return_value={"active_job_id": "abc123", "active_branch": "feature/live"}):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_drain(Namespace()), 0)
        self.assertIn("[abc123] feature/live", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", side_effect=ValueError("bad targets")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_enqueue(Namespace(branch=None, sha=None, targets=None, priority=None, smoke=False)), 1)
        self.assertIn("bad targets", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
