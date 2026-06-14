#!/usr/bin/env python3
"""Tests for logs job-resolution command facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("logs_resolution_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LogsResolutionCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_exports_match_logs_resolution_helpers(self):
        self.assertEqual(self.mod.LOGS_RESOLUTION_COMMAND_EXPORTS, ("resolve_job_for_logs",))

    def test_logs_resolution_binds_facade_dependencies(self):
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

        self.assertEqual(self.mod.resolve_job_for_logs(bindings, "job"), {"id": "job"})
        self.assertEqual(captured["resolve_args"], ("job",))
        self.assertIs(captured["resolve_kwargs"]["load_queue_fn"], bindings["load_queue"])
        self.assertIs(captured["resolve_kwargs"]["current_runner_info_fn"], bindings["current_runner_info"])
        self.assertIs(
            captured["resolve_kwargs"]["select_job_for_logs_fn"],
            bindings["_queue_orchestrator"].select_job_for_logs,
        )

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
