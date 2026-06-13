#!/usr/bin/env python3
"""Tests for queue runner-state facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_runner_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueRunnerBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_runner_exports_match_facade_helpers(self):
        expected = (
            "read_runner_info",
            "pid_alive",
            "current_runner_info",
            "stale_running_jobs_unlocked",
            "write_runner_info",
            "update_runner_active_targets",
            "clear_runner_info",
        )

        self.assertEqual(self.mod.QUEUE_RUNNER_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_queue_runner_bindings_delegate_to_runner_state(self):
        calls = []

        runner_state = types.SimpleNamespace(
            read_runner_info=lambda: calls.append("read") or {"pid": 123},
            pid_alive=lambda pid: calls.append(("pid", pid)) or True,
            current_runner_info=lambda: calls.append("current") or {"pid": 456},
            stale_running_jobs_for_current_runner=lambda queue, **kwargs: calls.append(("stale", queue, kwargs)) or [{"id": "old"}],
            write_runner_info=lambda info: calls.append(("write", info)),
            clear_runner_info=lambda: calls.append("clear"),
        )
        orchestrator = types.SimpleNamespace(stale_running_jobs_for_runner_unlocked=object())
        bindings = {
            "_runner_state": runner_state,
            "_queue_orchestrator": orchestrator,
        }

        self.assertEqual(self.mod.read_runner_info(bindings), {"pid": 123})
        self.assertTrue(self.mod.pid_alive(bindings, 789))
        self.assertEqual(self.mod.current_runner_info(bindings), {"pid": 456})
        self.assertEqual(self.mod.stale_running_jobs_unlocked(bindings, [{"id": "run"}]), [{"id": "old"}])
        self.mod.write_runner_info(bindings, {"pid": 1})
        self.mod.clear_runner_info(bindings)

        self.assertEqual(calls[0:3], ["read", ("pid", 789), "current"])
        self.assertEqual(calls[3][0], "stale")
        self.assertIs(calls[3][2]["stale_running_jobs_for_runner_unlocked_fn"], orchestrator.stale_running_jobs_for_runner_unlocked)
        self.assertEqual(calls[4:], [("write", {"pid": 1}), "clear"])

    def test_update_runner_active_targets_binds_orchestrator_mutator(self):
        captured = {}

        def update_current_runner_active_targets(*args, **kwargs):
            captured["runner"] = (args, kwargs)

        def update_runner_info_active_targets(info, job_id, active_targets, *, now_iso_fn):
            captured["mutate"] = (info, job_id, active_targets, now_iso_fn)
            return True

        bindings = {
            "_runner_state": types.SimpleNamespace(update_current_runner_active_targets=update_current_runner_active_targets),
            "_queue_orchestrator": types.SimpleNamespace(update_runner_info_active_targets=update_runner_info_active_targets),
            "now_iso": object(),
        }

        self.mod.update_runner_active_targets(bindings, "job1", {"mac": {"status": "pass"}})

        self.assertEqual(captured["runner"][0], ("job1", {"mac": {"status": "pass"}}))
        update_info = captured["runner"][1]["update_runner_info_active_targets_fn"]
        self.assertTrue(update_info({"pid": 1}, "job1", {"mac": {"status": "pass"}}))
        self.assertIs(captured["mutate"][3], bindings["now_iso"])

    def test_install_queue_runner_helpers_wires_named_exports(self):
        calls = []
        bindings = {
            "_runner_state": types.SimpleNamespace(
                read_runner_info=lambda: calls.append("read") or {"pid": 1},
                clear_runner_info=lambda: calls.append("clear"),
            ),
        }

        self.mod.install_queue_runner_helpers(bindings, ("read_runner_info", "clear_runner_info"))

        self.assertEqual(bindings["read_runner_info"](), {"pid": 1})
        bindings["clear_runner_info"]()
        self.assertEqual(calls, ["read", "clear"])


if __name__ == "__main__":
    unittest.main()
