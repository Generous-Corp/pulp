#!/usr/bin/env python3
"""Tests for queue runner active-target dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_runner_active_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueRunnerActiveBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_runner_active_exports_match_facade_helpers(self):
        expected = ("update_runner_active_targets",)

        self.assertEqual(self.mod.QUEUE_RUNNER_ACTIVE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

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

    def test_install_queue_runner_active_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_runner_active_helpers(bindings, ("update_runner_active_targets", "custom_runner_active"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("update_runner_active_targets",)),
                mock.call(bindings, self.mod.__dict__, ("custom_runner_active",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
