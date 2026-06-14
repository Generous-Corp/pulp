#!/usr/bin/env python3
"""Tests for queue command mutation facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_command_mutation_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueCommandMutationBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_command_mutation_exports_match_facade_helpers(self):
        expected = (
            "bump_queue_command_job",
            "cancel_queue_command_job",
        )

        self.assertEqual(self.mod.QUEUE_COMMAND_MUTATION_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def _bindings(self, lifecycle=None, orchestrator=None):
        bindings = {
            "_queue_lifecycle": lifecycle or types.SimpleNamespace(),
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
        }
        for name in [
            "queue_lock_path",
            "file_lock",
            "load_queue_unlocked",
            "save_queue_unlocked",
            "cancel_job_unlocked",
            "trim_completed_jobs",
            "summarize_job",
            "now_iso",
        ]:
            bindings[name] = object()
        return bindings

    def test_queue_command_mutation_delegates_with_assembled_dependencies(self):
        captured = {}

        def bump_queue_command_job_locked(*args, **kwargs):
            captured["bump"] = (args, kwargs)
            return {"id": "job1", "priority": "high"}

        def cancel_queue_command_job_locked(*args, **kwargs):
            captured["cancel"] = (args, kwargs)
            return {"id": "job1", "status": "canceled"}

        lifecycle = types.SimpleNamespace(
            bump_queue_command_job_locked=bump_queue_command_job_locked,
            cancel_queue_command_job_locked=cancel_queue_command_job_locked,
        )
        bindings = self._bindings(lifecycle=lifecycle)
        bump_deps = {"queue_lock_path_fn": object(), "summarize_job_fn": object()}
        cancel_deps = {"cancel_job_unlocked_fn": object(), "trim_completed_jobs_fn": object()}

        with mock.patch.object(self.mod, "queue_bump_command_dependencies", return_value=bump_deps):
            self.assertEqual(
                self.mod.bump_queue_command_job(bindings, "job1", "high"),
                {"id": "job1", "priority": "high"},
            )
        self.assertEqual(captured["bump"][0], ("job1", "high"))
        self.assertIs(captured["bump"][1]["queue_lock_path_fn"], bump_deps["queue_lock_path_fn"])
        self.assertIs(captured["bump"][1]["summarize_job_fn"], bump_deps["summarize_job_fn"])

        with mock.patch.object(self.mod, "queue_cancel_command_dependencies", return_value=cancel_deps):
            self.assertEqual(
                self.mod.cancel_queue_command_job(bindings, "job1"),
                {"id": "job1", "status": "canceled"},
            )
        self.assertEqual(captured["cancel"][0], ("job1",))
        self.assertIs(captured["cancel"][1]["cancel_job_unlocked_fn"], cancel_deps["cancel_job_unlocked_fn"])
        self.assertIs(captured["cancel"][1]["trim_completed_jobs_fn"], cancel_deps["trim_completed_jobs_fn"])

    def test_install_queue_command_mutation_helpers_installs_requested_facades(self):
        bindings = self._bindings()

        self.mod.install_queue_command_mutation_helpers(bindings, ("bump_queue_command_job",))

        self.assertIn("bump_queue_command_job", bindings)
        self.assertNotIn("cancel_queue_command_job", bindings)
        self.assertEqual(bindings["bump_queue_command_job"].__name__, "bump_queue_command_job")


if __name__ == "__main__":
    unittest.main()
