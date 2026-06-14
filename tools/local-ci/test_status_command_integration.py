#!/usr/bin/env python3
"""Status command integration tests."""

from __future__ import annotations

import io
import json
import pathlib
import tempfile
import unittest
from argparse import Namespace
from contextlib import redirect_stdout
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_status_command_integration",
        add_module_dir=True,
    )


class StatusCommandIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

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


if __name__ == "__main__":
    unittest.main()
