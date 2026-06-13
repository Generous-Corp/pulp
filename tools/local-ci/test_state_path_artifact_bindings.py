#!/usr/bin/env python3
"""Tests for artifact state path facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("state_path_artifact_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class StatePathArtifactBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        paths = types.SimpleNamespace(
            bundles_dir=make_runner("bundles_dir", Path("/state/bundles")),
            prepared_dir=make_runner("prepared_dir", Path("/state/prepared")),
            desktop_state_dir=make_runner("desktop_state_dir", Path("/state/desktop-automation")),
            desktop_receipts_dir=make_runner("desktop_receipts_dir", Path("/state/desktop-automation/receipts")),
        )
        return {"_state_paths": paths}, calls

    def test_artifact_path_helpers_delegate_to_state_paths_module(self) -> None:
        bindings, calls = self._bindings()
        helpers = [
            "bundles_dir",
            "prepared_dir",
            "desktop_state_dir",
            "desktop_receipts_dir",
        ]

        for name in helpers:
            with self.subTest(name=name):
                self.assertIsInstance(getattr(self.mod, name)(bindings), Path)

        self.assertEqual([call[0] for call in calls], helpers)
        self.assertTrue(all(call[1] == () and call[2] == {} for call in calls))

    def test_install_state_path_artifact_helpers_wires_named_exports(self) -> None:
        bindings, calls = self._bindings()

        self.mod.install_state_path_artifact_helpers(bindings, ("bundles_dir", "prepared_dir"))

        self.assertEqual(bindings["bundles_dir"](), Path("/state/bundles"))
        self.assertEqual(bindings["prepared_dir"](), Path("/state/prepared"))
        self.assertEqual(bindings["bundles_dir"].__name__, "bundles_dir")
        self.assertEqual([call[0] for call in calls], ["bundles_dir", "prepared_dir"])

    def test_install_state_path_artifact_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_state_path_artifact_helper = lambda _bindings: "future"

        self.mod.install_state_path_artifact_helpers(bindings, ("future_state_path_artifact_helper",))

        self.assertEqual(bindings["future_state_path_artifact_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
