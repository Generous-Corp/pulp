#!/usr/bin/env python3
"""Tests for evidence index persistence helpers."""

from __future__ import annotations

import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("evidence_index_store.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_evidence_index_store", add_module_dir=True)


class EvidenceIndexStoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_update_evidence_index_keeps_latest_passing_target_per_branch_sha_validation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            state_dir = Path(tmp) / "state"
            result_path_one = state_dir / "results" / "one.json"
            result_path_two = state_dir / "results" / "two.json"
            result_path_one.parent.mkdir(parents=True)
            with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(state_dir)}, clear=True):
                self.mod.update_evidence_index(
                    {
                        "job_id": "job111",
                        "branch": "feature/evidence",
                        "sha": "1" * 40,
                        "validation": "full",
                        "completed_at": "2026-04-01T00:10:00+00:00",
                        "results": [
                            {"target": "mac", "status": "pass", "duration_secs": 10.0},
                            {"target": "ubuntu", "status": "fail", "duration_secs": 20.0},
                        ],
                    },
                    result_path_one,
                )
                self.mod.update_evidence_index(
                    {
                        "job_id": "job112",
                        "branch": "feature/evidence",
                        "sha": "1" * 40,
                        "validation": "full",
                        "completed_at": "2026-04-01T00:20:00+00:00",
                        "results": [{"target": "ubuntu", "status": "pass", "duration_secs": 12.0}],
                    },
                    result_path_two,
                )

                index = self.mod.load_evidence_index()

        self.assertIn("feature/evidence:" + "1" * 40 + ":full:mac", index["entries"])
        self.assertEqual(index["entries"]["feature/evidence:" + "1" * 40 + ":full:ubuntu"]["job_id"], "job112")


if __name__ == "__main__":
    unittest.main()
