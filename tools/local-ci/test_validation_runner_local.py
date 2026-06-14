#!/usr/bin/env python3
"""No-network tests for local validation runner orchestration."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("validation_runner_local.py")


def result_from_run(target_name: str, run: dict, *, log_path: pathlib.Path, validation: str, transport_mode: str) -> dict:
    return {
        "target": target_name,
        "status": "pass" if run["returncode"] == 0 else "fail",
        "log_file": str(log_path),
        "validation": validation,
        "transport_mode": transport_mode,
    }


class ValidationRunnerLocalTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_run_local_validation_reports_progress_and_returns_result(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp) / "repo"
            root.mkdir()
            log_path = pathlib.Path(tmp) / "mac.log"
            progress = []
            messages = []
            captured = {}

            def report_progress(**fields) -> None:
                progress.append(fields)

            def local_command(job: dict, exclude_tests: str) -> tuple[list[str], str]:
                captured["command_job"] = job["id"]
                captured["exclude_tests"] = exclude_tests
                return ["validate", job["sha"]], "full"

            def run_logged_command(cmd: list[str], **kwargs) -> dict:
                captured["run"] = (cmd, kwargs)
                return {"returncode": 0}

            result = self.mod.run_local_validation(
                {"id": "job-local", "branch": "feature/local", "sha": "c" * 40},
                "slow",
                report_progress,
                root=root,
                print_fn=messages.append,
                short_sha_fn=lambda sha: sha[:12],
                prepare_target_log_fn=lambda job_id, target_name: captured.setdefault("prepared", (job_id, target_name)) and log_path,
                now_iso_fn=lambda: "2026-06-10T00:00:00Z",
                local_validation_command_fn=local_command,
                run_logged_command_fn=run_logged_command,
                validation_result_from_run_fn=result_from_run,
            )

        self.assertEqual(messages, ["  [mac] Running local validation on feature/local @ cccccccccccc..."])
        self.assertEqual(captured["prepared"], ("job-local", "mac"))
        self.assertEqual(captured["command_job"], "job-local")
        self.assertEqual(captured["exclude_tests"], "slow")
        self.assertEqual(captured["run"][0], ["validate", "c" * 40])
        self.assertEqual(captured["run"][1]["cwd"], root)
        self.assertEqual(captured["run"][1]["timeout"], 3600)
        self.assertEqual(captured["run"][1]["log_path"], log_path)
        self.assertIs(captured["run"][1]["report_progress"], report_progress)
        self.assertEqual(
            progress,
            [
                {
                    "phase": "validate",
                    "log_path": str(log_path),
                    "last_output_at": "2026-06-10T00:00:00Z",
                    "transport_mode": "local",
                }
            ],
        )
        self.assertEqual(result["target"], "mac")
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["validation"], "full")
        self.assertEqual(result["transport_mode"], "local")


if __name__ == "__main__":
    unittest.main()
