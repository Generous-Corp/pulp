#!/usr/bin/env python3
"""Tests for config evidence summary facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("config_evidence_summary_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ConfigEvidenceSummaryBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, **overrides):
        bindings = {
            "evidence_index_module": types.SimpleNamespace(),
            "load_evidence_index": lambda: {"version": 3, "entries": {}},
            "collect_evidence_groups": lambda **_kwargs: {"full": []},
            "collect_evidence_groups_from_index": lambda index, **kwargs: {
                "index": index,
                "filters": kwargs,
            },
        }
        bindings.update(overrides)
        return bindings

    def test_evidence_wrappers_preserve_facade_monkeypatch_seams(self) -> None:
        calls = {}

        evidence_module = types.SimpleNamespace(
            load_evidence_index=lambda: {"loaded": True},
            update_evidence_index=lambda result, path: calls.setdefault("update", (result, path)),
            print_evidence_summary_from_groups=lambda groups, **kwargs: calls.setdefault(
                "summary",
                (groups, kwargs),
            )
            or True,
            evidence_scope_header_line=lambda branch, sha: f"{branch}:{sha}",
            evidence_empty_line=lambda *, has_header: "empty-with-header" if has_header else "empty",
        )
        bindings = self._bindings(
            evidence_index_module=evidence_module,
            load_evidence_index=lambda: {"facade": True},
        )

        self.assertEqual(self.mod.load_evidence_index(bindings), {"loaded": True})
        self.mod.update_evidence_index(bindings, {"job": "1"}, Path("/tmp/result.json"))
        self.assertEqual(calls["update"], ({"job": "1"}, Path("/tmp/result.json")))

        groups = self.mod.collect_evidence_groups(bindings, branch="feature/a", sha="abc")
        self.assertEqual(groups, {"index": {"facade": True}, "filters": {"branch": "feature/a", "sha": "abc"}})

        self.assertTrue(self.mod.print_evidence_summary(bindings, branch="feature/a", limit=2, indent="  "))
        self.assertEqual(
            calls["summary"],
            ({"full": []}, {"limit": 2, "indent": "  "}),
        )
        self.assertEqual(self.mod.evidence_scope_header_line(bindings, "feature/a", None), "feature/a:None")
        self.assertEqual(self.mod.evidence_empty_line(bindings, has_header=True), "empty-with-header")

    def test_install_config_evidence_summary_helpers_wires_named_exports(self) -> None:
        calls = {}

        def evidence_empty_line(*, has_header):
            calls["has_header"] = has_header
            return "empty-with-header" if has_header else "empty"

        evidence_module = types.SimpleNamespace(
            evidence_empty_line=evidence_empty_line,
        )
        bindings = self._bindings(evidence_index_module=evidence_module)

        self.mod.install_config_evidence_summary_helpers(bindings, ("evidence_empty_line",))

        self.assertEqual(bindings["evidence_empty_line"](has_header=True), "empty-with-header")
        self.assertEqual(calls["has_header"], True)
        self.assertEqual(bindings["evidence_empty_line"].__name__, "evidence_empty_line")


if __name__ == "__main__":
    unittest.main()
