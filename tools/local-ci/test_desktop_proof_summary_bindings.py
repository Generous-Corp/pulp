#!/usr/bin/env python3
"""Tests for desktop proof summary dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_proof_summary_bindings.py")


class DesktopProofSummaryBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        return {
            "_reporting": types.SimpleNamespace(**{runner_name: runner}),
        }

    def test_summary_exports_match_wrappers(self):
        expected = (
            "normalize_desktop_proof_source_mode",
            "desktop_manifest_adapter",
            "desktop_manifest_run_status",
            "desktop_manifest_source",
            "desktop_proof_scope_for_adapter",
            "desktop_run_summary",
        )

        self.assertEqual(self.mod.DESKTOP_PROOF_SUMMARY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

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

if __name__ == "__main__":
    unittest.main()
