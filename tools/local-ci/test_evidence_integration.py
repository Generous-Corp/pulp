#!/usr/bin/env python3
"""Facade-level evidence index integration tests."""

from __future__ import annotations

import os
import pathlib
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_local_ci_evidence_integration", add_module_dir=True)


class EvidenceIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_evidence_helpers_rebuild_filter_and_skip_stale_results(self) -> None:
        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.root / "state")}, clear=True):
            self.mod.ensure_state_dirs()
            result_path = self.mod.results_dir() / "result-feature-abc123.json"
            result = {
                "job_id": "job1",
                "branch": "feature/local-ci",
                "sha": "a" * 40,
                "validation": "smoke",
                "completed_at": "2026-04-30T00:00:00+00:00",
                "provenance": {"execution_kind": "hosted", "hosted_orchestrator": "github-actions"},
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 12.5},
                    {"target": "windows", "status": "fail", "duration_secs": 2},
                ],
            }
            result_path.write_text(self.mod.json.dumps(result))
            (self.mod.results_dir() / "broken.json").write_text("{")

            index, rebuilt = self.mod.load_evidence_index_unlocked()
            self.assertTrue(rebuilt)
            self.assertEqual(len(index["entries"]), 1)
            self.assertFalse(
                self.mod.merge_result_into_evidence_index(
                    index,
                    dict(result, completed_at="2026-04-29T00:00:00+00:00"),
                    result_path,
                )
            )
            newer = dict(result, completed_at="2026-05-01T00:00:00+00:00")
            self.assertTrue(self.mod.merge_result_into_evidence_index(index, newer, result_path))

            with mock.patch.object(self.mod, "load_evidence_index", return_value=index):
                groups = self.mod.collect_evidence_groups(branch="feature/local-ci", sha="a" * 40)
            self.assertEqual(list(groups), ["smoke"])
            self.assertEqual(groups["smoke"][0]["targets"]["mac"]["status"], "pass")
            self.assertEqual(
                self.mod.evidence_entry_key("branch", "sha", "target", "full"),
                "branch:sha:full:target",
            )

            legacy_path = self.mod.evidence_path()
            legacy_path.write_text('{"version": 1, "entries": []}')
            index, rebuilt = self.mod.load_evidence_index_unlocked()
            self.assertTrue(rebuilt)
            self.assertEqual(index["version"], 3)



if __name__ == "__main__":
    unittest.main()
