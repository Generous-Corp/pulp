#!/usr/bin/env python3
"""Tests for queue finalize facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_finalize_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueFinalizeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, lifecycle=None, orchestrator=None):
        bindings = {
            "_queue_lifecycle": lifecycle or types.SimpleNamespace(),
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
            "KEEP_COMPLETED_JOBS": 17,
        }
        for name in [
            "queue_lock_path",
            "file_lock",
            "load_queue_unlocked",
            "trim_completed_jobs_with_removed_ids",
            "save_queue_unlocked",
            "collect_local_ci_cleanup_plan",
            "apply_local_ci_cleanup_plan",
            "now_iso",
        ]:
            bindings[name] = object()
        return bindings

    def test_finalize_exports_match_facade_helpers(self) -> None:
        self.assertEqual(self.mod.QUEUE_FINALIZE_EXPORTS, ("finalize_job",))

    def test_finalize_job_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def finalize_job_locked(*args, **kwargs):
            captured["finalize"] = (args, kwargs)

        lifecycle = types.SimpleNamespace(finalize_job_locked=finalize_job_locked)
        bindings = self._bindings(lifecycle=lifecycle)
        result = {"overall": "pass"}
        result_path = Path("/result.json")
        deps = {"queue_lock_path_fn": object(), "keep_results": 17}

        with mock.patch.object(self.mod, "queue_finalize_dependencies", return_value=deps):
            self.mod.finalize_job(bindings, "job1", result, result_path)

        self.assertEqual(captured["finalize"][0], ("job1", result, result_path))
        kwargs = captured["finalize"][1]
        self.assertIs(kwargs["queue_lock_path_fn"], deps["queue_lock_path_fn"])
        self.assertEqual(kwargs["keep_results"], 17)

    def test_install_queue_finalize_helpers_wires_named_export(self) -> None:
        bindings = self._bindings()

        self.mod.install_queue_finalize_helpers(bindings, ("finalize_job",))

        self.assertIn("finalize_job", bindings)
        self.assertEqual(bindings["finalize_job"].__name__, "finalize_job")


if __name__ == "__main__":
    unittest.main()
