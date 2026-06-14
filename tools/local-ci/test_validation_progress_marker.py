#!/usr/bin/env python3
"""No-network tests for validation progress marker parsing."""

from __future__ import annotations

import pathlib
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("validation_progress_marker.py")


class ValidationProgressMarkerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_parse_progress_marker_detects_progress_contract(self) -> None:
        self.assertEqual(self.mod.parse_progress_marker("__PULP_PHASE__:build\n"), {"phase": "build"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_WAIT__:host-lock\n"), {"wait_reason": "host-lock"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_VALIDATION__:smoke\n"), {"validation_mode": "smoke"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_TEST_POLICY__:skip\n"), {"test_policy": "skip"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_PREPARED__:reused\n"), {"prepared_state": "reused"})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_VALIDATOR_PID__:4321\n"), {"validator_pid": 4321})
        self.assertEqual(self.mod.parse_progress_marker("__PULP_VALIDATOR_PID__:bad\n"), {"validator_pid": "bad"})
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_VALIDATOR_STARTED__:2026-04-02T04:00:00+00:00\n"),
            {"validator_started_at": "2026-04-02T04:00:00+00:00"},
        )
        self.assertEqual(self.mod.parse_progress_marker("normal output\n"), {})


if __name__ == "__main__":
    unittest.main()
