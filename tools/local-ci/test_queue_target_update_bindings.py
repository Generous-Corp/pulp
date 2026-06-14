#!/usr/bin/env python3
"""Tests for queue target-state update bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_target_update_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueTargetUpdateBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_target_update_exports_match_wrappers(self):
        expected = ("update_job_target_state",)

        self.assertEqual(self.mod.QUEUE_TARGET_UPDATE_EXPORTS, expected)
        self.assertTrue(callable(self.mod.update_job_target_state))

    def test_update_job_target_state_binds_locked_queue_dependencies(self):
        captured = {}

        def update_job_target_state_locked(*args, **kwargs):
            captured["target_state"] = (args, kwargs)

        def update_job_target_state_unlocked(queue, job_id, target_name, fields, *, now_iso_fn):
            captured["target_unlocked"] = (queue, job_id, target_name, fields, now_iso_fn)
            return True

        lifecycle = types.SimpleNamespace(update_job_target_state_locked=update_job_target_state_locked)
        orchestrator = types.SimpleNamespace(update_job_target_state_unlocked=update_job_target_state_unlocked)
        bindings = {
            "_queue_lifecycle": lifecycle,
            "_queue_orchestrator": orchestrator,
            "queue_lock_path": object(),
            "file_lock": object(),
            "load_queue_unlocked": object(),
            "save_queue_unlocked": object(),
            "now_iso": object(),
        }

        self.mod.update_job_target_state(bindings, "job1", "mac", status="pass")
        self.assertEqual(captured["target_state"][0], ("job1", "mac", {"status": "pass"}))
        self.assertIs(captured["target_state"][1]["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(captured["target_state"][1]["file_lock_fn"], bindings["file_lock"])
        self.assertIs(captured["target_state"][1]["load_queue_unlocked_fn"], bindings["load_queue_unlocked"])
        self.assertIs(captured["target_state"][1]["save_queue_unlocked_fn"], bindings["save_queue_unlocked"])

        captured["target_state"][1]["update_job_target_state_unlocked_fn"]([], "job1", "mac", {"status": "fail"})
        self.assertEqual(captured["target_unlocked"], ([], "job1", "mac", {"status": "fail"}, bindings["now_iso"]))

    def test_install_queue_target_update_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_target_update_helpers(bindings, ("update_job_target_state", "custom_target_update"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("update_job_target_state",)),
                mock.call(bindings, self.mod.__dict__, ("custom_target_update",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
