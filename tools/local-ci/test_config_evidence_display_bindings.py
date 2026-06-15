#!/usr/bin/env python3
"""Tests for config evidence display dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("config_evidence_display_bindings.py")


class ConfigEvidenceDisplayBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, **overrides):
        bindings = {
            "evidence_index_module": types.SimpleNamespace(),
            "collect_evidence_groups": lambda **_kwargs: {"full": []},
        }
        bindings.update(overrides)
        return bindings

    def test_display_exports_match_wrappers(self) -> None:
        expected = (
            "print_evidence_summary",
            "evidence_scope_header_line",
            "evidence_empty_line",
        )

        self.assertEqual(self.mod.CONFIG_EVIDENCE_DISPLAY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_display_wrappers_preserve_facade_monkeypatch_seams(self) -> None:
        calls = {}
        evidence_module = types.SimpleNamespace(
            print_evidence_summary_from_groups=lambda groups, **kwargs: calls.setdefault("summary", (groups, kwargs)) or True,
            evidence_scope_header_line=lambda branch, sha: f"{branch}:{sha}",
            evidence_empty_line=lambda *, has_header: "empty-with-header" if has_header else "empty",
        )
        bindings = self._bindings(evidence_index_module=evidence_module)

        self.assertTrue(self.mod.print_evidence_summary(bindings, branch="feature/a", limit=2, indent="  "))
        self.assertEqual(calls["summary"], ({"full": []}, {"limit": 2, "indent": "  "}))
        self.assertEqual(self.mod.evidence_scope_header_line(bindings, "feature/a", None), "feature/a:None")
        self.assertEqual(self.mod.evidence_empty_line(bindings, has_header=True), "empty-with-header")

    def test_install_config_evidence_display_helpers_wires_named_exports(self) -> None:
        calls = {}

        def evidence_empty_line(*, has_header):
            calls["has_header"] = has_header
            return "empty-with-header" if has_header else "empty"

        evidence_module = types.SimpleNamespace(evidence_empty_line=evidence_empty_line)
        bindings = self._bindings(evidence_index_module=evidence_module)

        self.mod.install_config_evidence_display_helpers(bindings, ("evidence_empty_line",))

        self.assertEqual(bindings["evidence_empty_line"](has_header=True), "empty-with-header")
        self.assertEqual(calls["has_header"], True)
        self.assertEqual(bindings["evidence_empty_line"].__name__, "evidence_empty_line")


if __name__ == "__main__":
    unittest.main()
