#!/usr/bin/env python3
"""Tests for provenance facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("provenance_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ProvenanceBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_provenance_helpers_delegate_to_provenance_module(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        bindings = {
            "_provenance": types.SimpleNamespace(
                normalize_provenance=make_runner("normalize_provenance", {"execution_kind": "direct"}),
                provenance_summary=make_runner("provenance_summary", "direct via local-ci"),
                normalize_result=make_runner("normalize_result", {"overall": "pass"}),
            )
        }

        provenance = {"runner_provider": "github-hosted"}
        result = {"overall": "pass"}
        self.assertEqual(self.mod.normalize_provenance(bindings, provenance), {"execution_kind": "direct"})
        self.assertEqual(self.mod.provenance_summary(bindings, provenance), "direct via local-ci")
        self.assertEqual(self.mod.normalize_result(bindings, result), {"overall": "pass"})

        self.assertEqual([call[0] for call in calls], [
            "normalize_provenance",
            "provenance_summary",
            "normalize_result",
        ])
        self.assertEqual(calls[0][1], (provenance,))
        self.assertEqual(calls[1][1], (provenance,))
        self.assertEqual(calls[2][1], (result,))

    def test_install_provenance_helpers_wires_named_exports(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        bindings = {
            "_provenance": types.SimpleNamespace(
                normalize_provenance=make_runner("normalize_provenance", {"execution_kind": "direct"}),
                normalize_result=make_runner("normalize_result", {"overall": "pass"}),
            )
        }
        self.mod.install_provenance_helpers(bindings, ("normalize_provenance", "normalize_result"))

        self.assertEqual(bindings["normalize_provenance"](), {"execution_kind": "direct"})
        self.assertEqual(bindings["normalize_result"]({"overall": "pass"}), {"overall": "pass"})
        self.assertEqual(bindings["normalize_provenance"].__name__, "normalize_provenance")
        self.assertEqual([call[0] for call in calls], ["normalize_provenance", "normalize_result"])

    def test_install_provenance_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_provenance_helper = lambda _bindings: "future"

        self.mod.install_provenance_helpers(bindings, ("future_provenance_helper",))

        self.assertEqual(bindings["future_provenance_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
