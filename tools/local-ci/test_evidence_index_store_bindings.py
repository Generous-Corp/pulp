#!/usr/bin/env python3
"""Tests for evidence-index persistence dependency bindings."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("evidence_index_store_bindings.py")
evidence_index_store_bindings = load_module_from_path(MODULE_PATH, module_name="evidence_index_store_bindings", add_module_dir=True)


class FakeEvidenceIndexStore:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def rebuild_evidence_index_unlocked(self):
        self.calls.append(("rebuild_evidence_index_unlocked",))
        return {"rebuilt": True}

    def load_evidence_index_unlocked(self):
        self.calls.append(("load_evidence_index_unlocked",))
        return {"loaded": True}, False

    def save_evidence_index_unlocked(self, index):
        self.calls.append(("save_evidence_index_unlocked", index))


class EvidenceIndexStoreBindingTests(unittest.TestCase):
    def test_store_exports_are_declared(self) -> None:
        self.assertEqual(
            evidence_index_store_bindings.EVIDENCE_INDEX_STORE_EXPORTS,
            (
                "rebuild_evidence_index_unlocked",
                "load_evidence_index_unlocked",
                "save_evidence_index_unlocked",
            ),
        )

    def test_store_bindings_delegate_to_bound_module(self) -> None:
        fake = FakeEvidenceIndexStore()
        bindings = {"evidence_index_module": fake}
        index = {"entries": {}}

        self.assertEqual(evidence_index_store_bindings.rebuild_evidence_index_unlocked(bindings), {"rebuilt": True})
        self.assertEqual(evidence_index_store_bindings.load_evidence_index_unlocked(bindings), ({"loaded": True}, False))
        self.assertIsNone(evidence_index_store_bindings.save_evidence_index_unlocked(bindings, index))
        self.assertEqual(
            fake.calls,
            [
                ("rebuild_evidence_index_unlocked",),
                ("load_evidence_index_unlocked",),
                ("save_evidence_index_unlocked", index),
            ],
        )

    def test_install_evidence_index_store_helpers_wires_named_exports(self) -> None:
        fake = FakeEvidenceIndexStore()
        bindings = {"evidence_index_module": fake}

        evidence_index_store_bindings.install_evidence_index_store_helpers(
            bindings,
            ("load_evidence_index_unlocked",),
        )

        self.assertEqual(bindings["load_evidence_index_unlocked"](), ({"loaded": True}, False))
        self.assertEqual(bindings["load_evidence_index_unlocked"].__name__, "load_evidence_index_unlocked")
        self.assertNotIn("save_evidence_index_unlocked", bindings)


if __name__ == "__main__":
    unittest.main()
