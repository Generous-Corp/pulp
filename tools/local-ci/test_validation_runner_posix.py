#!/usr/bin/env python3
"""No-network tests for POSIX SSH validation runner orchestration."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("validation_runner_posix.py")


def result_from_run(target_name: str, run: dict, *, log_path: pathlib.Path, validation: str, transport_mode: str) -> dict:
    return {
        "target": target_name,
        "status": "pass" if run["returncode"] == 0 else "fail",
        "log_file": str(log_path),
        "validation": validation,
        "transport_mode": transport_mode,
    }


def error_result(target_name: str, message: str, *, log_path: pathlib.Path, transport_mode: str) -> dict:
    return {
        "target": target_name,
        "status": "error",
        "stderr_tail": message,
        "log_file": str(log_path),
        "transport_mode": transport_mode,
    }


class ValidationRunnerPosixTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_run_posix_ssh_validation_syncs_bundle_reports_progress_and_returns_result(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = pathlib.Path(tmp) / "ubuntu.log"
            progress = []
            messages = []
            captured = {}

            def report_progress(**fields) -> None:
                progress.append(fields)

            def sync_bundle(host: str, job: dict, **kwargs) -> tuple[str, str]:
                captured["sync"] = (host, job["id"], kwargs["report_progress"], kwargs["config"])
                return "pulp-ci-job-posix.bundle", "refs/pulp-ci-bundles/job-posix"

            def posix_command(target_name: str, host: str, repo_path: str, job: dict, **kwargs) -> tuple[list[str], str]:
                captured["command"] = (target_name, host, repo_path, job["id"], kwargs)
                return ["ssh", host, "validate"], "smoke"

            def run_logged_command(cmd: list[str], **kwargs) -> dict:
                captured["run"] = (cmd, kwargs)
                return {"returncode": 0}

            result = self.mod.run_posix_ssh_validation(
                "ubuntu",
                "ubuntu.example.com",
                "/tmp/pulp",
                {"id": "job-posix", "branch": "feature/posix", "sha": "d" * 40},
                "slow",
                {"ssh": {"ubuntu": {}}},
                report_progress,
                print_fn=messages.append,
                short_sha_fn=lambda sha: sha[:12],
                prepare_target_log_fn=lambda _job_id, _target_name: log_path,
                now_iso_fn=lambda: "2026-06-10T00:00:00Z",
                sync_job_bundle_to_ssh_host_fn=sync_bundle,
                posix_ssh_validation_command_fn=posix_command,
                run_logged_command_fn=run_logged_command,
                validation_result_from_run_fn=result_from_run,
                validation_error_result_fn=error_result,
            )

        self.assertEqual(messages, ["  [ubuntu] Running validation on ubuntu.example.com:/tmp/pulp @ dddddddddddd..."])
        self.assertEqual(captured["sync"], ("ubuntu.example.com", "job-posix", report_progress, {"ssh": {"ubuntu": {}}}))
        self.assertEqual(captured["command"][4]["bundle_name"], "pulp-ci-job-posix.bundle")
        self.assertEqual(captured["command"][4]["bundle_ref"], "refs/pulp-ci-bundles/job-posix")
        self.assertEqual(captured["command"][4]["exclude_tests"], "slow")
        self.assertEqual(captured["run"][0], ["ssh", "ubuntu.example.com", "validate"])
        self.assertEqual(captured["run"][1]["timeout"], 3600)
        self.assertEqual(captured["run"][1]["log_path"], log_path)
        self.assertIs(captured["run"][1]["report_progress"], report_progress)
        self.assertEqual(progress[0]["phase"], "connect")
        self.assertEqual(progress[0]["transport_mode"], "bundle")
        self.assertEqual(result["target"], "ubuntu")
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["validation"], "smoke")

    def test_run_posix_ssh_validation_returns_bundle_sync_error_result(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = pathlib.Path(tmp) / "ubuntu.log"
            result = self.mod.run_posix_ssh_validation(
                "ubuntu",
                "ubuntu.example.com",
                "/tmp/pulp",
                {"id": "job-posix-error", "branch": "feature/posix", "sha": "e" * 40},
                print_fn=lambda _message: None,
                short_sha_fn=lambda sha: sha[:12],
                prepare_target_log_fn=lambda _job_id, _target_name: log_path,
                now_iso_fn=lambda: "2026-06-10T00:00:00Z",
                sync_job_bundle_to_ssh_host_fn=lambda *_args, **_kwargs: (_ for _ in ()).throw(RuntimeError("upload failed")),
                posix_ssh_validation_command_fn=lambda *_args, **_kwargs: self.fail("unexpected command build"),
                run_logged_command_fn=lambda *_args, **_kwargs: self.fail("unexpected command run"),
                validation_result_from_run_fn=result_from_run,
                validation_error_result_fn=error_result,
            )

        self.assertEqual(result["target"], "ubuntu")
        self.assertEqual(result["status"], "error")
        self.assertEqual(result["stderr_tail"], "upload failed")
        self.assertEqual(result["log_file"], str(log_path))


if __name__ == "__main__":
    unittest.main()
