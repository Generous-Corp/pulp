#!/usr/bin/env python3
"""Tests for validation job/result facade bindings."""

from __future__ import annotations

import unittest
from pathlib import Path
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("execution_job_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionJobBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_execution_job_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.EXECUTION_JOB_CONFIG_EXPORTS,
            *self.mod.EXECUTION_TARGET_TASK_EXPORTS,
            *self.mod.EXECUTION_RESULT_IO_EXPORTS,
        )

        self.assertEqual(self.mod.EXECUTION_JOB_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_execution_job_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_execution_job_config_helpers") as install_config,
            mock.patch.object(self.mod, "install_execution_target_task_helpers") as install_task,
            mock.patch.object(self.mod, "install_execution_result_io_helpers") as install_result_io,
        ):
            self.mod.install_execution_job_helpers(
                bindings,
                (
                    "config_for_job_execution",
                    "process_job",
                    "save_result",
                ),
            )

        install_config.assert_called_once_with(bindings, ("config_for_job_execution",))
        install_task.assert_called_once_with(bindings, ("process_job",))
        install_result_io.assert_called_once_with(bindings, ("save_result",))

    def test_install_execution_job_helpers_preserves_unknown_fallbacks(self) -> None:
        bindings = {"custom": object()}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_execution_job_helpers(bindings, ("custom",))

        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
