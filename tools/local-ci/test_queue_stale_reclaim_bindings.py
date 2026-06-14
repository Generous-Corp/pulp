#!/usr/bin/env python3
"""Tests for stale remote validator reclaim bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_stale_reclaim_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStaleReclaimBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_stale_reclaim_exports_match_wrappers(self):
        expected = ("reclaim_stale_remote_validators",)

        self.assertEqual(self.mod.QUEUE_STALE_RECLAIM_EXPORTS, expected)
        self.assertTrue(callable(self.mod.reclaim_stale_remote_validators))

    def test_reclaim_stale_remote_validators_delegates_with_assembled_dependencies(self):
        captured = {}

        def reclaim_stale_remote_validators_locked(**kwargs):
            captured["reclaim"] = kwargs
            return 2

        lifecycle = types.SimpleNamespace(reclaim_stale_remote_validators_locked=reclaim_stale_remote_validators_locked)
        bindings = {
            "_queue_lifecycle": lifecycle,
            "queue_lock_path": object(),
            "file_lock": object(),
            "load_queue_unlocked": object(),
            "collect_stale_windows_cleanup_candidates_unlocked": object(),
            "save_queue_unlocked": object(),
            "cleanup_stale_windows_validator": object(),
            "update_job_target_state": object(),
            "now_iso": object(),
            "trim_line": object(),
        }
        deps = {"queue_lock_path_fn": object(), "cleanup_validator_fn": object()}

        with mock.patch.object(self.mod, "queue_stale_reclaim_dependencies", return_value=deps):
            self.assertEqual(self.mod.reclaim_stale_remote_validators(bindings, {"targets": {}}), 2)
        self.assertIs(captured["reclaim"]["queue_lock_path_fn"], deps["queue_lock_path_fn"])
        self.assertIs(captured["reclaim"]["cleanup_validator_fn"], deps["cleanup_validator_fn"])

    def test_install_queue_stale_reclaim_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_stale_reclaim_helpers(bindings, ("reclaim_stale_remote_validators", "custom_reclaim"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("reclaim_stale_remote_validators",)),
                mock.call(bindings, self.mod.__dict__, ("custom_reclaim",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
