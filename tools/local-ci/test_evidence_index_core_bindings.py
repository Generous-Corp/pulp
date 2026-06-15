#!/usr/bin/env python3
"""Tests for evidence-index core dependency bindings."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module


evidence_index_core_bindings = load_local_ci_module("evidence_index_core_bindings.py", module_name="evidence_index_core_bindings", add_module_dir=True)


class FakeEvidenceIndexCore:
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


class EvidenceIndexCoreBindingTests(unittest.TestCase):
    def test_core_exports_are_declared(self) -> None:
        self.assertEqual(
            evidence_index_core_bindings.EVIDENCE_INDEX_CORE_EXPORTS,
            (
                "empty_evidence_index",
                "evidence_entry_key",
                "normalize_evidence_index",
                "evidence_record_from_result",
                "merge_result_into_evidence_index",
            ),
        )

    def test_core_bindings_delegate_to_bound_module(self) -> None:
        fake = FakeEvidenceIndexCore()
        bindings = {"evidence_index_module": fake}
        index = {"entries": {}}
        result = {"job_id": "job"}
        item = {"target": "mac"}
        path = Path("result.json")

        self.assertEqual(evidence_index_core_bindings.empty_evidence_index(bindings), {"version": 3, "entries": {}})
        self.assertEqual(evidence_index_core_bindings.evidence_entry_key(bindings, "b", "s", "t", "full"), "key")
        self.assertEqual(evidence_index_core_bindings.normalize_evidence_index(bindings, index), {"normalized": True})
        self.assertEqual(
            evidence_index_core_bindings.evidence_record_from_result(bindings, result, item, path),
            {"record": True},
        )
        self.assertTrue(evidence_index_core_bindings.merge_result_into_evidence_index(bindings, index, result, path))
        self.assertEqual(
            fake.calls,
            [
                ("empty_evidence_index",),
                ("evidence_entry_key", "b", "s", "t", "full"),
                ("normalize_evidence_index", index),
                ("evidence_record_from_result", result, item, path),
                ("merge_result_into_evidence_index", index, result, path),
            ],
        )

    def test_install_evidence_index_core_helpers_wires_named_exports(self) -> None:
        fake = FakeEvidenceIndexCore()
        bindings = {"evidence_index_module": fake}

        evidence_index_core_bindings.install_evidence_index_core_helpers(bindings, ("empty_evidence_index",))

        self.assertEqual(bindings["empty_evidence_index"](), {"version": 3, "entries": {}})
        self.assertEqual(bindings["empty_evidence_index"].__name__, "empty_evidence_index")
        self.assertNotIn("normalize_evidence_index", bindings)


if __name__ == "__main__":
    unittest.main()
