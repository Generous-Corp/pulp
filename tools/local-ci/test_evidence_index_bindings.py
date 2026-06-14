#!/usr/bin/env python3
"""Binding tests for evidence-index compatibility facade helpers."""

from __future__ import annotations

from pathlib import Path
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("evidence_index_bindings.py")
evidence_index_bindings = load_module_from_path(MODULE_PATH, module_name="evidence_index_bindings", add_module_dir=True)


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

    def test_install_evidence_index_helpers_routes_each_group_and_fallback(self) -> None:
        bindings = {"evidence_index_module": object()}

        with (
            mock.patch.object(evidence_index_bindings, "install_evidence_index_core_helpers") as core,
            mock.patch.object(evidence_index_bindings, "install_evidence_index_store_helpers") as store,
            mock.patch.object(evidence_index_bindings, "install_evidence_index_query_helpers") as query,
            mock.patch.object(evidence_index_bindings, "install_local_helpers") as install_local,
        ):
            evidence_index_bindings.install_evidence_index_helpers(
                bindings,
                (
                    "empty_evidence_index",
                    "load_evidence_index_unlocked",
                    "collect_evidence_groups_from_index",
                    "unknown_helper",
                ),
            )

        core.assert_called_once_with(bindings, ("empty_evidence_index",))
        store.assert_called_once_with(bindings, ("load_evidence_index_unlocked",))
        query.assert_called_once_with(bindings, ("collect_evidence_groups_from_index",))
        install_local.assert_called_once_with(bindings, evidence_index_bindings.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
