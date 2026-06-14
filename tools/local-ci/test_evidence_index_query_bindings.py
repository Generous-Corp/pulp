#!/usr/bin/env python3
"""Tests for evidence-index query dependency bindings."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("evidence_index_query_bindings.py")
evidence_index_query_bindings = load_module_from_path(MODULE_PATH, module_name="evidence_index_query_bindings", add_module_dir=True)


class FakeEvidenceIndexQuery:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def collect_evidence_groups_from_index(self, index, *, branch=None, sha=None):
        self.calls.append(("collect_evidence_groups_from_index", index, branch, sha))
        return {"full": []}


class EvidenceIndexQueryBindingTests(unittest.TestCase):
    def test_query_exports_are_declared(self) -> None:
        self.assertEqual(
            evidence_index_query_bindings.EVIDENCE_INDEX_QUERY_EXPORTS,
            ("collect_evidence_groups_from_index",),
        )

    def test_query_bindings_delegate_to_bound_module(self) -> None:
        fake = FakeEvidenceIndexQuery()
        bindings = {"evidence_index_module": fake}
        index = {"entries": {}}

        self.assertEqual(
            evidence_index_query_bindings.collect_evidence_groups_from_index(bindings, index, branch="b", sha="s"),
            {"full": []},
        )
        self.assertEqual(fake.calls, [("collect_evidence_groups_from_index", index, "b", "s")])

    def test_install_evidence_index_query_helpers_wires_named_exports(self) -> None:
        fake = FakeEvidenceIndexQuery()
        bindings = {"evidence_index_module": fake}
        index = {"entries": {}}

        evidence_index_query_bindings.install_evidence_index_query_helpers(
            bindings,
            ("collect_evidence_groups_from_index",),
        )

        self.assertEqual(
            bindings["collect_evidence_groups_from_index"](index, branch="b", sha="s"),
            {"full": []},
        )
        self.assertEqual(bindings["collect_evidence_groups_from_index"].__name__, "collect_evidence_groups_from_index")


if __name__ == "__main__":
    unittest.main()
