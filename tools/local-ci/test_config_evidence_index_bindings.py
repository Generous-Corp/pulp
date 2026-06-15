#!/usr/bin/env python3
"""Tests for config evidence index dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
from pathlib import Path
import types
import unittest



def load_module():
    return load_local_ci_module("config_evidence_index_bindings.py")


class ConfigEvidenceIndexBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, **overrides):
        bindings = {
            "evidence_index_module": types.SimpleNamespace(),
            "load_evidence_index": lambda: {"version": 3, "entries": {}},
            "collect_evidence_groups_from_index": lambda index, **kwargs: {
                "index": index,
                "filters": kwargs,
            },
        }
        bindings.update(overrides)
        return bindings

    def test_index_exports_match_wrappers(self) -> None:
        expected = (
            "load_evidence_index",
            "update_evidence_index",
            "collect_evidence_groups",
        )

        self.assertEqual(self.mod.CONFIG_EVIDENCE_INDEX_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_index_wrappers_preserve_facade_monkeypatch_seams(self) -> None:
        calls = {}
        evidence_module = types.SimpleNamespace(
            load_evidence_index=lambda: {"loaded": True},
            update_evidence_index=lambda result, path: calls.setdefault("update", (result, path)),
        )
        bindings = self._bindings(
            evidence_index_module=evidence_module,
            load_evidence_index=lambda: {"facade": True},
        )

        self.assertEqual(self.mod.load_evidence_index(bindings), {"loaded": True})
        self.mod.update_evidence_index(bindings, {"job": "1"}, Path("/tmp/result.json"))
        self.assertEqual(calls["update"], ({"job": "1"}, Path("/tmp/result.json")))

        self.assertEqual(
            self.mod.collect_evidence_groups(bindings, branch="feature/a", sha="abc"),
            {"index": {"facade": True}, "filters": {"branch": "feature/a", "sha": "abc"}},
        )

    def test_install_config_evidence_index_helpers_wires_named_exports(self) -> None:
        calls = {}
        evidence_module = types.SimpleNamespace(
            update_evidence_index=lambda result, path: calls.setdefault("update", (result, path)),
        )
        bindings = self._bindings(evidence_index_module=evidence_module)

        self.mod.install_config_evidence_index_helpers(bindings, ("update_evidence_index",))

        bindings["update_evidence_index"]({"job": "1"}, Path("/tmp/result.json"))
        self.assertEqual(calls["update"], ({"job": "1"}, Path("/tmp/result.json")))
        self.assertEqual(bindings["update_evidence_index"].__name__, "update_evidence_index")


if __name__ == "__main__":
    unittest.main()
