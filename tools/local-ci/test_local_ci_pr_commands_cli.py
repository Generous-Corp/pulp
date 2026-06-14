#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module



def load_local_ci_pr_commands_cli_module():
    return load_local_ci_module("local_ci_pr_commands_cli.py")


class LocalCiPrCommandsCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_local_ci_pr_commands_cli_module()
        self.printed: list[str] = []

    def print_line(self, *values):
        if not values:
            self.printed.append("")
        else:
            self.printed.append(" ".join(str(value) for value in values))

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
            Namespace(
                pr="latest",
                targets="mac",
                priority=None,
                smoke=False,
                allow_root_mismatch=False,
                allow_unreachable_targets=True,
            ),
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


if __name__ == "__main__":
    unittest.main()
