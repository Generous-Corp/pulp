#!/usr/bin/env python3
"""Tests for desktop proof listing dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_proof_list_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopProofListBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner):
        bindings = {
            "_reporting": types.SimpleNamespace(desktop_proof_summaries=runner),
        }
        for name in [
            "desktop_run_manifests",
            "desktop_run_summary",
        ]:
            bindings[name] = object()
        return bindings

    def test_list_exports_match_wrappers(self):
        expected = ("desktop_proof_summaries",)

        self.assertEqual(self.mod.DESKTOP_PROOF_LIST_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_proof_summaries_bind_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return [{"latest_run": {}}]

        bindings = self._bindings(runner)
        config = {"desktop_automation": {}}
        result = self.mod.desktop_proof_summaries(
            bindings,
            config,
            target_name="mac",
            action="inspect",
            source_mode="exact-sha",
            sha="abc",
            branch="feature",
            limit=3,
        )
        self.assertEqual(result, [{"latest_run": {}}])
        self.assertEqual(captured["args"], (config,))
        for key, value in {
            "target_name": "mac",
            "action": "inspect",
            "source_mode": "exact-sha",
            "sha": "abc",
            "branch": "feature",
            "limit": 3,
        }.items():
            self.assertEqual(captured["kwargs"][key], value)
        self.assertIs(captured["kwargs"]["desktop_run_manifests_fn"], bindings["desktop_run_manifests"])
        self.assertIs(captured["kwargs"]["desktop_run_summary_fn"], bindings["desktop_run_summary"])

    def test_install_desktop_proof_list_helpers_wires_named_exports(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return [{"installed": True}]

        bindings = self._bindings(runner)
        self.mod.install_desktop_proof_list_helpers(bindings)

        self.assertEqual(bindings["desktop_proof_summaries"]({"desktop_automation": {}}, target_name="mac"), [{"installed": True}])
        self.assertEqual(captured["args"], ({"desktop_automation": {}},))
        self.assertEqual(captured["kwargs"]["target_name"], "mac")

    def test_install_desktop_proof_list_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_desktop_proof_list_helper = lambda _bindings: "future"

        self.mod.install_desktop_proof_list_helpers(bindings, ("future_desktop_proof_list_helper",))

        self.assertEqual(bindings["future_desktop_proof_list_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
