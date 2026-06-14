#!/usr/bin/env python3
"""Tests for queue target-state payload dependency helpers."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("queue_target_payload_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueTargetPayloadDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_target_log_path_binds_facade_log_path_helper(self) -> None:
        bindings = {"target_log_path": lambda job_id, target: Path(f"/logs/{job_id}-{target}.log")}

        self.assertEqual(self.mod.queue_target_log_path(bindings, "job1", "mac"), "/logs/job1-mac.log")


if __name__ == "__main__":
    unittest.main()
