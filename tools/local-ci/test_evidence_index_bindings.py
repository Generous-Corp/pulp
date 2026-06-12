#!/usr/bin/env python3
"""Binding tests for evidence-index facade helpers."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import evidence_index_bindings  # noqa: E402


class FakeEvidenceIndex:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def empty_evidence_index(self):
        self.calls.append(("empty_evidence_index",))
        return {"version": 3, "entries": {}}

    def evidence_entry_key(self, branch, sha, target, validation):
        self.calls.append(("evidence_entry_key", branch, sha, target, validation))
        return "key"

    def normalize_evidence_index(self, index):
        self.calls.append(("normalize_evidence_index", index))
        return {"normalized": True}

    def evidence_record_from_result(self, result, item, result_path):
        self.calls.append(("evidence_record_from_result", result, item, result_path))
        return {"record": True}

    def merge_result_into_evidence_index(self, index, result, result_path):
        self.calls.append(("merge_result_into_evidence_index", index, result, result_path))
        return True

    def rebuild_evidence_index_unlocked(self):
        self.calls.append(("rebuild_evidence_index_unlocked",))
        return {"rebuilt": True}

    def load_evidence_index_unlocked(self):
        self.calls.append(("load_evidence_index_unlocked",))
        return {"loaded": True}, False

    def save_evidence_index_unlocked(self, index):
        self.calls.append(("save_evidence_index_unlocked", index))

    def collect_evidence_groups_from_index(self, index, *, branch=None, sha=None):
        self.calls.append(("collect_evidence_groups_from_index", index, branch, sha))
        return {"full": []}


class EvidenceIndexBindingTests(unittest.TestCase):
    def test_evidence_index_bindings_delegate_to_bound_module(self) -> None:
        fake = FakeEvidenceIndex()
        bindings = {"evidence_index_module": fake}
        index = {"entries": {}}
        result = {"job_id": "job"}
        item = {"target": "mac"}
        path = Path("result.json")

        self.assertEqual(evidence_index_bindings.empty_evidence_index(bindings), {"version": 3, "entries": {}})
        self.assertEqual(evidence_index_bindings.evidence_entry_key(bindings, "b", "s", "t", "full"), "key")
        self.assertEqual(evidence_index_bindings.normalize_evidence_index(bindings, index), {"normalized": True})
        self.assertEqual(
            evidence_index_bindings.evidence_record_from_result(bindings, result, item, path),
            {"record": True},
        )
        self.assertTrue(evidence_index_bindings.merge_result_into_evidence_index(bindings, index, result, path))
        self.assertEqual(evidence_index_bindings.rebuild_evidence_index_unlocked(bindings), {"rebuilt": True})
        self.assertEqual(evidence_index_bindings.load_evidence_index_unlocked(bindings), ({"loaded": True}, False))
        self.assertIsNone(evidence_index_bindings.save_evidence_index_unlocked(bindings, index))
        self.assertEqual(
            evidence_index_bindings.collect_evidence_groups_from_index(bindings, index, branch="b", sha="s"),
            {"full": []},
        )
        self.assertEqual(
            fake.calls,
            [
                ("empty_evidence_index",),
                ("evidence_entry_key", "b", "s", "t", "full"),
                ("normalize_evidence_index", index),
                ("evidence_record_from_result", result, item, path),
                ("merge_result_into_evidence_index", index, result, path),
                ("rebuild_evidence_index_unlocked",),
                ("load_evidence_index_unlocked",),
                ("save_evidence_index_unlocked", index),
                ("collect_evidence_groups_from_index", index, "b", "s"),
            ],
        )


if __name__ == "__main__":
    unittest.main()
