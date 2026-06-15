#!/usr/bin/env python3
"""Tests for logs job-resolution command facade bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("logs_resolution_command_bindings.py")


class LogsResolutionCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_logs_resolution_helpers(self):
        self.assertEqual(self.mod.LOGS_RESOLUTION_COMMAND_EXPORTS, ("resolve_job_for_logs",))

    def test_logs_resolution_delegates_with_assembled_dependencies(self):
        captured = {}

        def resolve_runner(*args, **kwargs):
            captured["resolve_args"] = args
            captured["resolve_kwargs"] = kwargs
            return {"id": "job"}

        bindings = {
            "_logs_cli": types.SimpleNamespace(resolve_job_for_logs=resolve_runner),
            "_queue_orchestrator": types.SimpleNamespace(select_job_for_logs=object()),
            "load_queue": object(),
            "current_runner_info": object(),
        }
        deps = {"load_queue_fn": object(), "select_job_for_logs_fn": object()}

        with mock.patch.object(self.mod, "logs_resolution_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.resolve_job_for_logs(bindings, "job"), {"id": "job"})
        self.assertEqual(captured["resolve_args"], ("job",))
        self.assertIs(captured["resolve_kwargs"]["load_queue_fn"], deps["load_queue_fn"])
        self.assertIs(captured["resolve_kwargs"]["select_job_for_logs_fn"], deps["select_job_for_logs_fn"])

    def test_install_logs_resolution_command_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_logs_resolution_command_helpers(
                bindings,
                ("resolve_job_for_logs", "custom_logs_resolution"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("resolve_job_for_logs",)),
                mock.call(bindings, self.mod.__dict__, ("custom_logs_resolution",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
