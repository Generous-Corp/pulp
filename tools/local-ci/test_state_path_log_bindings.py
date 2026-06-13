#!/usr/bin/env python3
"""Tests for target log state path facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("state_path_log_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class StatePathLogBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        paths = types.SimpleNamespace(
            job_logs_dir=make_runner("job_logs_dir", Path("/state/logs/job-1")),
            target_log_path=make_runner("target_log_path", Path("/state/logs/job-1/mac.log")),
            prepare_target_log=make_runner("prepare_target_log", Path("/state/logs/job-1/mac.log")),
        )
        return {"_state_paths": paths}, calls

    def test_log_path_helpers_delegate_with_job_and_target_arguments(self) -> None:
        bindings, calls = self._bindings()

        self.assertEqual(self.mod.job_logs_dir(bindings, "job-1"), Path("/state/logs/job-1"))
        self.assertEqual(self.mod.target_log_path(bindings, "job-1", "mac"), Path("/state/logs/job-1/mac.log"))
        self.assertEqual(self.mod.prepare_target_log(bindings, "job-1", "mac"), Path("/state/logs/job-1/mac.log"))

        self.assertEqual([call[0] for call in calls], ["job_logs_dir", "target_log_path", "prepare_target_log"])
        self.assertEqual(calls[0][1], ("job-1",))
        self.assertEqual(calls[1][1], ("job-1", "mac"))
        self.assertEqual(calls[2][1], ("job-1", "mac"))

    def test_install_state_path_log_helpers_wires_named_exports(self) -> None:
        bindings, calls = self._bindings()

        self.mod.install_state_path_log_helpers(bindings, ("job_logs_dir", "prepare_target_log"))

        self.assertEqual(bindings["job_logs_dir"]("job-1"), Path("/state/logs/job-1"))
        self.assertEqual(bindings["prepare_target_log"]("job-1", "mac"), Path("/state/logs/job-1/mac.log"))
        self.assertEqual(bindings["job_logs_dir"].__name__, "job_logs_dir")
        self.assertEqual([call[0] for call in calls], ["job_logs_dir", "prepare_target_log"])

    def test_install_state_path_log_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_state_path_log_helper = lambda _bindings: "future"

        self.mod.install_state_path_log_helpers(bindings, ("future_state_path_log_helper",))

        self.assertEqual(bindings["future_state_path_log_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
