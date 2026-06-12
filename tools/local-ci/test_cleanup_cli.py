#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("cleanup_cli.py")


def load_cleanup_cli_module():
    return load_module_from_path(MODULE_PATH)


class CleanupCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_cleanup_cli_module()
        self.printed: list[str] = []
        self.footprint_calls: list[str] = []
        self.plan_calls: list[tuple[dict, bool]] = []
        self.collect_calls: list[dict] = []

    def print_line(self, line: str):
        self.printed.append(line)

    def print_cleanup_plan(self, plan: dict, *, dry_run: bool):
        self.plan_calls.append((plan, dry_run))
        self.printed.append("cleanup plan")

    def print_footprint(self, *, indent: str = ""):
        self.footprint_calls.append(indent)
        self.printed.append(f"{indent}footprint")

    def collect_plan(self, queue, *, keep_results, keep_logs, keep_bundles, include_prepared):
        self.collect_calls.append(
            {
                "queue": queue,
                "keep_results": keep_results,
                "keep_logs": keep_logs,
                "keep_bundles": keep_bundles,
                "include_prepared": include_prepared,
            }
        )
        return {"categories": {}, "total_paths": 0}

    def args(self, *, apply=False, include_prepared=False):
        return Namespace(
            apply=apply,
            include_prepared=include_prepared,
            keep_results=1,
            keep_logs=2,
            keep_bundles=3,
        )

    def run_cmd(self, args, *, queue=None, apply_result=None):
        return self.mod.cmd_cleanup(
            args,
            load_queue_fn=lambda: list(queue or []),
            collect_cleanup_plan_fn=self.collect_plan,
            apply_cleanup_plan_fn=lambda _plan: apply_result or {"removed": [], "removed_bytes": 0, "failed": []},
            print_cleanup_plan_fn=self.print_cleanup_plan,
            print_state_footprint_fn=self.print_footprint,
            format_size_fn=lambda value: f"{value} B",
            describe_path_fn=lambda path: f"desc:{path.name}",
            print_fn=self.print_line,
        )

    def test_print_helpers_delegate_to_line_formatters(self):
        self.mod.print_local_ci_state_footprint(
            local_ci_state_footprint_fn=lambda: {"total_bytes": 5},
            state_footprint_lines_fn=lambda footprint, *, indent: [f"{indent}{footprint['total_bytes']}"],
            indent="  ",
            print_fn=self.print_line,
        )
        self.mod.print_local_ci_cleanup_plan(
            {"total_paths": 1},
            dry_run=True,
            cleanup_plan_lines_fn=lambda plan, *, dry_run: [f"{plan['total_paths']} dry={dry_run}"],
            print_fn=self.print_line,
        )

        self.assertEqual(self.printed, ["  5", "1 dry=True"])

    def test_cmd_cleanup_dry_run_reports_plan_footprint_and_prepared_note(self):
        result = self.run_cmd(self.args(include_prepared=True), queue=[{"id": "done", "status": "completed"}])

        self.assertEqual(result, 0)
        self.assertEqual(self.collect_calls[0]["keep_results"], 1)
        self.assertEqual(self.collect_calls[0]["keep_logs"], 2)
        self.assertEqual(self.collect_calls[0]["keep_bundles"], 3)
        self.assertTrue(self.collect_calls[0]["include_prepared"])
        self.assertEqual(self.plan_calls[0][1], True)
        self.assertEqual(self.footprint_calls, ["  "])
        self.assertTrue(any("prepared cleanup removes cached build/install state" in line for line in self.printed))

    def test_cmd_cleanup_apply_refuses_running_jobs(self):
        result = self.run_cmd(self.args(apply=True), queue=[{"id": "run", "status": "running"}])

        self.assertEqual(result, 1)
        self.assertEqual(self.collect_calls, [])
        self.assertIn("blocked while local CI jobs are running", self.printed[0])

    def test_cmd_cleanup_apply_reports_success(self):
        result = self.run_cmd(
            self.args(apply=True, include_prepared=True),
            apply_result={"removed": ["/tmp/a", "/tmp/b"], "removed_bytes": 42, "failed": []},
        )

        self.assertEqual(result, 0)
        self.assertIn("removed: 2 path(s), 42 B", "\n".join(self.printed))
        self.assertEqual(self.plan_calls[0][1], False)
        self.assertEqual(self.footprint_calls, ["  "])
        self.assertTrue(any("prepared cleanup removes cached build/install state" in line for line in self.printed))

    def test_cmd_cleanup_apply_reports_failures(self):
        result = self.run_cmd(
            self.args(apply=True),
            apply_result={
                "removed": [],
                "removed_bytes": 0,
                "failed": [{"path": "/tmp/bad", "error": "denied"}],
            },
        )

        self.assertEqual(result, 1)
        output = "\n".join(self.printed)
        self.assertIn("failed: 1 path(s)", output)
        self.assertIn("desc:bad: denied", output)
        self.assertEqual(self.footprint_calls, [])


if __name__ == "__main__":
    unittest.main()
