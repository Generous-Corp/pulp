#!/usr/bin/env python3
from __future__ import annotations

import io
import tempfile
from argparse import Namespace
from contextlib import redirect_stdout
from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module



def load_local_ci_commands_cli_module():
    return load_local_ci_module("local_ci_commands_cli.py")


class LocalCiCommandsCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_local_ci_commands_cli_module()
        self.printed: list[str] = []

    def print_line(self, *values):
        if not values:
            self.printed.append("")
        else:
            self.printed.append(" ".join(str(value) for value in values))

    def test_resolve_submission_options_uses_current_branch_sha_defaults_and_builds_metadata(self):
        calls = []

        def build_metadata(config, branch, sha, targets, priority, validation, **kwargs):
            calls.append((config, branch, sha, targets, priority, validation, kwargs))
            return {"branch": branch, "sha": sha, "targets": targets}

        result = self.mod.resolve_submission_options(
            Namespace(
                branch=None,
                sha=None,
                targets="mac,ubuntu",
                priority=None,
                smoke=True,
                allow_root_mismatch=True,
                allow_unreachable_targets=False,
            ),
            "run",
            load_config_fn=lambda: {"defaults": {"priority": "normal"}},
            current_branch_fn=lambda: "feature/current",
            resolve_git_ref_sha_fn=lambda _branch: "unused",
            current_sha_fn=lambda: "a" * 40,
            resolve_targets_fn=lambda _config, targets: targets,
            parse_targets_arg_fn=lambda value: value.split(","),
            normalize_priority_fn=lambda value: value,
            default_priority_for_fn=lambda command, _config: f"{command}-priority",
            normalize_validation_mode_fn=lambda value: value,
            build_submission_metadata_fn=build_metadata,
        )

        config, branch, sha, targets, priority, validation, submission = result
        self.assertEqual(config, {"defaults": {"priority": "normal"}})
        self.assertEqual(branch, "feature/current")
        self.assertEqual(sha, "a" * 40)
        self.assertEqual(targets, ["mac", "ubuntu"])
        self.assertEqual(priority, "run-priority")
        self.assertEqual(validation, "smoke")
        self.assertEqual(submission, {"branch": "feature/current", "sha": "a" * 40, "targets": ["mac", "ubuntu"]})
        self.assertEqual(calls[0][6]["allow_root_mismatch"], True)
        self.assertEqual(calls[0][6]["allow_unreachable_targets"], False)

    def test_cmd_enqueue_prints_metadata_and_result_line(self):
        calls = []

        result = self.mod.cmd_enqueue(
            Namespace(),
            resolve_submission_options_fn=lambda _args, command: (
                {"config": True},
                "feature/a",
                "a" * 40,
                ["mac"],
                "normal",
                "full",
                {"submission": True, "command": command},
            ),
            print_submission_metadata_fn=lambda metadata: calls.append(("metadata", metadata)),
            enqueue_job_fn=lambda branch, sha, priority, targets, mode, validation, *, submission: (
                {
                    "id": "job-1",
                    "branch": branch,
                    "sha": sha,
                    "priority": priority,
                    "targets": targets,
                    "mode": mode,
                    "validation": validation,
                    "submission": submission,
                },
                True,
            ),
            enqueue_command_result_line_fn=lambda job, *, created: f"queued {job['id']} created={created}",
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        self.assertEqual(calls, [("metadata", {"submission": True, "command": "enqueue"})])
        self.assertEqual(self.printed, ["queued job-1 created=True"])

    def test_cmd_drain_reports_active_runner_without_notify(self):
        notify_calls = []

        result = self.mod.cmd_drain(
            Namespace(),
            load_config_fn=lambda: {"config": True},
            drain_pending_jobs_fn=lambda _config, *, blocking: (False, False),
            current_runner_info_fn=lambda: {"pid": 123},
            drain_runner_active_line_fn=lambda runner: f"active {runner['pid']}",
            notify_fn=notify_calls.append,
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        self.assertEqual(self.printed, ["active 123"])
        self.assertEqual(notify_calls, [])

    def test_cmd_run_dispatches_namespace_failover_and_runs_remaining_targets(self):
        dispatches = []
        enqueues = []
        printed_results = []
        notifications = []

        def enqueue(branch, sha, priority, targets, mode, validation, *, submission):
            enqueues.append((branch, sha, priority, targets, mode, validation, submission))
            return {"id": "job-1"}, True

        result = self.mod.cmd_run(
            Namespace(),
            resolve_submission_options_fn=lambda _args, command: (
                {"github_actions": {"repository": "owner/repo"}},
                "feature/a",
                "a" * 40,
                ["mac", "ubuntu"],
                "normal",
                "full",
                {"namespace_failover_targets": ["ubuntu"], "command": command},
            ),
            print_submission_metadata_fn=lambda metadata: self.print_line(f"metadata {metadata['command']}"),
            gh_workflow_dispatch_fn=lambda repository, workflow, branch, inputs: dispatches.append(
                (repository, workflow, branch, inputs)
            ),
            enqueue_job_fn=enqueue,
            enqueue_command_result_line_fn=lambda job, *, created: f"queued {job['id']} created={created}",
            wait_for_job_fn=lambda job_id, _config: ({"job_id": job_id, "overall": "pass"}, 0),
            load_job_fn=lambda _job_id: {"result_file": "/tmp/result.json"},
            print_result_fn=lambda result, path: printed_results.append((result, str(path))),
            notify_fn=notifications.append,
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        self.assertEqual(dispatches, [("owner/repo", "build.yml", "feature/a", {"runner_provider": "namespace"})])
        self.assertEqual(enqueues[0][3], ["mac"])
        self.assertEqual(printed_results, [({"job_id": "job-1", "overall": "pass"}, "/tmp/result.json")])
        self.assertIn("queued job-1 created=True", self.printed)
        self.assertEqual(notifications, ["CI run complete - PASSED"])

    def test_cmd_status_renders_queue_cloud_footprint_and_vm_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            result_file = Path(tmp) / "result.json"
            result_file.write_text("{}\n")
            running = [{"id": "run-1", "targets": ["mac"], "started_at": "start"}]
            pending = [{"id": "pend-1", "targets": ["mac"], "queued_at": "queue", "last_progress_at": "later"}]
            completed = [{"id": "done-1", "result_file": str(result_file)}]
            cloud_records = [{"id": "cloud-1"}]

            result = self.mod.cmd_status(
                Namespace(),
                load_config_fn=lambda: {
                    "targets": {
                        "mac": {"type": "local"},
                        "ubuntu": {"type": "ssh", "host": "ubuntu"},
                    }
                },
                load_queue_fn=lambda: [],
                queue_status_groups_fn=lambda _queue: (pending, running, completed),
                current_runner_info_fn=lambda: {"pid": 123},
                state_dir_fn=lambda: Path("/state"),
                config_path_fn=lambda: Path("/config.json"),
                status_runner_line_fn=lambda runner: f"Runner {runner['pid']}",
                summarize_job_fn=lambda job: f"job {job['id']}",
                status_submission_lines_fn=lambda job: [f"submission {job['id']}"],
                status_active_targets_fn=lambda *_args: {"mac": {"status": "running"}},
                summarize_active_targets_fn=lambda active, _targets: "mac:running" if active else "",
                status_target_detail_lines_fn=lambda _job, _active: ["target detail"],
                recent_completed_jobs_for_status_fn=lambda jobs: jobs,
                load_result_fn=lambda path: {"path": str(path)},
                recent_completed_status_line_fn=lambda job, result: f"recent {job['id']} {result['path']}",
                recent_completed_missing_result_line_fn=lambda job: f"missing {job['id']}",
                current_branch_fn=lambda: "feature/a",
                print_evidence_summary_fn=lambda **_kwargs: False,
                list_cloud_records_fn=lambda **kwargs: cloud_records if kwargs["limit"] == 5 else cloud_records,
                load_optional_config_fn=lambda: {"github_actions": {}},
                github_actions_settings_for_display_fn=lambda _config: {"workflow": "build", "provider": "display"},
                resolve_github_actions_settings_fn=lambda _config: {"workflow": "build", "provider": "resolved"},
                resolve_default_provider_for_workflow_fn=lambda _settings, _workflow: ("namespace", "config"),
                print_billing_period_summary_fn=lambda _totals, *, indent: self.print_line(f"{indent}billing"),
                estimate_billing_period_totals_fn=lambda _records, _config: {"total": 1},
                cloud_record_summary_fn=lambda record, _config: f"cloud {record['id']}",
                print_state_footprint_fn=lambda *, indent: self.print_line(f"{indent}footprint"),
                utmctl_vm_status_fn=lambda name: "running" if name == "Windows" else None,
                ssh_reachable_fn=lambda host, timeout: host == "ubuntu" and timeout == 3,
                print_fn=self.print_line,
            )

        self.assertEqual(result, 0)
        output = "\n".join(self.printed)
        self.assertIn("State: /state", output)
        self.assertIn("Running (1):", output)
        self.assertIn("Pending (1):", output)
        self.assertIn("Recent (1):", output)
        self.assertIn("Evidence (feature/a):", output)
        self.assertIn("Cloud defaults: workflow=build provider=namespace", output)
        self.assertIn("  cloud cloud-1", output)
        self.assertIn("  footprint", output)
        self.assertIn("  Ubuntu 24.04 desktop: not found", output)
        self.assertIn("  Windows: running", output)
        self.assertIn("  ssh ubuntu: up", output)

    def test_cmd_status_includes_recent_cloud_summary(self):
        cloud_records = [
            {
                "dispatch_id": "abc123def456",
                "workflow_key": "docs-check",
                "requested_ref": "feature/cloud",
                "provider_requested": "namespace",
                "status": "completed",
                "conclusion": "success",
                "run_id": 98765,
                "dispatched_at": "2026-04-04T12:00:00+00:00",
                "updated_at": "2026-04-04T12:01:00+00:00",
            }
        ]

        result = self._cmd_status_cloud_case(
            list_cloud_records_fn=lambda **kwargs: cloud_records,
            cloud_record_summary_fn=lambda record, _config: f"{record['workflow_key']} gha#{record['run_id']}",
        )

        self.assertEqual(result, 0)
        output = "\n".join(self.printed)
        self.assertIn("Cloud defaults: workflow=build provider=github-hosted", output)
        self.assertIn("Cloud (latest 5 known to this machine):", output)
        self.assertIn("docs-check", output)
        self.assertIn("gha#98765", output)

    def test_cmd_status_handles_invalid_cloud_defaults_config(self):
        result = self._cmd_status_cloud_case(
            resolve_github_actions_settings_fn=lambda _config: (_ for _ in ()).throw(
                ValueError("github_actions.defaults.wait_poll_secs must be an integer.")
            )
        )

        self.assertEqual(result, 0)
        output = "\n".join(self.printed)
        self.assertIn("Cloud defaults: workflow=build provider=github-hosted", output)
        self.assertIn("note: github_actions.defaults.wait_poll_secs must be an integer.", output)

    def test_cmd_status_period_cost_uses_full_cloud_history(self):
        cloud_records = [{"id": f"hist{index}"} for index in range(6)]

        def list_cloud_records(**kwargs):
            return cloud_records[:5] if kwargs["limit"] == 5 else cloud_records

        result = self._cmd_status_cloud_case(
            list_cloud_records_fn=list_cloud_records,
            estimate_billing_period_totals_fn=lambda records, _config: {
                "status": "estimated",
                "estimated_total": 3.0,
                "currency": "USD",
                "runs": len(records),
                "reason": "estimated; verify provider pricing",
            },
            cloud_record_summary_fn=lambda record, _config: f"cloud {record['id']}",
        )

        self.assertEqual(result, 0)
        output = "\n".join(self.printed)
        self.assertIn("period cost: est $3.00 over 6 run(s); estimated; verify provider pricing", output)
        self.assertIn("Cloud (latest 5 known to this machine):", output)

    def _cmd_status_cloud_case(self, **overrides):
        def print_billing_period_summary(totals, *, indent):
            total = totals.get("estimated_total")
            if total is None:
                self.print_line(f"{indent}period cost: unavailable")
            else:
                self.print_line(
                    f"{indent}period cost: est ${total:.2f} over {totals.get('runs', 0)} run(s); {totals.get('reason')}"
                )

        kwargs = {
            "load_config_fn": lambda: {"targets": {"ubuntu": {"type": "ssh", "host": "ubuntu"}}},
            "load_queue_fn": lambda: [],
            "queue_status_groups_fn": lambda _queue: ([], [], []),
            "current_runner_info_fn": lambda: None,
            "state_dir_fn": lambda: Path("/state"),
            "config_path_fn": lambda: Path("/config.json"),
            "status_runner_line_fn": lambda _runner: "Runner: idle",
            "summarize_job_fn": lambda job: f"job {job['id']}",
            "status_submission_lines_fn": lambda _job: [],
            "status_active_targets_fn": lambda *_args: None,
            "summarize_active_targets_fn": lambda _active, _targets: "",
            "status_target_detail_lines_fn": lambda _job, _active: [],
            "recent_completed_jobs_for_status_fn": lambda jobs: jobs,
            "load_result_fn": lambda _path: {},
            "recent_completed_status_line_fn": lambda job, _result: f"recent {job['id']}",
            "recent_completed_missing_result_line_fn": lambda job: f"missing {job['id']}",
            "current_branch_fn": lambda: "feature/cloud",
            "print_evidence_summary_fn": lambda **_kwargs: False,
            "list_cloud_records_fn": lambda **_kwargs: [],
            "load_optional_config_fn": lambda: {"github_actions": {}},
            "github_actions_settings_for_display_fn": lambda _config: {
                "workflow": "build",
                "provider": "github-hosted",
            },
            "resolve_github_actions_settings_fn": lambda _config: {
                "workflow": "build",
                "provider": "github-hosted",
            },
            "resolve_default_provider_for_workflow_fn": lambda _settings, _workflow: ("github-hosted", "default"),
            "print_billing_period_summary_fn": print_billing_period_summary,
            "estimate_billing_period_totals_fn": lambda _records, _config: {"estimated_total": None},
            "cloud_record_summary_fn": lambda record, _config: f"cloud {record.get('id', '?')}",
            "print_state_footprint_fn": lambda *, indent: self.print_line(f"{indent}footprint"),
            "utmctl_vm_status_fn": lambda _name: "stopped",
            "ssh_reachable_fn": lambda _host, _timeout: True,
            "print_fn": self.print_line,
        }
        kwargs.update(overrides)

        buf = io.StringIO()
        with redirect_stdout(buf):
            return self.mod.cmd_status(Namespace(), **kwargs)


if __name__ == "__main__":
    unittest.main()
