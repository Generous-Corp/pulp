#!/usr/bin/env python3
"""Tests for validation execution facade bindings."""

import importlib.util
import builtins
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("execution_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class ExecutionBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        execution = types.SimpleNamespace(**{runner_name: runner})
        bindings = {"_execution": execution, "ROOT": Path("/repo"), "print": object()}
        for name in [
            "short_sha",
            "prepare_target_log",
            "now_iso",
            "normalize_provenance",
            "local_validation_command",
            "run_logged_command",
            "validation_result_from_run",
            "sync_job_bundle_to_ssh_host",
            "posix_ssh_validation_command",
            "validation_error_result",
            "ensure_windows_remote_repo_checkout",
            "git_origin_clone_url",
            "probe_windows_ssh_cmake_settings",
            "windows_validation_script",
            "windows_ssh_powershell_command",
            "load_config_file",
            "ensure_host_reachable",
            "enabled_targets",
            "resolve_ssh_target_execution",
            "run_local_validation",
            "run_posix_ssh_validation",
            "run_windows_ssh_validation",
            "config_for_job_execution",
            "_build_target_tasks",
            "target_state_snapshot",
            "update_runner_active_targets",
            "update_job_active_targets",
            "updated_target_state",
            "initial_target_state",
            "completed_target_state",
            "run_target_tasks",
            "completed_job_result",
            "sorted_target_results",
            "ensure_state_dirs",
            "results_dir",
            "update_evidence_index",
            "normalize_result",
            "result_validation_line",
            "result_execution_line",
            "result_target_lines",
            "result_overall_line",
            "target_exception_result",
            "ps_literal",
        ]:
            bindings[name] = object()
        bindings["datetime"] = types.SimpleNamespace(now=object())
        return bindings

    def test_simple_execution_helpers_delegate_and_bind_facade_dependencies(self):
        captured = {}

        def completed_job_result(job, results, *, completed_at, provenance):
            captured["completed"] = (job, results, completed_at, provenance)
            return {"overall": "pass"}

        def run_target_tasks(tasks, *, exception_result_fn, on_target_complete):
            captured["tasks"] = (tasks, exception_result_fn, on_target_complete)
            return [{"target": "mac"}]

        def run_logged_command(cmd, **kwargs):
            captured["logged"] = (cmd, kwargs)
            return {"exit_code": 0}

        def windows_validation_script(*args, **kwargs):
            captured["windows_script"] = (args, kwargs)
            return "script", "full"

        execution = types.SimpleNamespace(
            HEARTBEAT_INTERVAL_SECS=15.0,
            STUCK_IDLE_SECS=90.0,
            remote_commit_error=lambda target, host, job: f"{target}:{host}:{job['id']}",
            parse_progress_marker=lambda line: {"line": line},
            prepared_state_root=lambda target, validation: Path(f"/prepared/{target}/{validation}"),
            should_reuse_prepared_state=lambda job: job.get("reuse", False),
            local_validation_command=lambda job, exclude_tests="": ([job["id"], exclude_tests], job.get("validation", "full")),
            posix_ssh_validation_command=lambda *args, **kwargs: (list(args), kwargs["exclude_tests"]),
            validation_result_from_run=lambda *args, **kwargs: {"validation": args, **kwargs},
            validation_error_result=lambda *args, **kwargs: {"error": args, **kwargs},
            unreachable_target_result=lambda target, detail="Host unreachable": {"target": target, "detail": detail},
            target_exception_result=lambda target, exc: {"target": target, "error": str(exc)},
            completed_job_result=completed_job_result,
            sorted_target_results=lambda results: list(reversed(results)),
            run_target_tasks=run_target_tasks,
            run_logged_command=run_logged_command,
            windows_validation_script=windows_validation_script,
            submission_target_state=lambda job, target: {"job": job["id"], "target": target},
        )
        bindings = self._bindings("unused", lambda: None)
        bindings["_execution"] = execution
        bindings["now_iso"] = lambda: "now"
        bindings["normalize_provenance"] = lambda provenance: {"normalized": provenance}
        bindings["target_exception_result"] = object()
        bindings["ps_literal"] = object()

        self.assertEqual(self.mod.heartbeat_interval_secs(bindings), 15.0)
        self.assertEqual(self.mod.stuck_idle_secs(bindings), 90.0)
        self.assertEqual(self.mod.remote_commit_error(bindings, "mac", "host", {"id": "job"}), "mac:host:job")
        self.assertEqual(self.mod.parse_progress_marker(bindings, "line"), {"line": "line"})
        self.assertEqual(self.mod.prepared_state_root(bindings, "mac", "full"), Path("/prepared/mac/full"))
        self.assertTrue(self.mod.should_reuse_prepared_state(bindings, {"reuse": True}))
        self.assertEqual(self.mod.local_validation_command(bindings, {"id": "job", "validation": "smoke"}, "slow"), (["job", "slow"], "smoke"))
        self.assertEqual(
            self.mod.posix_ssh_validation_command(
                bindings,
                "ubuntu",
                "host",
                "/repo",
                {"id": "job"},
                bundle_name="bundle",
                bundle_ref="ref",
                exclude_tests="slow",
            )[1],
            "slow",
        )
        self.assertEqual(
            self.mod.validation_result_from_run(
                bindings,
                "mac",
                {"exit_code": 0},
                log_path=Path("/log"),
                validation="full",
                transport_mode="local",
            )["timeout_secs"],
            3600,
        )
        self.assertEqual(
            self.mod.validation_error_result(
                bindings,
                "mac",
                "detail",
                log_path=Path("/log"),
                transport_mode="local",
            )["transport_mode"],
            "local",
        )
        self.assertEqual(self.mod.unreachable_target_result(bindings, "ubuntu"), {"target": "ubuntu", "detail": "Host unreachable"})
        self.assertEqual(self.mod.target_exception_result(bindings, "mac", RuntimeError("boom")), {"target": "mac", "error": "boom"})
        self.assertEqual(self.mod.completed_job_result(bindings, {"id": "job", "provenance": "p"}, [{"target": "mac"}]), {"overall": "pass"})
        self.assertEqual(captured["completed"][2], "now")
        self.assertEqual(captured["completed"][3], {"normalized": "p"})
        self.assertEqual(self.mod.sorted_target_results(bindings, [1, 2]), [2, 1])
        complete = object()
        self.assertEqual(self.mod.run_target_tasks(bindings, [("mac", lambda: {})], on_target_complete=complete), [{"target": "mac"}])
        self.assertIs(captured["tasks"][1], bindings["target_exception_result"])
        self.assertIs(captured["tasks"][2], complete)
        self.assertEqual(
            self.mod.run_logged_command(bindings, ["cmd"], heartbeat_interval_secs=1.5, stuck_idle_secs=2.5),
            {"exit_code": 0},
        )
        self.assertEqual(captured["logged"][1]["heartbeat_interval_secs"], 1.5)
        self.assertEqual(captured["logged"][1]["stuck_idle_secs"], 2.5)
        self.assertEqual(
            self.mod.windows_validation_script(
                bindings,
                "windows",
                "host",
                r"C:\Repo",
                {"id": "job"},
                bundle_name="bundle",
                bundle_ref="ref",
                exclude_tests="slow",
                cmake_generator="Ninja",
                resolved_platform="ARM64",
                resolved_generator_instance=r"C:\VS",
            ),
            ("script", "full"),
        )
        self.assertIs(captured["windows_script"][1]["ps_literal_fn"], bindings["ps_literal"])
        self.assertEqual(self.mod.submission_target_state(bindings, {"id": "job"}, "mac"), {"job": "job", "target": "mac"})

    def test_run_local_validation_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"target": "mac"}

        bindings = self._bindings("run_local_validation", runner)
        progress = object()

        result = self.mod.run_local_validation(bindings, {"id": "job"}, "slow", progress)

        self.assertEqual(result, {"target": "mac"})
        self.assertEqual(captured["args"], ({"id": "job"}, "slow", progress))
        self.assertIs(captured["kwargs"]["root"], bindings["ROOT"])
        self.assertIs(captured["kwargs"]["print_fn"], bindings["print"])
        self.assertIs(captured["kwargs"]["local_validation_command_fn"], bindings["local_validation_command"])
        self.assertIs(captured["kwargs"]["run_logged_command_fn"], bindings["run_logged_command"])
        self.assertIs(captured["kwargs"]["validation_result_from_run_fn"], bindings["validation_result_from_run"])

    def test_run_local_validation_uses_builtin_print_when_globals_lack_print(self):
        captured = {}

        def runner(*_args, **kwargs):
            captured["kwargs"] = kwargs
            return {"target": "mac"}

        bindings = self._bindings("run_local_validation", runner)
        del bindings["print"]

        result = self.mod.run_local_validation(bindings, {"id": "job"})

        self.assertEqual(result, {"target": "mac"})
        self.assertIs(captured["kwargs"]["print_fn"], builtins.print)

    def test_run_posix_ssh_validation_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"target": "ubuntu"}

        bindings = self._bindings("run_posix_ssh_validation", runner)
        progress = object()
        config = {"ssh": {}}

        result = self.mod.run_posix_ssh_validation(
            bindings,
            "ubuntu",
            "ubuntu.example.com",
            "/repo",
            {"id": "job"},
            "slow",
            config,
            progress,
        )

        self.assertEqual(result, {"target": "ubuntu"})
        self.assertEqual(captured["args"], ("ubuntu", "ubuntu.example.com", "/repo", {"id": "job"}, "slow", config, progress))
        self.assertIs(captured["kwargs"]["sync_job_bundle_to_ssh_host_fn"], bindings["sync_job_bundle_to_ssh_host"])
        self.assertIs(captured["kwargs"]["posix_ssh_validation_command_fn"], bindings["posix_ssh_validation_command"])
        self.assertIs(captured["kwargs"]["run_logged_command_fn"], bindings["run_logged_command"])
        self.assertIs(captured["kwargs"]["validation_error_result_fn"], bindings["validation_error_result"])

    def test_run_windows_ssh_validation_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"target": "windows"}

        bindings = self._bindings("run_windows_ssh_validation", runner)
        progress = object()
        config = {"targets": {}}

        result = self.mod.run_windows_ssh_validation(
            bindings,
            "windows",
            "win.example.com",
            r"C:\Pulp",
            {"id": "job"},
            "slow",
            "Ninja",
            "ARM64",
            r"C:\VS",
            config,
            progress,
        )

        self.assertEqual(result, {"target": "windows"})
        self.assertEqual(
            captured["args"],
            ("windows", "win.example.com", r"C:\Pulp", {"id": "job"}, "slow", "Ninja", "ARM64", r"C:\VS", config, progress),
        )
        self.assertIs(captured["kwargs"]["root"], bindings["ROOT"])
        self.assertIs(captured["kwargs"]["ensure_windows_remote_repo_checkout_fn"], bindings["ensure_windows_remote_repo_checkout"])
        self.assertIs(captured["kwargs"]["git_origin_clone_url_fn"], bindings["git_origin_clone_url"])
        self.assertIs(captured["kwargs"]["probe_windows_ssh_cmake_settings_fn"], bindings["probe_windows_ssh_cmake_settings"])
        self.assertIs(captured["kwargs"]["windows_validation_script_fn"], bindings["windows_validation_script"])
        self.assertIs(captured["kwargs"]["windows_ssh_powershell_command_fn"], bindings["windows_ssh_powershell_command"])

    def test_config_for_job_execution_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"targets": {}}

        bindings = self._bindings("config_for_job_execution", runner)
        result = self.mod.config_for_job_execution(bindings, {"id": "job"}, {"targets": {}})

        self.assertEqual(result, {"targets": {}})
        self.assertEqual(captured["args"], ({"id": "job"}, {"targets": {}}))
        self.assertIs(captured["kwargs"]["load_config_file_fn"], bindings["load_config_file"])
        self.assertIs(captured["kwargs"]["warn_fn"], bindings["print"])

    def test_resolve_ssh_target_execution_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return ("host", "/repo")

        bindings = self._bindings("resolve_ssh_target_execution", runner)
        result = self.mod.resolve_ssh_target_execution(bindings, {"id": "job"}, "ubuntu", {"host": "u"}, {})

        self.assertEqual(result, ("host", "/repo"))
        self.assertEqual(captured["args"], ({"id": "job"}, "ubuntu", {"host": "u"}, {}))
        self.assertIs(captured["kwargs"]["ensure_host_reachable_fn"], bindings["ensure_host_reachable"])

    def test_build_target_tasks_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return [("mac", object())]

        bindings = self._bindings("build_target_tasks", runner)
        progress_factory = object()
        result = self.mod.build_target_tasks(bindings, {"id": "job"}, {"targets": {}}, progress_factory)

        self.assertEqual(len(result), 1)
        self.assertEqual(captured["args"], ({"id": "job"}, {"targets": {}}))
        self.assertIs(captured["kwargs"]["enabled_targets_fn"], bindings["enabled_targets"])
        self.assertIs(captured["kwargs"]["resolve_ssh_target_execution_fn"], bindings["resolve_ssh_target_execution"])
        self.assertIs(captured["kwargs"]["run_local_validation_fn"], bindings["run_local_validation"])
        self.assertIs(captured["kwargs"]["run_posix_ssh_validation_fn"], bindings["run_posix_ssh_validation"])
        self.assertIs(captured["kwargs"]["run_windows_ssh_validation_fn"], bindings["run_windows_ssh_validation"])
        self.assertIs(captured["kwargs"]["progress_factory"], progress_factory)

    def test_process_job_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"overall": "pass"}

        bindings = self._bindings("process_job", runner)
        result = self.mod.process_job(bindings, {"id": "job"}, {"targets": {}})

        self.assertEqual(result, {"overall": "pass"})
        self.assertEqual(captured["args"], ({"id": "job"}, {"targets": {}}))
        self.assertIs(captured["kwargs"]["print_fn"], bindings["print"])
        self.assertIs(captured["kwargs"]["short_sha_fn"], bindings["short_sha"])
        self.assertIs(captured["kwargs"]["config_for_job_execution_fn"], bindings["config_for_job_execution"])
        self.assertIs(captured["kwargs"]["build_target_tasks_fn"], bindings["_build_target_tasks"])
        self.assertIs(captured["kwargs"]["target_state_snapshot_fn"], bindings["target_state_snapshot"])
        self.assertIs(captured["kwargs"]["update_runner_active_targets_fn"], bindings["update_runner_active_targets"])
        self.assertIs(captured["kwargs"]["update_job_active_targets_fn"], bindings["update_job_active_targets"])
        self.assertIs(captured["kwargs"]["updated_target_state_fn"], bindings["updated_target_state"])
        self.assertIs(captured["kwargs"]["initial_target_state_fn"], bindings["initial_target_state"])
        self.assertIs(captured["kwargs"]["completed_target_state_fn"], bindings["completed_target_state"])
        self.assertIs(captured["kwargs"]["now_iso_fn"], bindings["now_iso"])
        self.assertIs(captured["kwargs"]["run_target_tasks_fn"], bindings["run_target_tasks"])
        self.assertIs(captured["kwargs"]["completed_job_result_fn"], bindings["completed_job_result"])
        self.assertIs(captured["kwargs"]["sorted_target_results_fn"], bindings["sorted_target_results"])

    def test_save_result_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return Path("/state/result.json")

        bindings = self._bindings("save_result", runner)
        result_payload = {"job_id": "job"}
        result = self.mod.save_result(bindings, result_payload)

        self.assertEqual(result, Path("/state/result.json"))
        self.assertEqual(captured["args"], (result_payload,))
        self.assertIs(captured["kwargs"]["ensure_state_dirs_fn"], bindings["ensure_state_dirs"])
        self.assertIs(captured["kwargs"]["results_dir_fn"], bindings["results_dir"])
        self.assertIs(captured["kwargs"]["update_evidence_index_fn"], bindings["update_evidence_index"])
        self.assertIs(captured["kwargs"]["now_fn"], bindings["datetime"].now)

    def test_print_result_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs

        bindings = self._bindings("print_result", runner)
        result_payload = {"job_id": "job"}
        result_path = Path("/state/result.json")
        self.mod.print_result(bindings, result_payload, result_path)

        self.assertEqual(captured["args"], (result_payload, result_path))
        self.assertIs(captured["kwargs"]["normalize_result_fn"], bindings["normalize_result"])
        self.assertIs(captured["kwargs"]["result_validation_line_fn"], bindings["result_validation_line"])
        self.assertIs(captured["kwargs"]["result_execution_line_fn"], bindings["result_execution_line"])
        self.assertIs(captured["kwargs"]["result_target_lines_fn"], bindings["result_target_lines"])
        self.assertIs(captured["kwargs"]["result_overall_line_fn"], bindings["result_overall_line"])
        self.assertIs(captured["kwargs"]["print_fn"], bindings["print"])


if __name__ == "__main__":
    unittest.main()
