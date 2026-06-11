#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from argparse import Namespace
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).resolve().with_name("queue_commands_cli.py")


def load_queue_commands_cli_module():
    spec = importlib.util.spec_from_file_location("queue_commands_cli_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class QueueCommandsCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_queue_commands_cli_module()
        self.printed: list[str] = []

    def print_line(self, line: str):
        self.printed.append(line)

    def test_cmd_bump_normalizes_priority_calls_job_and_prints_result_line(self):
        calls = []

        def bump_job(job_ref, priority):
            calls.append((job_ref, priority))
            return {"status": "updated", "summary": "summary"}

        result = self.mod.cmd_bump(
            Namespace(job="job1", priority="HIGH"),
            normalize_priority_fn=lambda value: value.lower(),
            bump_queue_command_job_fn=bump_job,
            bump_queue_command_result_line_fn=lambda payload, job_ref: (0, f"Updated priority: {payload['summary']} ({job_ref})"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        self.assertEqual(calls, [("job1", "high")])
        self.assertEqual(self.printed, ["Updated priority: summary (job1)"])

    def test_cmd_bump_reports_priority_and_job_errors(self):
        result = self.mod.cmd_bump(
            Namespace(job="job1", priority="urgent"),
            normalize_priority_fn=lambda _value: (_ for _ in ()).throw(ValueError("bad priority")),
            bump_queue_command_job_fn=lambda _job_ref, _priority: {"status": "updated"},
            bump_queue_command_result_line_fn=lambda _payload, _job_ref: (0, "unused"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["Error: bad priority"])

        self.printed.clear()
        result = self.mod.cmd_bump(
            Namespace(job="job1", priority="low"),
            normalize_priority_fn=lambda value: value,
            bump_queue_command_job_fn=lambda _job_ref, _priority: (_ for _ in ()).throw(ValueError("ambiguous job")),
            bump_queue_command_result_line_fn=lambda _payload, _job_ref: (0, "unused"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["Error: ambiguous job"])

    def test_cmd_bump_returns_result_line_exit_code_for_missing_or_not_pending(self):
        for payload, expected in [
            ({"status": "missing"}, "No active job matches 'job1'."),
            ({"status": "not_pending", "job_status": "running"}, "Job is already running; only pending jobs can be reprioritized."),
        ]:
            self.printed.clear()
            result = self.mod.cmd_bump(
                Namespace(job="job1", priority="low"),
                normalize_priority_fn=lambda value: value,
                bump_queue_command_job_fn=lambda _job_ref, _priority, payload=payload: payload,
                bump_queue_command_result_line_fn=lambda _payload, _job_ref, expected=expected: (1, expected),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 1)
            self.assertEqual(self.printed, [expected])

    def test_cmd_cancel_calls_job_and_prints_result_line(self):
        calls = []

        def cancel_job(job_ref):
            calls.append(job_ref)
            return {"status": "canceled", "summary": "summary"}

        result = self.mod.cmd_cancel(
            Namespace(job="job1"),
            cancel_queue_command_job_fn=cancel_job,
            cancel_queue_command_result_line_fn=lambda payload, job_ref: (0, f"Canceled: {payload['summary']} ({job_ref})"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        self.assertEqual(calls, ["job1"])
        self.assertEqual(self.printed, ["Canceled: summary (job1)"])

    def test_cmd_cancel_reports_job_errors_and_result_line_failures(self):
        result = self.mod.cmd_cancel(
            Namespace(job="job1"),
            cancel_queue_command_job_fn=lambda _job_ref: (_ for _ in ()).throw(ValueError("ambiguous job")),
            cancel_queue_command_result_line_fn=lambda _payload, _job_ref: (0, "unused"),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["Error: ambiguous job"])

        self.printed.clear()
        result = self.mod.cmd_cancel(
            Namespace(job="job1"),
            cancel_queue_command_job_fn=lambda _job_ref: {"status": "not_pending", "job_status": "running"},
            cancel_queue_command_result_line_fn=lambda _payload, _job_ref: (
                1,
                "Job is already running; only pending jobs can be canceled safely.",
            ),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["Job is already running; only pending jobs can be canceled safely."])


if __name__ == "__main__":
    unittest.main()
