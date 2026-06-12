#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("logs_cli.py")


def load_logs_cli_module():
    return load_module_from_path(MODULE_PATH)


class LogsCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_logs_cli_module()
        self.printed: list[str] = []
        self.completed = {"id": "done1", "branch": "feature/done", "sha": "c" * 40, "status": "completed"}
        self.running = {"id": "run1", "branch": "feature/run", "sha": "d" * 40, "status": "running"}

    def print_line(self, line: str):
        self.printed.append(line)

    def args(self, *, job=None, target=None, lines=80):
        return Namespace(job=job, target=target, lines=lines)

    def run_cmd(
        self,
        args,
        *,
        job=None,
        log_dir=None,
        target_log=None,
        tail_lines=None,
    ):
        return self.mod.cmd_logs(
            args,
            resolve_job_for_logs_fn=lambda _job_ref: job,
            target_log_path_fn=lambda _job_id, _target: target_log,
            job_logs_dir_fn=lambda _job_id: log_dir,
            tail_lines_fn=lambda path, limit: tail_lines(path, limit) if tail_lines else path.read_text().splitlines(keepends=True)[-limit:],
            missing_job_logs_line_fn=lambda: "No matching job logs found.",
            missing_log_files_line_fn=lambda job: f"No logs found for job [{job['id']}] {job['branch']}.",
            job_logs_header_line_fn=lambda job: f"Logs for [{job['id']}] {job['branch']}",
            log_section_header_line_fn=lambda target: f"== {target} ==",
            empty_log_line_fn=lambda: "(empty)",
            print_fn=self.print_line,
        )

    def test_resolve_job_for_logs_uses_queue_runner_and_selector(self):
        calls = []

        def select_job(queue, runner_info, job_ref):
            calls.append((queue, runner_info, job_ref))
            return queue[0]

        result = self.mod.resolve_job_for_logs(
            "done1",
            load_queue_fn=lambda: [self.completed],
            current_runner_info_fn=lambda: {"active_job_id": "run1"},
            select_job_for_logs_fn=select_job,
        )

        self.assertEqual(result, self.completed)
        self.assertEqual(calls, [([self.completed], {"active_job_id": "run1"}, "done1")])

    def test_cmd_logs_reports_selection_errors_and_missing_jobs(self):
        def raises(_job_ref):
            raise ValueError("ambiguous")

        result = self.mod.cmd_logs(
            self.args(job="feature"),
            resolve_job_for_logs_fn=raises,
            target_log_path_fn=lambda _job_id, _target: Path("/tmp/unused"),
            job_logs_dir_fn=lambda _job_id: Path("/tmp/unused"),
            tail_lines_fn=lambda _path, _limit: [],
            missing_job_logs_line_fn=lambda: "No matching job logs found.",
            missing_log_files_line_fn=lambda _job: "no files",
            job_logs_header_line_fn=lambda _job: "header",
            log_section_header_line_fn=lambda _target: "section",
            empty_log_line_fn=lambda: "(empty)",
            print_fn=self.print_line,
        )
        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["Error: ambiguous"])

        self.printed.clear()
        self.assertEqual(self.run_cmd(self.args(), job=None, log_dir=Path("/tmp/unused")), 1)
        self.assertEqual(self.printed, ["No matching job logs found."])

    def test_cmd_logs_tails_single_target_log(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            log_path = Path(tmpdir) / "mac.log"
            log_path.write_text("one\ntwo\nthree\n")

            result = self.run_cmd(self.args(job="done1", target="mac", lines=2), job=self.completed, target_log=log_path)

        self.assertEqual(result, 0)
        output = "\n".join(self.printed)
        self.assertIn("Logs for [done1] feature/done", output)
        self.assertIn("== mac ==", output)
        self.assertIn("two\nthree", self.printed)
        self.assertNotIn("one\ntwo\nthree", self.printed)

    def test_cmd_logs_tails_all_logs_in_sorted_order(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            log_dir = Path(tmpdir)
            (log_dir / "windows.log").write_text("win\n")
            (log_dir / "mac.log").write_text("mac\n")

            result = self.run_cmd(self.args(job="done1", lines=5), job=self.completed, log_dir=log_dir)

        self.assertEqual(result, 0)
        output = "\n".join(self.printed)
        self.assertLess(output.index("== mac =="), output.index("== windows =="))
        self.assertIn("mac", output)
        self.assertIn("win", output)

    def test_cmd_logs_reports_missing_and_empty_log_files(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            self.assertEqual(self.run_cmd(self.args(job="done1"), job=self.completed, log_dir=Path(tmpdir)), 1)
        self.assertEqual(self.printed, ["No logs found for job [done1] feature/done."])

        self.printed.clear()
        with tempfile.TemporaryDirectory() as tmpdir:
            log_path = Path(tmpdir) / "mac.log"
            log_path.write_text("")
            result = self.run_cmd(self.args(job="done1", target="mac"), job=self.completed, target_log=log_path)

        self.assertEqual(result, 0)
        self.assertIn("(empty)", self.printed)


if __name__ == "__main__":
    unittest.main()
