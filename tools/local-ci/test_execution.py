#!/usr/bin/env python3
"""Tests for local CI command execution helpers."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution.py")
MODULE_DIR = MODULE_PATH.parent


def load_module():
    sys.path.insert(0, str(MODULE_DIR))
    try:
        spec = importlib.util.spec_from_file_location("pulp_local_ci_execution", MODULE_PATH)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.pop(0)


class ExecutionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_parse_progress_marker_detects_progress_contract(self) -> None:
        self.assertEqual(self.mod.parse_progress_marker("__PULP_PHASE__:build\n"), {"phase": "build"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_WAIT__:host-lock\n"), {"wait_reason": "host-lock"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_VALIDATION__:smoke\n"), {"validation_mode": "smoke"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_TEST_POLICY__:skip\n"), {"test_policy": "skip"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_PREPARED__:reused\n"), {"prepared_state": "reused"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_VALIDATOR_PID__:4321\n"), {"validator_pid": 4321})
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_VALIDATOR_STARTED__:2026-04-02T04:00:00+00:00\n"),
            {"validator_started_at": "2026-04-02T04:00:00+00:00"},
        )
        self.assertEqual(self.mod.parse_progress_marker("normal output\n"), {})

    def test_validation_helper_policy_matches_contract(self) -> None:
        job = {"sha": "abcdef1234567890", "targets": ["mac"]}
        self.assertEqual(
            self.mod.remote_commit_error("ubuntu", "ubuntu.local", job),
            (
                "ubuntu cannot validate abcdef123456 on ubuntu.local: "
                "commit is not available on origin. Push the branch first or use --targets mac."
            ),
        )
        self.assertEqual(
            self.mod.prepared_state_root("mac", " SMOKE "),
            self.mod.state_dir() / "prepared" / "mac" / "smoke",
        )
        self.assertTrue(self.mod.should_reuse_prepared_state({"targets": ["mac"]}))
        self.assertFalse(self.mod.should_reuse_prepared_state({"targets": ["mac", "ubuntu"]}))

    def test_unreachable_target_result_matches_queue_contract(self) -> None:
        self.assertEqual(
            self.mod.unreachable_target_result("ubuntu"),
            {
                "target": "ubuntu",
                "status": "unreachable",
                "exit_code": -1,
                "duration_secs": 0,
                "stdout_tail": "",
                "stderr_tail": "Host unreachable",
            },
        )
        self.assertEqual(
            self.mod.unreachable_target_result("windows", "VM unavailable")["stderr_tail"],
            "VM unavailable",
        )

    def test_target_exception_result_matches_queue_contract(self) -> None:
        result = self.mod.target_exception_result("windows", RuntimeError("boom"))

        self.assertEqual(
            result,
            {
                "target": "windows",
                "status": "error",
                "exit_code": -1,
                "duration_secs": 0,
                "stdout_tail": "",
                "stderr_tail": "boom",
            },
        )

    def test_completed_job_result_preserves_empty_job_contract(self) -> None:
        job = {
            "id": "job-empty",
            "branch": "feature/empty",
            "sha": "1" * 40,
            "priority": "low",
            "targets": [],
            "queued_at": "2026-04-01T00:00:00+00:00",
            "validation": "smoke",
            "submission": {"target_hosts": {}},
        }

        result = self.mod.completed_job_result(
            job,
            [],
            completed_at="2026-04-01T00:01:00+00:00",
            provenance={"kind": "direct"},
        )

        self.assertEqual(result["job_id"], "job-empty")
        self.assertEqual(result["results"], [])
        self.assertEqual(result["overall"], "pass")
        self.assertNotIn("validation", result)
        self.assertEqual(result["submission"], {"target_hosts": {}})
        self.assertEqual(result["provenance"], {"kind": "direct"})

    def test_completed_job_result_summarizes_target_results(self) -> None:
        job = {
            "id": "job-results",
            "branch": "feature/results",
            "sha": "2" * 40,
            "priority": "normal",
            "targets": ["mac", "windows"],
            "validation": "smoke",
        }

        passing = self.mod.completed_job_result(
            job,
            [{"target": "mac", "status": "pass"}, {"target": "windows", "status": "pass"}],
            completed_at="2026-04-01T00:02:00+00:00",
            provenance={},
        )
        failing = self.mod.completed_job_result(
            job,
            [{"target": "mac", "status": "pass"}, {"target": "windows", "status": "fail"}],
            completed_at="2026-04-01T00:03:00+00:00",
            provenance={},
        )

        self.assertEqual(passing["validation"], "smoke")
        self.assertEqual(passing["overall"], "pass")
        self.assertEqual(failing["overall"], "fail")

    def test_sorted_target_results_orders_by_target(self) -> None:
        results = [
            {"target": "windows", "status": "pass"},
            {"target": "mac", "status": "pass"},
            {"target": "ubuntu", "status": "fail"},
        ]

        sorted_results = self.mod.sorted_target_results(results)

        self.assertEqual([item["target"] for item in sorted_results], ["mac", "ubuntu", "windows"])
        self.assertEqual([item["target"] for item in results], ["windows", "mac", "ubuntu"])

    def test_config_for_job_execution_uses_submission_config_when_available(self) -> None:
        fallback = {"targets": {}, "defaults": {}}
        loaded = {"targets": {"mac": {"enabled": True}}, "defaults": {"targets": "mac"}}
        warnings = []

        self.assertIs(
            self.mod.config_for_job_execution(
                {"submission": {}},
                fallback,
                load_config_file_fn=lambda path: self.fail(f"unexpected load {path}"),
                warn_fn=warnings.append,
            ),
            fallback,
        )
        self.assertEqual(
            self.mod.config_for_job_execution(
                {"submission": {"config_path": "submitted.json"}},
                fallback,
                load_config_file_fn=lambda path: loaded,
                warn_fn=warnings.append,
            ),
            loaded,
        )

        result = self.mod.config_for_job_execution(
            {"submission": {"config_path": "missing.json"}},
            fallback,
            load_config_file_fn=lambda path: (_ for _ in ()).throw(FileNotFoundError(path)),
            warn_fn=warnings.append,
        )

        self.assertIs(result, fallback)
        self.assertIn("failed to load submission config missing.json", warnings[-1])

        invalid_json_result = self.mod.config_for_job_execution(
            {"submission": {"config_path": "invalid.json"}},
            fallback,
            load_config_file_fn=lambda path: (_ for _ in ()).throw(json.JSONDecodeError("bad", "{}", 1)),
            warn_fn=warnings.append,
        )

        self.assertIs(invalid_json_result, fallback)
        self.assertIn("failed to load submission config invalid.json", warnings[-1])

    def test_submission_target_state_and_ssh_execution_resolution(self) -> None:
        self.assertEqual(self.mod.submission_target_state({"submission": {"target_hosts": {"mac": "bad"}}}, "mac"), {})
        self.assertEqual(
            self.mod.submission_target_state(
                {"submission": {"target_hosts": {"mac": {"status": "primary-up"}}}},
                "mac",
            ),
            {"status": "primary-up"},
        )

        ensure_calls = []

        def ensure(target_name: str, target_cfg: dict, defaults: dict) -> str:
            ensure_calls.append((target_name, dict(target_cfg), dict(defaults)))
            return "live-host"

        self.assertEqual(
            self.mod.resolve_ssh_target_execution(
                {"submission": {"target_hosts": {"ubuntu": {"status": "primary-up", "resolved_host": "u1"}}}},
                "ubuntu",
                {"host": "u0", "repo_path": "/repo"},
                {},
                ensure_host_reachable_fn=ensure,
            ),
            ("u1", "/repo"),
        )
        self.assertEqual(ensure_calls, [])

        self.assertEqual(
            self.mod.resolve_ssh_target_execution(
                {"submission": {"target_hosts": {"ubuntu": {"status": "unreachable", "repo_path": "/submitted"}}}},
                "ubuntu",
                {"host": "u0", "repo_path": "/repo"},
                {},
                ensure_host_reachable_fn=ensure,
            ),
            (None, "/submitted"),
        )
        self.assertEqual(ensure_calls, [])

        self.assertEqual(
            self.mod.resolve_ssh_target_execution(
                {"submission": {"target_hosts": {"ubuntu": {"status": "utm-fallback-pending", "configured_host": "fallback"}}}},
                "ubuntu",
                {"host": "u0", "repo_path": "/repo"},
                {"ssh_timeout": 3},
                ensure_host_reachable_fn=ensure,
            ),
            ("live-host", "/repo"),
        )
        self.assertEqual(ensure_calls[-1][1]["host"], "fallback")

        self.assertEqual(
            self.mod.resolve_ssh_target_execution(
                {},
                "ubuntu",
                {"host": "u0", "repo_path": "/repo"},
                {},
                ensure_host_reachable_fn=ensure,
            ),
            ("live-host", "/repo"),
        )
        self.assertEqual(ensure_calls[-1][1]["host"], "u0")

    def test_build_target_tasks_binds_target_runners_and_reporters(self) -> None:
        job = {
            "id": "job-plan",
            "branch": "feature/plan",
            "sha": "a" * 40,
            "targets": ["mac", "ubuntu", "windows"],
            "validation": "full",
        }
        config = {
            "targets": {
                "mac": {"enabled": True, "exclude_tests": "mac-slow"},
                "ubuntu": {
                    "enabled": True,
                    "host": "ubuntu",
                    "repo_path": "/home/daniel/pulp",
                    "exclude_tests": "ubuntu-slow",
                },
                "windows": {
                    "enabled": True,
                    "host": "win",
                    "repo_path": r"C:\Pulp",
                    "exclude_tests": "win-slow",
                    "cmake_generator": "Ninja",
                    "cmake_platform": "ARM64",
                    "cmake_generator_instance": "VS",
                },
            },
            "defaults": {},
        }
        reporters = {}
        calls = []

        def progress_factory(name: str) -> str:
            reporter = f"report-{name}"
            reporters[name] = reporter
            return reporter

        def resolve(_job: dict, target_name: str, target_cfg: dict, _defaults: dict) -> tuple[str, str]:
            return target_cfg["host"], target_cfg["repo_path"]

        def run_local(_job: dict, exclude_tests: str, report_progress: str) -> dict:
            calls.append(("mac", exclude_tests, report_progress))
            return {"target": "mac", "status": "pass"}

        def run_posix(target_name: str, host: str, repo_path: str, _job: dict, **kwargs) -> dict:
            calls.append((target_name, host, repo_path, kwargs["exclude_tests"], kwargs["report_progress"]))
            return {"target": target_name, "status": "pass"}

        def run_windows(target_name: str, host: str, repo_path: str, _job: dict, **kwargs) -> dict:
            calls.append(
                (
                    target_name,
                    host,
                    repo_path,
                    kwargs["exclude_tests"],
                    kwargs["cmake_generator"],
                    kwargs["cmake_platform"],
                    kwargs["cmake_generator_instance"],
                    kwargs["report_progress"],
                )
            )
            return {"target": target_name, "status": "pass"}

        tasks = self.mod.build_target_tasks(
            job,
            config,
            enabled_targets_fn=lambda _config: ["mac"],
            resolve_ssh_target_execution_fn=resolve,
            run_local_validation_fn=run_local,
            run_posix_ssh_validation_fn=run_posix,
            run_windows_ssh_validation_fn=run_windows,
            progress_factory=progress_factory,
        )

        self.assertEqual([name for name, _fn in tasks], ["mac", "ubuntu", "windows"])
        self.assertEqual([fn()["status"] for _name, fn in tasks], ["pass", "pass", "pass"])
        self.assertEqual(reporters, {"mac": "report-mac", "ubuntu": "report-ubuntu", "windows": "report-windows"})
        self.assertEqual(
            calls,
            [
                ("mac", "mac-slow", "report-mac"),
                ("ubuntu", "ubuntu", "/home/daniel/pulp", "ubuntu-slow", "report-ubuntu"),
                ("windows", "win", r"C:\Pulp", "win-slow", "Ninja", "ARM64", "VS", "report-windows"),
            ],
        )

    def test_build_target_tasks_returns_unreachable_results_without_reporters(self) -> None:
        job = {
            "id": "job-offline",
            "branch": "feature/offline",
            "sha": "b" * 40,
            "targets": ["ubuntu", "windows"],
            "validation": "full",
        }
        config = {
            "targets": {
                "mac": {"enabled": False},
                "ubuntu": {"enabled": True, "host": "ubuntu", "repo_path": "/repo"},
                "windows": {"enabled": True, "host": "win", "repo_path": r"C:\Repo"},
            },
            "defaults": {},
        }
        reporters = []

        tasks = self.mod.build_target_tasks(
            job,
            config,
            enabled_targets_fn=lambda _config: ["mac"],
            resolve_ssh_target_execution_fn=lambda _job, _name, _cfg, _defaults: (None, "/repo"),
            run_local_validation_fn=lambda *_args, **_kwargs: self.fail("unexpected local runner"),
            run_posix_ssh_validation_fn=lambda *_args, **_kwargs: self.fail("unexpected posix runner"),
            run_windows_ssh_validation_fn=lambda *_args, **_kwargs: self.fail("unexpected windows runner"),
            progress_factory=lambda name: reporters.append(name) or name,
        )

        self.assertEqual([name for name, _fn in tasks], ["ubuntu", "windows"])
        self.assertEqual([fn()["status"] for _name, fn in tasks], ["unreachable", "unreachable"])
        self.assertEqual(reporters, [])

    def test_run_target_tasks_collects_results_and_reports_completion(self) -> None:
        completed = []

        def failing_task() -> dict:
            raise RuntimeError("boom")

        results = self.mod.run_target_tasks(
            [
                ("mac", lambda: {"target": "mac", "status": "pass"}),
                ("windows", failing_task),
            ],
            exception_result_fn=lambda name, exc: {
                "target": name,
                "status": "error",
                "stderr_tail": str(exc),
            },
            on_target_complete=lambda name, result: completed.append((name, result["status"])),
        )

        self.assertCountEqual(
            [(item["target"], item["status"]) for item in results],
            [("mac", "pass"), ("windows", "error")],
        )
        self.assertCountEqual(completed, [("mac", "pass"), ("windows", "error")])
        self.assertEqual(
            self.mod.run_target_tasks(
                [],
                exception_result_fn=lambda name, exc: {},
                on_target_complete=lambda name, result: None,
            ),
            [],
        )

    def test_local_validation_command_builds_full_and_smoke_commands(self) -> None:
        full_cmd, full_validation = self.mod.local_validation_command(
            {"sha": "a" * 40, "targets": ["mac"], "validation": "full"},
            exclude_tests="slow",
        )
        smoke_cmd, smoke_validation = self.mod.local_validation_command(
            {"sha": "b" * 40, "targets": ["mac"], "validation": "smoke"}
        )

        self.assertEqual(full_validation, "full")
        self.assertEqual(full_cmd[0], "env")
        self.assertIn("PULP_VALIDATE_REUSE_PREPARED=1", full_cmd)
        self.assertIn("./validate-build.sh", full_cmd)
        self.assertIn("--keep-worktree", full_cmd)
        self.assertIn("--exclude-regex", full_cmd)
        self.assertIn("slow", full_cmd)
        self.assertNotIn("PULP_EXPECT_SMOKE=1", full_cmd)

        self.assertEqual(smoke_validation, "smoke")
        self.assertIn("PULP_EXPECT_SMOKE=1", smoke_cmd)
        self.assertIn("--smoke", smoke_cmd)
        self.assertIn("--no-tests", smoke_cmd)

    def test_run_local_validation_reports_progress_and_returns_result(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repo"
            root.mkdir()
            log_path = Path(tmp) / "mac.log"
            progress = []
            messages = []
            captured = {}

            def report_progress(**fields) -> None:
                progress.append(fields)

            def prepare_target_log(job_id: str, target_name: str) -> Path:
                captured["prepared"] = (job_id, target_name)
                return log_path

            def local_command(job: dict, exclude_tests: str) -> tuple[list[str], str]:
                captured["command_job"] = job["id"]
                captured["exclude_tests"] = exclude_tests
                return ["validate", job["sha"]], "full"

            def run_logged_command(cmd: list[str], **kwargs) -> dict:
                captured["cmd"] = cmd
                captured["cwd"] = kwargs["cwd"]
                captured["timeout"] = kwargs["timeout"]
                captured["log_path"] = kwargs["log_path"]
                captured["report_progress"] = kwargs["report_progress"]
                return {
                    "timed_out": False,
                    "returncode": 0,
                    "output": "ok\n",
                    "duration_secs": 1.0,
                }

            result = self.mod.run_local_validation(
                {"id": "job-local", "branch": "feature/local", "sha": "c" * 40},
                "slow",
                report_progress,
                root=root,
                print_fn=messages.append,
                short_sha_fn=lambda sha: sha[:12],
                prepare_target_log_fn=prepare_target_log,
                now_iso_fn=lambda: "2026-06-10T00:00:00Z",
                local_validation_command_fn=local_command,
                run_logged_command_fn=run_logged_command,
                validation_result_from_run_fn=self.mod.validation_result_from_run,
            )

        self.assertEqual(messages, ["  [mac] Running local validation on feature/local @ cccccccccccc..."])
        self.assertEqual(captured["prepared"], ("job-local", "mac"))
        self.assertEqual(captured["command_job"], "job-local")
        self.assertEqual(captured["exclude_tests"], "slow")
        self.assertEqual(captured["cmd"], ["validate", "c" * 40])
        self.assertEqual(captured["cwd"], root)
        self.assertEqual(captured["timeout"], 3600)
        self.assertEqual(captured["log_path"], log_path)
        self.assertIs(captured["report_progress"], report_progress)
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
        self.assertEqual(result["log_file"], str(log_path))
        self.assertEqual(result["validation"], "full")
        self.assertEqual(result["transport_mode"], "local")

    def test_run_posix_ssh_validation_syncs_bundle_reports_progress_and_returns_result(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "ubuntu.log"
            progress = []
            messages = []
            captured = {}

            def report_progress(**fields) -> None:
                progress.append(fields)

            def prepare_target_log(job_id: str, target_name: str) -> Path:
                captured["prepared"] = (job_id, target_name)
                return log_path

            def sync_bundle(host: str, job: dict, **kwargs) -> tuple[str, str]:
                captured["sync"] = (host, job["id"], kwargs["report_progress"], kwargs["config"])
                return "pulp-ci-job-posix.bundle", "refs/pulp-ci-bundles/job-posix"

            def posix_command(
                target_name: str,
                host: str,
                repo_path: str,
                job: dict,
                **kwargs,
            ) -> tuple[list[str], str]:
                captured["command"] = (target_name, host, repo_path, job["id"], kwargs)
                return ["ssh", host, "validate"], "smoke"

            def run_logged_command(cmd: list[str], **kwargs) -> dict:
                captured["run"] = (cmd, kwargs)
                return {
                    "timed_out": False,
                    "returncode": 0,
                    "output": "__PULP_VALIDATION__:smoke\n__PULP_TEST_POLICY__:skip\nok\n",
                    "duration_secs": 1.0,
                }

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
                prepare_target_log_fn=prepare_target_log,
                now_iso_fn=lambda: "2026-06-10T00:00:00Z",
                sync_job_bundle_to_ssh_host_fn=sync_bundle,
                posix_ssh_validation_command_fn=posix_command,
                run_logged_command_fn=run_logged_command,
                validation_result_from_run_fn=self.mod.validation_result_from_run,
                validation_error_result_fn=self.mod.validation_error_result,
            )

        self.assertEqual(messages, ["  [ubuntu] Running validation on ubuntu.example.com:/tmp/pulp @ dddddddddddd..."])
        self.assertEqual(captured["prepared"], ("job-posix", "ubuntu"))
        self.assertEqual(captured["sync"], ("ubuntu.example.com", "job-posix", report_progress, {"ssh": {"ubuntu": {}}}))
        self.assertEqual(
            captured["command"],
            (
                "ubuntu",
                "ubuntu.example.com",
                "/tmp/pulp",
                "job-posix",
                {
                    "bundle_name": "pulp-ci-job-posix.bundle",
                    "bundle_ref": "refs/pulp-ci-bundles/job-posix",
                    "exclude_tests": "slow",
                },
            ),
        )
        self.assertEqual(captured["run"][0], ["ssh", "ubuntu.example.com", "validate"])
        self.assertEqual(captured["run"][1]["timeout"], 3600)
        self.assertEqual(captured["run"][1]["log_path"], log_path)
        self.assertIs(captured["run"][1]["report_progress"], report_progress)
        self.assertEqual(
            progress,
            [
                {
                    "phase": "connect",
                    "host": "ubuntu.example.com",
                    "log_path": str(log_path),
                    "last_output_at": "2026-06-10T00:00:00Z",
                    "transport_mode": "bundle",
                }
            ],
        )
        self.assertEqual(result["target"], "ubuntu")
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["log_file"], str(log_path))
        self.assertEqual(result["validation"], "smoke")
        self.assertEqual(result["transport_mode"], "bundle")

    def test_run_posix_ssh_validation_returns_bundle_sync_error_result(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "ubuntu.log"

            def sync_bundle(*_args, **_kwargs) -> tuple[str, str]:
                raise RuntimeError("upload failed")

            result = self.mod.run_posix_ssh_validation(
                "ubuntu",
                "ubuntu.example.com",
                "/tmp/pulp",
                {"id": "job-posix-error", "branch": "feature/posix", "sha": "e" * 40},
                print_fn=lambda _message: None,
                short_sha_fn=lambda sha: sha[:12],
                prepare_target_log_fn=lambda _job_id, _target_name: log_path,
                now_iso_fn=lambda: "2026-06-10T00:00:00Z",
                sync_job_bundle_to_ssh_host_fn=sync_bundle,
                posix_ssh_validation_command_fn=lambda *_args, **_kwargs: self.fail("unexpected command build"),
                run_logged_command_fn=lambda *_args, **_kwargs: self.fail("unexpected command run"),
                validation_result_from_run_fn=self.mod.validation_result_from_run,
                validation_error_result_fn=self.mod.validation_error_result,
            )

        self.assertEqual(result["target"], "ubuntu")
        self.assertEqual(result["status"], "error")
        self.assertEqual(result["stderr_tail"], "upload failed")
        self.assertEqual(result["log_file"], str(log_path))
        self.assertEqual(result["transport_mode"], "bundle")

    def test_posix_ssh_validation_command_builds_full_command(self) -> None:
        job = {
            "id": "job701",
            "branch": "feature/posix",
            "sha": "c" * 40,
            "targets": ["ubuntu"],
            "validation": "full",
        }

        cmd, validation = self.mod.posix_ssh_validation_command(
            "ubuntu",
            "ubuntu.example.com",
            "/tmp/pulp repo",
            job,
            bundle_name="bundle name.bundle",
            bundle_ref="refs/pulp-ci/job701",
            exclude_tests="slow test",
        )
        remote_cmd = self.mod.shlex.split(cmd[-1])[0]

        self.assertEqual(validation, "full")
        self.assertEqual(cmd[:3], ["ssh", "ubuntu.example.com", "bash"])
        self.assertIn('export PATH="$HOME/.local/bin:$PATH"; set -euo pipefail', remote_cmd)
        self.assertIn("branch=feature/posix", remote_cmd)
        self.assertIn("sha=" + "c" * 40, remote_cmd)
        self.assertIn("git fetch \"$bundle\" \"$bundle_ref:refs/remotes/origin/$branch\"", remote_cmd)
        self.assertIn("PULP_EXPECT_SMOKE=0", remote_cmd)
        self.assertIn("bash \"$script\" --quiet --keep-worktree --ref \"$sha\"", remote_cmd)
        self.assertIn("--exclude-regex 'slow test'", remote_cmd)
        self.assertIn("ubuntu cannot validate cccccccccccc on ubuntu.example.com", remote_cmd)

    def test_posix_ssh_validation_command_builds_smoke_command(self) -> None:
        job = {
            "id": "job702",
            "branch": "feature/smoke",
            "sha": "d" * 40,
            "targets": ["ubuntu"],
            "validation": "smoke",
        }

        cmd, validation = self.mod.posix_ssh_validation_command(
            "ubuntu",
            "ubuntu.example.com",
            "/tmp/pulp",
            job,
            bundle_name="bundle.bundle",
            bundle_ref="refs/pulp-ci/job702",
        )
        remote_cmd = self.mod.shlex.split(cmd[-1])[0]

        self.assertEqual(validation, "smoke")
        self.assertIn("prepared/ubuntu/smoke", remote_cmd)
        self.assertIn("PULP_EXPECT_SMOKE=1", remote_cmd)
        self.assertIn("--smoke --no-tests", remote_cmd)

    def test_windows_validation_script_builds_full_script(self) -> None:
        job = {
            "id": "job801",
            "branch": "feature/windows",
            "sha": "e" * 40,
            "targets": ["windows"],
            "validation": "full",
        }

        script, validation = self.mod.windows_validation_script(
            "windows",
            "win.example.com",
            r"C:\Pulp's Repo",
            job,
            bundle_name="pulp-ci-job801.bundle",
            bundle_ref="refs/pulp-ci-bundles/job801",
            exclude_tests="slow windows",
            cmake_generator="Visual Studio 17 2022",
            resolved_platform="ARM64",
            resolved_generator_instance=r"C:\VS\2022",
            ps_literal_fn=lambda value: value.replace("'", "''"),
        )

        self.assertEqual(validation, "full")
        self.assertIn(r"$Repo = 'C:\Pulp''s Repo'", script)
        self.assertIn("$Branch = 'feature/windows'", script)
        self.assertIn("$Sha = '" + "e" * 40 + "'", script)
        self.assertIn("$BundleName = 'pulp-ci-job801.bundle'", script)
        self.assertIn("$BundleRef`:refs/pulp-ci-bundles/job801", script)
        self.assertIn("$ExcludeRegex = 'slow windows'", script)
        self.assertIn("$Generator = 'Visual Studio 17 2022'", script)
        self.assertIn("$Platform = 'ARM64'", script)
        self.assertIn(r"$GeneratorInstance = 'C:\VS\2022'", script)
        self.assertIn("$ValidationMode = 'full'", script)
        self.assertIn("$PreparedRoot = Join-Path $CiRoot 'prepared\\windows'", script)
        self.assertIn("$ReusePrepared = $true", script)
        self.assertIn('Write-Host "__PULP_TEST_POLICY__:run"', script)
        self.assertIn("windows cannot validate eeeeeeeeeeee on win.example.com", script)

    def test_windows_validation_script_builds_smoke_script(self) -> None:
        job = {
            "id": "job802",
            "branch": "feature/smoke",
            "sha": "f" * 40,
            "targets": ["mac", "windows"],
            "validation": "smoke",
        }

        script, validation = self.mod.windows_validation_script(
            "windows",
            "win.example.com",
            r"C:\Pulp",
            job,
            bundle_name="pulp-ci-job802.bundle",
            bundle_ref="refs/pulp-ci-bundles/job802",
            exclude_tests="",
            cmake_generator="Visual Studio 17 2022",
            resolved_platform="",
            resolved_generator_instance="",
            ps_literal_fn=lambda value: value.replace("'", "''"),
        )

        self.assertEqual(validation, "smoke")
        self.assertIn("$ValidationMode = 'smoke'", script)
        self.assertIn("$ReusePrepared = $false", script)
        self.assertIn('Write-Host "__PULP_TEST_POLICY__:skip"', script)
        self.assertIn("-DPULP_BUILD_TESTS=OFF", script)
        self.assertIn("-DPULP_BUILD_EXAMPLES=OFF", script)
        self.assertIn("-DPULP_ENABLE_GPU=OFF", script)
        self.assertIn("Invoke-Native cmake @('--install', $Build, '--prefix', $Install, '--config', 'Release')", script)
        self.assertIn('Write-Host "__PULP_PHASE__:smoke"', script)

    def test_validation_result_from_run_reports_timeout(self) -> None:
        result = self.mod.validation_result_from_run(
            "mac",
            {"timed_out": True, "duration_secs": 3600.0},
            log_path=Path("mac.log"),
            validation="full",
            transport_mode="local",
        )

        self.assertEqual(
            result,
            {
                "target": "mac",
                "status": "timeout",
                "exit_code": -1,
                "duration_secs": 3600.0,
                "stdout_tail": "",
                "stderr_tail": "Validation timed out after 3600s",
                "log_file": "mac.log",
                "transport_mode": "local",
            },
        )

    def test_validation_result_from_run_splits_pass_and_fail_tails(self) -> None:
        passed = self.mod.validation_result_from_run(
            "ubuntu",
            {"timed_out": False, "returncode": 0, "output": "ok\n", "duration_secs": 1.2},
            log_path=Path("ubuntu.log"),
            validation="full",
            transport_mode="bundle",
        )
        failed = self.mod.validation_result_from_run(
            "ubuntu",
            {"timed_out": False, "returncode": 7, "output": "boom\n", "duration_secs": 1.3},
            log_path=Path("ubuntu.log"),
            validation="full",
            transport_mode="bundle",
        )

        self.assertEqual(passed["status"], "pass")
        self.assertEqual(passed["stdout_tail"], "ok\n")
        self.assertEqual(passed["stderr_tail"], "")
        self.assertEqual(failed["status"], "fail")
        self.assertEqual(failed["stdout_tail"], "")
        self.assertEqual(failed["stderr_tail"], "boom\n")

    def test_validation_result_from_run_enforces_smoke_contract(self) -> None:
        result = self.mod.validation_result_from_run(
            "windows",
            {"timed_out": False, "returncode": 0, "output": "ok\n", "duration_secs": 1.2},
            log_path=Path("windows.log"),
            validation="smoke",
            transport_mode="bundle",
        )

        self.assertEqual(result["status"], "fail")
        self.assertIn("Smoke validation contract violated", result["stderr_tail"])
        self.assertEqual(result["stdout_tail"], "")

    def test_validation_error_result_matches_validation_contract(self) -> None:
        result = self.mod.validation_error_result(
            "windows",
            "probe failed",
            log_path=Path("windows.log"),
            transport_mode="bundle",
        )

        self.assertEqual(
            result,
            {
                "target": "windows",
                "status": "error",
                "exit_code": -1,
                "duration_secs": 0.0,
                "stdout_tail": "",
                "stderr_tail": "probe failed",
                "log_file": "windows.log",
                "transport_mode": "bundle",
            },
        )

    def test_run_logged_command_starts_reader_before_writing_input(self) -> None:
        read_started = threading.Event()
        read_finished = threading.Event()
        writes = []

        class FakeStdout:
            def __iter__(self):
                read_started.set()
                yield "ready\n"
                read_finished.set()

            def close(self):
                read_finished.set()

        class FakeStdin:
            def write(self, text):
                if not read_started.wait(timeout=1):
                    raise TimeoutError("reader did not start before stdin write")
                writes.append(text)

            def close(self):
                writes.append("<closed>")

        class FakeProc:
            def __init__(self):
                self.stdin = FakeStdin()
                self.stdout = FakeStdout()

            def poll(self):
                return 0 if read_finished.is_set() else None

            def wait(self, timeout=None):
                self.poll()
                read_finished.wait(timeout=timeout)
                return 0

            def kill(self):
                read_finished.set()

        original_popen = self.mod.subprocess.Popen
        self.mod.subprocess.Popen = lambda *args, **kwargs: FakeProc()
        try:
            result = self.mod.run_logged_command(["ssh", "win2"], input_text="payload", timeout=5)
        finally:
            self.mod.subprocess.Popen = original_popen

        self.assertFalse(result["timed_out"])
        self.assertEqual(result["returncode"], 0)
        self.assertEqual(result["output"], "ready\n")
        self.assertEqual(writes, ["payload", "<closed>"])

    def test_run_logged_command_keeps_markers_logs_and_reports_progress(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "marker.log"
            seen = {}

            def report_progress(**fields):
                seen.update(fields)

            result = self.mod.run_logged_command(
                [
                    "python3",
                    "-c",
                    (
                        "print('__PULP_VALIDATION__:smoke');"
                        "print('__PULP_TEST_POLICY__:skip');"
                        "print('__PULP_PHASE__:build');"
                        "print('done')"
                    ),
                ],
                log_path=log_path,
                report_progress=report_progress,
            )

            self.assertEqual(result["returncode"], 0)
            self.assertIn("__PULP_VALIDATION__:smoke", result["output"])
            self.assertIn("__PULP_TEST_POLICY__:skip", result["output"])
            self.assertIn("__PULP_PHASE__:build", result["output"])
            self.assertIn("done", result["output"])
            logged = log_path.read_text()
            self.assertIn("__PULP_VALIDATION__:smoke", logged)
            self.assertIn("__PULP_TEST_POLICY__:skip", logged)
            self.assertEqual(seen["validation_mode"], "smoke")
            self.assertEqual(seen["test_policy"], "skip")
            self.assertEqual(seen["phase"], "build")

    def test_run_logged_command_emits_quiet_heartbeat_and_stuck_state(self) -> None:
        seen: list[dict] = []

        def report_progress(**fields):
            seen.append(dict(fields))

        result = self.mod.run_logged_command(
            [
                "python3",
                "-c",
                "import time; time.sleep(0.18); print('done')",
            ],
            report_progress=report_progress,
            heartbeat_interval_secs=0.05,
            stuck_idle_secs=0.1,
        )

        self.assertEqual(result["returncode"], 0)
        heartbeat_events = [item for item in seen if item.get("last_heartbeat_at")]
        self.assertTrue(heartbeat_events, msg=f"missing heartbeat events in {seen}")
        liveness_values = {item.get("liveness") for item in heartbeat_events}
        self.assertTrue(liveness_values <= {"quiet", "stuck"}, msg=f"unexpected heartbeat states in {heartbeat_events}")
        self.assertTrue("stuck" in liveness_values, msg=f"missing stuck heartbeat in {heartbeat_events}")
        self.assertTrue(any(item.get("last_output_at") for item in seen))

    def test_run_logged_command_replaces_invalid_utf8_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "nonutf8.log"

            result = self.mod.run_logged_command(
                [
                    "python3",
                    "-c",
                    (
                        "import sys; "
                        "sys.stdout.buffer.write(b'prefix\\xe5suffix\\\\n'); "
                        "sys.stdout.flush()"
                    ),
                ],
                log_path=log_path,
            )

            self.assertEqual(result["returncode"], 0)
            self.assertIn("prefix", result["output"])
            self.assertIn("suffix", result["output"])
            self.assertIn("\ufffd", result["output"])
            logged = log_path.read_text()
            self.assertIn("prefix", logged)
            self.assertIn("suffix", logged)


if __name__ == "__main__":
    unittest.main()
