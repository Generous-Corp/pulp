#!/usr/bin/env python3
"""Tests for queue terminal result facade bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("queue_terminal_result_bindings.py")


class QueueTerminalResultBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_terminal_result_exports_match_facade_helpers(self):
        expected = (
            "supersede_job_unlocked",
            "cancel_job_unlocked",
        )

        self.assertEqual(self.mod.QUEUE_TERMINAL_RESULT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def _bindings(self, lifecycle=None, orchestrator=None):
        return {
            "_queue_lifecycle": lifecycle or types.SimpleNamespace(),
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
            "supersedence_result": object(),
            "cancellation_result": object(),
            "save_result": object(),
        }

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

    def test_install_queue_terminal_result_helpers_installs_requested_facades(self):
        bindings = self._bindings()

        self.mod.install_queue_terminal_result_helpers(bindings, ("cancel_job_unlocked",))

        self.assertIn("cancel_job_unlocked", bindings)
        self.assertNotIn("supersede_job_unlocked", bindings)
        self.assertEqual(bindings["cancel_job_unlocked"].__name__, "cancel_job_unlocked")


if __name__ == "__main__":
    unittest.main()
