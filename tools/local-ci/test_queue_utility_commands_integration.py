#!/usr/bin/env python3
"""Facade-level queue utility command integration tests."""

from __future__ import annotations

import io
import pathlib
import tempfile
import unittest
from argparse import Namespace
from contextlib import nullcontext, redirect_stdout
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_queue_utility_commands_integration",
        add_module_dir=True,
    )


class QueueUtilityCommandsIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

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


if __name__ == "__main__":
    unittest.main()
