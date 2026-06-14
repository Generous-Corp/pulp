#!/usr/bin/env python3
"""Binding tests for evidence-index compatibility facade helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("evidence_index_bindings.py")
evidence_index_bindings = load_module_from_path(MODULE_PATH, module_name="evidence_index_bindings", add_module_dir=True)


class FakeEvidenceIndex:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def empty_evidence_index(self):
        self.calls.append(("empty_evidence_index",))
        return {"version": 3, "entries": {}}

    def collect_evidence_groups_from_index(self, index, *, branch=None, sha=None):
        self.calls.append(("collect_evidence_groups_from_index", index, branch, sha))
        return {"full": []}


class EvidenceIndexBindingTests(unittest.TestCase):
    def test_facade_reexports_core_store_and_query_helpers(self) -> None:
        expected_exports = (
            *evidence_index_bindings.EVIDENCE_INDEX_CORE_EXPORTS,
            *evidence_index_bindings.EVIDENCE_INDEX_STORE_EXPORTS,
            *evidence_index_bindings.EVIDENCE_INDEX_QUERY_EXPORTS,
        )

        self.assertEqual(evidence_index_bindings.EVIDENCE_INDEX_EXPORTS, expected_exports)
        self.assertEqual(len(expected_exports), len(set(expected_exports)))
        for name in expected_exports:
            self.assertTrue(callable(getattr(evidence_index_bindings, name)))

    def test_install_evidence_index_helpers_wires_named_exports(self) -> None:
        fake = FakeEvidenceIndex()
        bindings = {"evidence_index_module": fake}
        index = {"entries": {}}

        evidence_index_bindings.install_evidence_index_helpers(
            bindings,
            ("empty_evidence_index", "collect_evidence_groups_from_index"),
        )

        self.assertEqual(bindings["empty_evidence_index"](), {"version": 3, "entries": {}})
        self.assertEqual(
            bindings["collect_evidence_groups_from_index"](index, branch="b", sha="s"),
            {"full": []},
        )
        self.assertEqual(bindings["empty_evidence_index"].__name__, "empty_evidence_index")
        self.assertEqual([call[0] for call in fake.calls], ["empty_evidence_index", "collect_evidence_groups_from_index"])

    def test_install_evidence_index_helpers_routes_each_group(self) -> None:
        class FakeEvidenceIndexAll(FakeEvidenceIndex):
            def load_evidence_index_unlocked(self):
                self.calls.append(("load_evidence_index_unlocked",))
                return {"loaded": True}, False

        fake = FakeEvidenceIndexAll()
        bindings = {"evidence_index_module": fake}
        index = {"entries": {}}

        evidence_index_bindings.install_evidence_index_helpers(
            bindings,
            (
                "empty_evidence_index",
                "load_evidence_index_unlocked",
                "collect_evidence_groups_from_index",
            ),
        )

        self.assertEqual(bindings["empty_evidence_index"](), {"version": 3, "entries": {}})
        self.assertEqual(bindings["load_evidence_index_unlocked"](), ({"loaded": True}, False))
        self.assertEqual(bindings["collect_evidence_groups_from_index"](index), {"full": []})
        self.assertNotIn("normalize_evidence_index", bindings)
        self.assertNotIn("save_evidence_index_unlocked", bindings)
        self.assertEqual(
            [call[0] for call in fake.calls],
            ["empty_evidence_index", "load_evidence_index_unlocked", "collect_evidence_groups_from_index"],
        )


if __name__ == "__main__":
    unittest.main()
