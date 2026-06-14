#!/usr/bin/env python3
"""Tests for desktop proof dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_proof_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopProofBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        bindings = {
            "_reporting": types.SimpleNamespace(**{runner_name: runner}),
        }
        for name in [
            "desktop_run_manifests",
            "desktop_run_summary",
        ]:
            bindings[name] = object()
        return bindings

    def test_report_summary_wrappers_delegate_arguments(self):
        cases = [
            ("normalize_desktop_proof_source_mode", self.mod.normalize_desktop_proof_source_mode, ("exact-sha",), "exact-sha"),
            ("desktop_manifest_adapter", self.mod.desktop_manifest_adapter, ({"desktop_automation": {}}, {"target": "mac"}), "macos-local"),
            ("desktop_manifest_run_status", self.mod.desktop_manifest_run_status, ({"status": "pass"},), "pass"),
            ("desktop_manifest_source", self.mod.desktop_manifest_source, ({"source": {"mode": "current"}},), {"mode": "current"}),
            ("desktop_proof_scope_for_adapter", self.mod.desktop_proof_scope_for_adapter, ("linux-xvfb",), "remote"),
            ("desktop_run_summary", self.mod.desktop_run_summary, ({"desktop_automation": {}}, {"target": "mac"}), {"target": "mac"}),
        ]
        for runner_name, wrapper, args, expected in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*runner_args, **runner_kwargs):
                    captured["args"] = runner_args
                    captured["kwargs"] = runner_kwargs
                    return expected

                bindings = self._bindings(runner_name, runner)
                self.assertEqual(wrapper(bindings, *args), expected)
                self.assertEqual(captured["args"], args)
                self.assertEqual(captured["kwargs"], {})

    def test_proof_summaries_bind_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return [{"latest_run": {}}]

        bindings = self._bindings("desktop_proof_summaries", runner)
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

    def test_proof_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_PROOF_SUMMARY_EXPORTS,
            *self.mod.DESKTOP_PROOF_LIST_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_PROOF_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_proof_helpers_wires_named_exports(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return [{"target": "mac"}]

        bindings = self._bindings("desktop_proof_summaries", runner)
        self.mod.install_desktop_proof_helpers(bindings, ("desktop_proof_summaries",))

        self.assertEqual(bindings["desktop_proof_summaries"]({"desktop_automation": {}}, target_name="mac"), [{"target": "mac"}])
        self.assertEqual(captured["args"], ({"desktop_automation": {}},))
        self.assertEqual(captured["kwargs"]["target_name"], "mac")


if __name__ == "__main__":
    unittest.main()
