#!/usr/bin/env python3
"""Tests for local CI command execution helpers."""

from __future__ import annotations

from datetime import datetime
import json
import os
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("execution.py")
QUEUE_MODULE_PATH = Path(__file__).with_name("queue_orchestrator.py")


def load_module(path: Path = MODULE_PATH, name: str = "pulp_local_ci_execution"):
    return load_module_from_path(path, module_name=name, add_module_dir=True)


class ExecutionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.queue = load_module(QUEUE_MODULE_PATH, "pulp_local_ci_queue_orchestrator")

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

    def test_process_job_returns_empty_result_without_target_tasks(self) -> None:
        job = {
            "id": "job-empty",
            "branch": "feature/empty",
            "sha": "a" * 40,
            "priority": "low",
        }
        messages = []
        completed = {}

        result = self.mod.process_job(
            job,
            {"targets": {}},
            print_fn=messages.append,
            short_sha_fn=lambda sha: sha[:12],
            config_for_job_execution_fn=lambda queued_job, config: {"resolved": config, "job": queued_job["id"]},
            build_target_tasks_fn=lambda queued_job, config, progress_factory=None: [],
            target_state_snapshot_fn=lambda states: dict(states),
            update_runner_active_targets_fn=lambda *_args: self.fail("unexpected runner active-target update"),
            update_job_active_targets_fn=lambda *_args: self.fail("unexpected job active-target update"),
            updated_target_state_fn=self.queue.updated_target_state,
            initial_target_state_fn=lambda job_id, target_name, *, started_at: {
                "status": "running",
                "started_at": started_at,
                "phase": "starting",
                "log_path": f"{job_id}-{target_name}.log",
            },
            completed_target_state_fn=lambda job_id, target_name, result, previous_state, *, completed_at: {
                **dict(previous_state or {}),
                "status": result["status"],
                "exit_code": result["exit_code"],
                "duration_secs": result["duration_secs"],
                "completed_at": completed_at,
                "phase": "done",
                "log_path": result.get("log_file", f"{job_id}-{target_name}.log"),
            },
            now_iso_fn=lambda: "2026-06-10T00:00:00Z",
            run_target_tasks_fn=lambda *_args, **_kwargs: self.fail("unexpected target runner"),
            completed_job_result_fn=lambda queued_job, results: completed.setdefault(
                "result",
                {"job_id": queued_job["id"], "results": results, "overall": "pass"},
            ),
            sorted_target_results_fn=self.mod.sorted_target_results,
        )

        self.assertEqual(result, {"job_id": "job-empty", "results": [], "overall": "pass"})
        self.assertEqual(
            messages,
            ["\n=== Validating [job-empty] feature/empty @ aaaaaaaaaaaa priority=low ===\n"],
        )
        self.assertEqual(completed["result"]["results"], [])

    def test_process_job_tracks_target_progress_and_completion(self) -> None:
        job = {
            "id": "job-process",
            "branch": "feature/process",
            "sha": "b" * 40,
            "priority": "normal",
        }
        snapshots = []
        messages = []
        build_seen = {}
        now_values = iter(
            [
                "2026-06-10T00:00:00Z",
                "2026-06-10T00:00:01Z",
            ]
        )

        def snapshot(states: dict[str, dict]) -> dict[str, dict]:
            return {target: dict(state) for target, state in states.items()}

        def build_tasks(queued_job: dict, config: dict, progress_factory=None):
            build_seen["job_id"] = queued_job["id"]
            build_seen["config"] = config
            reporter = progress_factory("mac")

            def run_mac() -> dict:
                reporter(phase="validate", log_path="mac.log", transport_mode="local")
                return {
                    "target": "mac",
                    "status": "pass",
                    "exit_code": 0,
                    "duration_secs": 1.0,
                }

            return [("mac", run_mac)]

        def run_tasks(tasks, *, on_target_complete):
            results = []
            for name, task in tasks:
                result = task()
                on_target_complete(name, result)
                results.append(result)
            return results

        result = self.mod.process_job(
            job,
            {"targets": {"mac": {}}},
            print_fn=messages.append,
            short_sha_fn=lambda sha: sha[:12],
            config_for_job_execution_fn=lambda queued_job, config: {"resolved_for": queued_job["id"], **config},
            build_target_tasks_fn=build_tasks,
            target_state_snapshot_fn=snapshot,
            update_runner_active_targets_fn=lambda job_id, states: snapshots.append(("runner", job_id, states)),
            update_job_active_targets_fn=lambda job_id, states: snapshots.append(("job", job_id, states)),
            updated_target_state_fn=self.queue.updated_target_state,
            initial_target_state_fn=lambda job_id, target_name, *, started_at: {
                "status": "running",
                "started_at": started_at,
                "phase": "starting",
                "log_path": f"{job_id}-{target_name}.log",
            },
            completed_target_state_fn=lambda job_id, target_name, result, previous_state, *, completed_at: {
                **dict(previous_state or {}),
                "status": result["status"],
                "exit_code": result["exit_code"],
                "duration_secs": result["duration_secs"],
                "completed_at": completed_at,
                "phase": "done",
                "log_path": result.get("log_file", f"{job_id}-{target_name}.log"),
            },
            now_iso_fn=lambda: next(now_values),
            run_target_tasks_fn=run_tasks,
            completed_job_result_fn=lambda queued_job, results: {
                "job_id": queued_job["id"],
                "results": results,
                "overall": "pass" if all(result["status"] == "pass" for result in results) else "fail",
            },
            sorted_target_results_fn=self.mod.sorted_target_results,
        )

        self.assertEqual(build_seen["job_id"], "job-process")
        self.assertEqual(build_seen["config"]["resolved_for"], "job-process")
        self.assertEqual(result["overall"], "pass")
        self.assertEqual(result["results"], [{"target": "mac", "status": "pass", "exit_code": 0, "duration_secs": 1.0}])
        self.assertEqual(len(snapshots), 6)
        initial_state = snapshots[0][2]["mac"]
        progress_state = snapshots[2][2]["mac"]
        completed_state = snapshots[4][2]["mac"]
        self.assertEqual(snapshots[0][0], "runner")
        self.assertEqual(snapshots[1][0], "job")
        self.assertEqual(initial_state["status"], "running")
        self.assertEqual(initial_state["started_at"], "2026-06-10T00:00:00Z")
        self.assertEqual(progress_state["phase"], "validate")
        self.assertEqual(progress_state["log_path"], "mac.log")
        self.assertEqual(completed_state["status"], "pass")
        self.assertEqual(completed_state["completed_at"], "2026-06-10T00:00:01Z")

    def test_process_job_keeps_multi_target_snapshots_consistent_when_completion_order_varies(self) -> None:
        job = {
            "id": "job-multi",
            "branch": "feature/multi",
            "sha": "c" * 40,
            "priority": "normal",
        }
        snapshots = []
        now_values = iter(
            [
                "2026-06-10T00:00:00Z",
                "2026-06-10T00:00:01Z",
                "2026-06-10T00:00:02Z",
                "2026-06-10T00:00:03Z",
                "2026-06-10T00:00:04Z",
                "2026-06-10T00:00:05Z",
            ]
        )

        def initial_state(job_id: str, target_name: str, *, started_at: str) -> dict:
            return {
                "status": "running",
                "started_at": started_at,
                "phase": "starting",
                "log_path": f"{job_id}-{target_name}.log",
            }

        def completed_state(
            job_id: str,
            target_name: str,
            result: dict,
            previous_state: dict | None,
            *,
            completed_at: str,
        ) -> dict:
            return {
                **dict(previous_state or {}),
                "status": result["status"],
                "exit_code": result["exit_code"],
                "duration_secs": result["duration_secs"],
                "completed_at": completed_at,
                "phase": "done",
                "log_path": result.get("log_file", f"{job_id}-{target_name}.log"),
                "transport_mode": result.get("transport_mode", (previous_state or {}).get("transport_mode")),
            }

        def snapshot(states: dict[str, dict]) -> dict[str, dict]:
            return {target: dict(state) for target, state in states.items()}

        def record_snapshot(kind: str, job_id: str, states: dict[str, dict]) -> None:
            snapshots.append((kind, job_id, snapshot(states)))

        reporters = {}

        def build_tasks(_queued_job: dict, _config: dict, progress_factory=None):
            def make_task(target: str, status: str, transport_mode: str):
                reporter = progress_factory(target)
                reporters[target] = reporter

                def run_target() -> dict:
                    reporter(phase=f"validate-{target}", log_path=f"{target}.log", transport_mode=transport_mode)
                    return {
                        "target": target,
                        "status": status,
                        "exit_code": 0 if status == "pass" else 1,
                        "duration_secs": {"mac": 1.0, "ubuntu": 2.0, "windows": 3.0}[target],
                        "log_file": f"{target}.log",
                        "transport_mode": transport_mode,
                    }

                return run_target

            return [
                ("mac", make_task("mac", "pass", "local")),
                ("ubuntu", make_task("ubuntu", "fail", "bundle")),
                ("windows", make_task("windows", "pass", "bundle")),
            ]

        def run_tasks(tasks, *, on_target_complete):
            by_name = dict(tasks)
            results = []
            for name in ["windows", "mac", "ubuntu"]:
                result = by_name[name]()
                on_target_complete(name, result)
                results.append(result)
            return results

        result = self.mod.process_job(
            job,
            {"targets": {"mac": {}, "ubuntu": {}, "windows": {}}},
            print_fn=lambda _message: None,
            short_sha_fn=lambda sha: sha[:12],
            config_for_job_execution_fn=lambda _queued_job, config: config,
            build_target_tasks_fn=build_tasks,
            target_state_snapshot_fn=snapshot,
            update_runner_active_targets_fn=lambda job_id, states: record_snapshot("runner", job_id, states),
            update_job_active_targets_fn=lambda job_id, states: record_snapshot("job", job_id, states),
            updated_target_state_fn=self.queue.updated_target_state,
            initial_target_state_fn=initial_state,
            completed_target_state_fn=completed_state,
            now_iso_fn=lambda: next(now_values),
            run_target_tasks_fn=run_tasks,
            completed_job_result_fn=lambda queued_job, results: {
                "job_id": queued_job["id"],
                "results": results,
                "overall": "pass" if all(item["status"] == "pass" for item in results) else "fail",
            },
            sorted_target_results_fn=self.mod.sorted_target_results,
        )

        self.assertEqual([item["target"] for item in result["results"]], ["mac", "ubuntu", "windows"])
        self.assertEqual(result["overall"], "fail")
        self.assertEqual(len(snapshots), 14)
        for index in range(0, len(snapshots), 2):
            runner = snapshots[index]
            queued_job = snapshots[index + 1]
            self.assertEqual(runner[0], "runner")
            self.assertEqual(queued_job[0], "job")
            self.assertEqual(runner[1], "job-multi")
            self.assertEqual(queued_job[1], "job-multi")
            self.assertEqual(runner[2], queued_job[2])

        initial_snapshot = snapshots[0][2]
        self.assertEqual(set(initial_snapshot), {"mac", "ubuntu", "windows"})
        self.assertTrue(all(state["status"] == "running" for state in initial_snapshot.values()))
        windows_done_snapshot = snapshots[4][2]
        self.assertEqual(windows_done_snapshot["windows"]["status"], "pass")
        self.assertEqual(windows_done_snapshot["mac"]["status"], "running")
        self.assertEqual(windows_done_snapshot["ubuntu"]["status"], "running")
        final_snapshot = snapshots[-2][2]
        self.assertEqual(final_snapshot["mac"]["status"], "pass")
        self.assertEqual(final_snapshot["ubuntu"]["status"], "fail")
        self.assertEqual(final_snapshot["windows"]["status"], "pass")
        self.assertEqual(final_snapshot["ubuntu"]["completed_at"], "2026-06-10T00:00:05Z")

    def test_save_result_persists_json_and_updates_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            results_dir = Path(tmp) / "results"
            ensured = []
            evidence_updates = []
            result = {
                "job_id": "job123",
                "branch": "feature/result-store",
                "sha": "a" * 40,
                "priority": "normal",
                "results": [],
                "overall": "pass",
            }

            path = self.mod.save_result(
                result,
                ensure_state_dirs_fn=lambda: ensured.append(True) or results_dir.mkdir(parents=True),
                results_dir_fn=lambda: results_dir,
                update_evidence_index_fn=lambda payload, result_path: evidence_updates.append(
                    (dict(payload), result_path)
                ),
                now_fn=lambda: datetime(2026, 6, 10, 1, 2, 3),
            )

            self.assertEqual(path.name, "20260610-010203-job123-feature-result-store.json")
            self.assertEqual(json.loads(path.read_text()), result)
            self.assertEqual(ensured, [True])
            self.assertEqual(evidence_updates, [(result, path)])

    def test_print_result_renders_lines_through_injected_helpers(self) -> None:
        printed = []
        result = {
            "job_id": "job123",
            "branch": "feature/result-store",
            "validation": "smoke",
            "results": [{"target": "mac", "status": "pass"}],
            "overall": "pass",
        }

        self.mod.print_result(
            result,
            Path("result.json"),
            normalize_result_fn=lambda payload: {**payload, "normalized": True},
            result_validation_line_fn=lambda payload: "  validation  smoke" if payload["normalized"] else None,
            result_execution_line_fn=lambda payload: "  execution   direct",
            result_target_lines_fn=lambda payload: ["  mac         PASS          1.0s"],
            result_overall_line_fn=lambda payload: "  overall     PASS",
            print_fn=printed.append,
        )

        self.assertEqual(
            printed,
            [
                "\n--- Result: [job123] feature/result-store ---",
                "  validation  smoke",
                "  execution   direct",
                "  mac         PASS          1.0s",
                "  overall     PASS",
                "  Saved: result.json",
                "",
            ],
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

    def test_run_windows_ssh_validation_probes_checkout_and_runs_powershell(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repo"
            root.mkdir()
            log_path = Path(tmp) / "windows.log"
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

            def windows_script(
                target_name: str,
                host: str,
                effective_repo_path: str,
                job: dict,
                **kwargs,
            ) -> tuple[str, str]:
                captured["script"] = (target_name, host, effective_repo_path, job["id"], kwargs)
                return "Write-Host ok", "full"

            def run_logged_command(cmd: list[str], **kwargs) -> dict:
                captured["run"] = (cmd, kwargs)
                return {
                    "timed_out": False,
                    "returncode": 0,
                    "output": "ok\n",
                    "duration_secs": 1.0,
                }

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
                prepare_target_log_fn=lambda job_id, target_name: log_path,
                sync_job_bundle_to_ssh_host_fn=sync_bundle,
                validation_error_result_fn=self.mod.validation_error_result,
                ensure_windows_remote_repo_checkout_fn=ensure_repo,
                git_origin_clone_url_fn=lambda repo_root: f"https://example.invalid/{repo_root.name}.git",
                print_fn=messages.append,
                short_sha_fn=lambda sha: sha[:12],
                now_iso_fn=lambda: "2026-06-10T00:00:00Z",
                probe_windows_ssh_cmake_settings_fn=probe_cmake,
                windows_validation_script_fn=windows_script,
                windows_ssh_powershell_command_fn=lambda host: ["ssh", host, "powershell"],
                run_logged_command_fn=run_logged_command,
                validation_result_from_run_fn=self.mod.validation_result_from_run,
            )

        self.assertEqual(messages, [r"  [windows] Running validation on win.example.com:C:\Prepared\Pulp @ ffffffffffff..."])
        self.assertEqual(captured["sync"], ("win.example.com", "job-windows", report_progress, {"targets": {"windows": {}}}))
        self.assertEqual(captured["repo"][0], "win.example.com")
        self.assertEqual(captured["repo"][1], r"C:\Pulp")
        self.assertEqual(captured["repo"][2]["remote_url"], "https://example.invalid/repo.git")
        self.assertEqual(captured["repo"][2]["bundle_name"], "pulp-ci-job-windows.bundle")
        self.assertEqual(captured["repo"][2]["bundle_ref"], "refs/pulp-ci-bundles/job-windows")
        self.assertEqual(captured["probe"], ("win.example.com", "Visual Studio 17 2022", "ARM64", r"C:\RequestedVS"))
        self.assertEqual(
            captured["script"],
            (
                "windows",
                "win.example.com",
                r"C:\Prepared\Pulp",
                "job-windows",
                {
                    "bundle_name": "pulp-ci-job-windows.bundle",
                    "bundle_ref": "refs/pulp-ci-bundles/job-windows",
                    "exclude_tests": "slow",
                    "cmake_generator": "Visual Studio 17 2022",
                    "resolved_platform": "ARM64",
                    "resolved_generator_instance": r"C:\VS",
                },
            ),
        )
        self.assertEqual(captured["run"][0], ["ssh", "win.example.com", "powershell"])
        self.assertEqual(captured["run"][1]["input_text"], "Write-Host ok")
        self.assertEqual(captured["run"][1]["timeout"], 3600)
        self.assertEqual(captured["run"][1]["log_path"], log_path)
        self.assertIs(captured["run"][1]["report_progress"], report_progress)
        self.assertEqual(
            progress,
            [
                {
                    "phase": "connect",
                    "host": "win.example.com",
                    "log_path": str(log_path),
                    "last_output_at": "2026-06-10T00:00:00Z",
                    "transport_mode": "bundle",
                }
            ],
        )
        self.assertEqual(result["target"], "windows")
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["validation"], "full")
        self.assertEqual(result["transport_mode"], "bundle")

    def test_run_windows_ssh_validation_returns_missing_repo_probe_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repo"
            root.mkdir()
            log_path = Path(tmp) / "windows.log"

            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win.example.com",
                r"C:\Pulp",
                {"id": "job-windows-error", "branch": "feature/windows", "sha": "a" * 40},
                root=root,
                prepare_target_log_fn=lambda _job_id, _target_name: log_path,
                sync_job_bundle_to_ssh_host_fn=lambda *_args, **_kwargs: ("bundle", "ref"),
                validation_error_result_fn=self.mod.validation_error_result,
                ensure_windows_remote_repo_checkout_fn=lambda *_args, **_kwargs: None,
                git_origin_clone_url_fn=lambda _repo_root: "https://example.invalid/repo.git",
                print_fn=lambda _message: None,
                short_sha_fn=lambda sha: sha[:12],
                now_iso_fn=lambda: "2026-06-10T00:00:00Z",
                probe_windows_ssh_cmake_settings_fn=lambda *_args, **_kwargs: self.fail("unexpected cmake probe"),
                windows_validation_script_fn=lambda *_args, **_kwargs: self.fail("unexpected script build"),
                windows_ssh_powershell_command_fn=lambda _host: self.fail("unexpected command build"),
                run_logged_command_fn=lambda *_args, **_kwargs: self.fail("unexpected command run"),
                validation_result_from_run_fn=self.mod.validation_result_from_run,
            )

        self.assertEqual(result["target"], "windows")
        self.assertEqual(result["status"], "error")
        self.assertIn("no structured payload", result["stderr_tail"])
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
        self.assertIn("-DPULP_ENABLE_AUDIO_PROBES=OFF", script)
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
