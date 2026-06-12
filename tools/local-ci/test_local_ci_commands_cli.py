#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import tempfile
from argparse import Namespace
from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("local_ci_commands_cli.py")


def load_local_ci_commands_cli_module():
    return load_module_from_path(MODULE_PATH)


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

    def test_cmd_ship_rejects_smoke_validation_before_github_or_push(self):
        result = self.mod.cmd_ship(
            Namespace(base="main"),
            resolve_submission_options_fn=lambda _args, _command: (
                {},
                "feature/a",
                "a" * 40,
                ["mac"],
                "normal",
                "smoke",
                {},
            ),
            gh_available_fn=lambda: (_ for _ in ()).throw(AssertionError("should not check gh")),
            print_submission_metadata_fn=lambda _metadata: None,
            root=Path("/tmp"),
            run_fn=lambda *args, **kwargs: (_ for _ in ()).throw(AssertionError("should not push")),
            gh_pr_create_fn=lambda _branch, _base: None,
            enqueue_job_fn=lambda *args, **kwargs: ({}, False),
            summarize_job_fn=lambda _job: "summary",
            wait_for_job_fn=lambda _job_id, _config: (None, 1),
            gh_pr_comment_fn=lambda _pr_number, _comment: None,
            format_ci_comment_fn=lambda _result: "comment",
            gh_pr_merge_fn=lambda _pr_number: False,
            notify_fn=lambda _message: None,
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertEqual(
            self.printed,
            ["Error: ship only supports full validation. Use `run --smoke` or `check --smoke` for preflight."],
        )

    def test_cmd_check_queues_pr_and_comments_result(self):
        comments = []
        notifications = []

        result = self.mod.cmd_check(
            Namespace(pr="latest", targets="mac", priority=None, smoke=False, allow_root_mismatch=False, allow_unreachable_targets=True),
            gh_available_fn=lambda: True,
            gh_pr_head_fn=lambda pr_ref: (42, f"feature/{pr_ref}", "b" * 40),
            short_sha_fn=lambda sha: sha[:7],
            load_config_fn=lambda: {"targets": {}},
            resolve_targets_fn=lambda _config, targets: targets,
            parse_targets_arg_fn=lambda value: value.split(","),
            normalize_priority_fn=lambda value: value,
            default_priority_for_fn=lambda command, _config: f"{command}-priority",
            normalize_validation_mode_fn=lambda value: value,
            build_submission_metadata_fn=lambda *_args, **_kwargs: {"submission": True},
            print_submission_metadata_fn=lambda metadata: self.print_line(f"metadata {metadata['submission']}"),
            enqueue_job_fn=lambda branch, sha, priority, targets, mode, validation, *, submission: (
                {
                    "id": "job-42",
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
            summarize_job_fn=lambda job: f"{job['id']} {job['branch']}",
            wait_for_job_fn=lambda _job_id, _config: ({"overall": "pass"}, 0),
            gh_pr_comment_fn=lambda pr_number, comment: comments.append((pr_number, comment)),
            format_ci_comment_fn=lambda result: f"overall={result['overall']}",
            notify_fn=notifications.append,
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        self.assertEqual(comments, [(42, "overall=pass")])
        self.assertIn("  Queueing CI: job-42 feature/latest", self.printed)
        self.assertEqual(notifications, ["CI check complete - PASSED"])

    def test_cmd_list_requires_gh_and_prints_open_pr_lines(self):
        missing_result = self.mod.cmd_list(
            Namespace(),
            gh_available_fn=lambda: False,
            gh_pr_list_open_fn=lambda: [],
            open_pr_list_lines_fn=lambda _prs: [],
            print_fn=self.print_line,
        )

        self.assertEqual(missing_result, 1)
        self.assertEqual(self.printed, ["Error: gh CLI not available. Run: gh auth login"])

        self.printed.clear()
        result = self.mod.cmd_list(
            Namespace(),
            gh_available_fn=lambda: True,
            gh_pr_list_open_fn=lambda: [{"number": 1}],
            open_pr_list_lines_fn=lambda _prs: ["#1 title"],
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        self.assertEqual(self.printed, ["#1 title"])

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


if __name__ == "__main__":
    unittest.main()
