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
        ]:
            bindings[name] = object()
        return bindings

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


if __name__ == "__main__":
    unittest.main()
