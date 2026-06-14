#!/usr/bin/env python3
"""Tests for desktop proof dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_proof_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopProofBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_proof_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_PROOF_SUMMARY_EXPORTS,
            *self.mod.DESKTOP_PROOF_LIST_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_PROOF_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

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

    def test_install_desktop_proof_helpers_routes_focused_groups_and_fallback(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_desktop_proof_summary_helpers") as install_summary,
            mock.patch.object(self.mod, "install_desktop_proof_list_helpers") as install_list,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_proof_helpers(
                bindings,
                ("desktop_manifest_adapter", "desktop_proof_summaries", "unknown_helper"),
            )

        install_summary.assert_called_once_with(bindings, ("desktop_manifest_adapter",))
        install_list.assert_called_once_with(bindings, ("desktop_proof_summaries",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))

    def _bindings(self, runner_name: str, runner):
        return {
            "_reporting": types.SimpleNamespace(**{runner_name: runner}),
            "desktop_run_manifests": object(),
            "desktop_run_summary": object(),
        }


if __name__ == "__main__":
    unittest.main()
