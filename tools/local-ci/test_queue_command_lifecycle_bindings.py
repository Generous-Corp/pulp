#!/usr/bin/env python3
"""Tests for queue command lifecycle facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_command_lifecycle_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueCommandLifecycleBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_command_lifecycle_exports_match_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_TERMINAL_RESULT_EXPORTS,
            *self.mod.QUEUE_COMMAND_MUTATION_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_COMMAND_LIFECYCLE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_queue_command_lifecycle_helpers_routes_focused_groups(self):
        calls = []

        def terminal_install(bindings, names):
            calls.append(("terminal", names))

        def command_install(bindings, names):
            calls.append(("command", names))

        self.mod.install_queue_terminal_result_helpers = terminal_install
        self.mod.install_queue_command_mutation_helpers = command_install

        self.mod.install_queue_command_lifecycle_helpers(
            {},
            ("cancel_job_unlocked", "bump_queue_command_job"),
        )

        self.assertEqual(
            calls,
            [
                ("terminal", ("cancel_job_unlocked",)),
                ("command", ("bump_queue_command_job",)),
            ],
        )

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
            "supersedence_result",
            "cancellation_result",
            "save_result",
            "cancel_job_unlocked",
            "trim_completed_jobs",
            "summarize_job",
            "now_iso",
        ]:
            bindings[name] = object()
        return bindings

    def test_supersede_and_cancel_job_bind_completion_dependencies(self):
        captured = {}

        def complete_superseded_job_unlocked(*args, **kwargs):
            captured["supersede"] = (args, kwargs)

        def complete_canceled_job_unlocked(*args, **kwargs):
            captured["cancel"] = (args, kwargs)

        lifecycle = types.SimpleNamespace(
            complete_superseded_job_unlocked=complete_superseded_job_unlocked,
            complete_canceled_job_unlocked=complete_canceled_job_unlocked,
        )
        orchestrator = types.SimpleNamespace(complete_job_with_result_unlocked=object())
        bindings = self._bindings(lifecycle=lifecycle, orchestrator=orchestrator)

        self.mod.supersede_job_unlocked(bindings, {"id": "old"}, "new", "newer_sha")
        self.mod.cancel_job_unlocked(bindings, {"id": "old"}, "operator")

        self.assertEqual(captured["supersede"][0], ({"id": "old"}, "new", "newer_sha"))
        self.assertIs(captured["supersede"][1]["supersedence_result_fn"], bindings["supersedence_result"])
        self.assertIs(captured["supersede"][1]["save_result_fn"], bindings["save_result"])
        self.assertIs(
            captured["supersede"][1]["complete_job_with_result_unlocked_fn"],
            orchestrator.complete_job_with_result_unlocked,
        )

        self.assertEqual(captured["cancel"][0], ({"id": "old"}, "operator"))
        self.assertIs(captured["cancel"][1]["cancellation_result_fn"], bindings["cancellation_result"])
        self.assertIs(captured["cancel"][1]["save_result_fn"], bindings["save_result"])
        self.assertIs(
            captured["cancel"][1]["complete_job_with_result_unlocked_fn"],
            orchestrator.complete_job_with_result_unlocked,
        )

    def test_queue_command_mutation_binds_lock_and_now_dependencies(self):
        captured = {}

        def bump_queue_command_job_locked(*args, **kwargs):
            captured["bump"] = (args, kwargs)
            return {"id": "job1", "priority": "high"}

        def cancel_queue_command_job_locked(*args, **kwargs):
            captured["cancel"] = (args, kwargs)
            return {"id": "job1", "status": "canceled"}

        def set_pending_job_priority_unlocked(job, priority, *, now_iso_fn):
            captured["set_priority"] = (job, priority, now_iso_fn)
            return True

        lifecycle = types.SimpleNamespace(
            bump_queue_command_job_locked=bump_queue_command_job_locked,
            cancel_queue_command_job_locked=cancel_queue_command_job_locked,
        )
        orchestrator = types.SimpleNamespace(
            find_queue_command_job_unlocked=object(),
            set_pending_job_priority_unlocked=set_pending_job_priority_unlocked,
        )
        bindings = self._bindings(lifecycle=lifecycle, orchestrator=orchestrator)

        self.assertEqual(
            self.mod.bump_queue_command_job(bindings, "job1", "high"),
            {"id": "job1", "priority": "high"},
        )
        self.assertEqual(captured["bump"][0], ("job1", "high"))
        self.assertIs(captured["bump"][1]["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(captured["bump"][1]["find_queue_command_job_unlocked_fn"], orchestrator.find_queue_command_job_unlocked)
        self.assertTrue(captured["bump"][1]["set_pending_job_priority_unlocked_fn"]({"id": "job1"}, "high"))
        self.assertEqual(captured["set_priority"], ({"id": "job1"}, "high", bindings["now_iso"]))

        self.assertEqual(
            self.mod.cancel_queue_command_job(bindings, "job1"),
            {"id": "job1", "status": "canceled"},
        )
        self.assertEqual(captured["cancel"][0], ("job1",))
        self.assertIs(captured["cancel"][1]["find_queue_command_job_unlocked_fn"], orchestrator.find_queue_command_job_unlocked)
        self.assertIs(captured["cancel"][1]["cancel_job_unlocked_fn"], bindings["cancel_job_unlocked"])
        self.assertIs(captured["cancel"][1]["trim_completed_jobs_fn"], bindings["trim_completed_jobs"])
        self.assertIs(captured["cancel"][1]["summarize_job_fn"], bindings["summarize_job"])


if __name__ == "__main__":
    unittest.main()
