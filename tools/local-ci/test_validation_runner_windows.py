#!/usr/bin/env python3
"""No-network tests for Windows SSH validation runner orchestration."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("validation_runner_windows.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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


class ValidationRunnerWindowsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_run_windows_ssh_validation_probes_checkout_and_runs_powershell(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp) / "repo"
            root.mkdir()
            log_path = pathlib.Path(tmp) / "windows.log"
            progress = []
            messages = []
            captured = {}

            def report_progress(**fields) -> None:
                progress.append(fields)

            def sync_bundle(host: str, job: dict, **kwargs) -> tuple[str, str]:
                captured["sync"] = (host, job["id"], kwargs["report_progress"], kwargs["config"])
                return "pulp-ci-job-windows.bundle", "refs/pulp-ci-bundles/job-windows"

            def ensure_repo(host: str, repo_path: str, **kwargs) -> dict:
                captured["repo"] = (host, repo_path, kwargs)
                return {"repo_path": r"C:\Prepared\Pulp"}

            def probe_cmake(host: str, generator: str, platform: str, instance: str) -> tuple[str, str]:
                captured["probe"] = (host, generator, platform, instance)
                return "ARM64", r"C:\VS"

            def windows_script(target_name: str, host: str, effective_repo_path: str, job: dict, **kwargs) -> tuple[str, str]:
                captured["script"] = (target_name, host, effective_repo_path, job["id"], kwargs)
                return "Write-Host ok", "full"

            def run_logged_command(cmd: list[str], **kwargs) -> dict:
                captured["run"] = (cmd, kwargs)
                return {"returncode": 0}

            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win.example.com",
                r"C:\Pulp",
                {"id": "job-windows", "branch": "feature/windows", "sha": "f" * 40},
                "slow",
                "Visual Studio 17 2022",
                "ARM64",
                r"C:\RequestedVS",
                {"targets": {"windows": {}}},
                report_progress,
                root=root,
                prepare_target_log_fn=lambda _job_id, _target_name: log_path,
                sync_job_bundle_to_ssh_host_fn=sync_bundle,
                validation_error_result_fn=error_result,
                ensure_windows_remote_repo_checkout_fn=ensure_repo,
                git_origin_clone_url_fn=lambda repo_root: f"https://example.invalid/{repo_root.name}.git",
                print_fn=messages.append,
                short_sha_fn=lambda sha: sha[:12],
                now_iso_fn=lambda: "2026-06-10T00:00:00Z",
                probe_windows_ssh_cmake_settings_fn=probe_cmake,
                windows_validation_script_fn=windows_script,
                windows_ssh_powershell_command_fn=lambda host: ["ssh", host, "powershell"],
                run_logged_command_fn=run_logged_command,
                validation_result_from_run_fn=result_from_run,
            )

        self.assertEqual(messages, [r"  [windows] Running validation on win.example.com:C:\Prepared\Pulp @ ffffffffffff..."])
        self.assertEqual(captured["sync"], ("win.example.com", "job-windows", report_progress, {"targets": {"windows": {}}}))
        self.assertEqual(captured["repo"][2]["remote_url"], "https://example.invalid/repo.git")
        self.assertEqual(captured["probe"], ("win.example.com", "Visual Studio 17 2022", "ARM64", r"C:\RequestedVS"))
        self.assertEqual(captured["script"][2], r"C:\Prepared\Pulp")
        self.assertEqual(captured["script"][4]["resolved_generator_instance"], r"C:\VS")
        self.assertEqual(captured["run"][0], ["ssh", "win.example.com", "powershell"])
        self.assertEqual(captured["run"][1]["input_text"], "Write-Host ok")
        self.assertEqual(progress[0]["phase"], "connect")
        self.assertEqual(result["target"], "windows")
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["validation"], "full")

    def test_run_windows_ssh_validation_returns_missing_repo_probe_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp) / "repo"
            root.mkdir()
            log_path = pathlib.Path(tmp) / "windows.log"

            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win.example.com",
                r"C:\Pulp",
                {"id": "job-windows-error", "branch": "feature/windows", "sha": "a" * 40},
                root=root,
                prepare_target_log_fn=lambda _job_id, _target_name: log_path,
                sync_job_bundle_to_ssh_host_fn=lambda *_args, **_kwargs: ("bundle", "ref"),
                validation_error_result_fn=error_result,
                ensure_windows_remote_repo_checkout_fn=lambda *_args, **_kwargs: None,
                git_origin_clone_url_fn=lambda _repo_root: "https://example.invalid/repo.git",
                print_fn=lambda _message: None,
                short_sha_fn=lambda sha: sha[:12],
                now_iso_fn=lambda: "2026-06-10T00:00:00Z",
                probe_windows_ssh_cmake_settings_fn=lambda *_args, **_kwargs: self.fail("unexpected cmake probe"),
                windows_validation_script_fn=lambda *_args, **_kwargs: self.fail("unexpected script build"),
                windows_ssh_powershell_command_fn=lambda _host: self.fail("unexpected command build"),
                run_logged_command_fn=lambda *_args, **_kwargs: self.fail("unexpected command run"),
                validation_result_from_run_fn=result_from_run,
            )

        self.assertEqual(result["target"], "windows")
        self.assertEqual(result["status"], "error")
        self.assertIn("no structured payload", result["stderr_tail"])


if __name__ == "__main__":
    unittest.main()
