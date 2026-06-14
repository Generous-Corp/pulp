#!/usr/bin/env python3
"""Tests for target submission metadata print bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("target_submission_print_bindings.py")


class TargetSubmissionPrintBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_submission_print_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.TARGET_SUBMISSION_PRINT_EXPORTS, ("print_submission_metadata",))

    def test_print_submission_metadata_delegates_facade_dependencies(self) -> None:
        captured = {}

        def capture(*args, **kwargs):
            captured["print"] = (args, kwargs)

        preflight = types.SimpleNamespace(print_submission_metadata=capture)
        bindings = {
            "_target_preflight": preflight,
            "short_sha": object(),
            "provenance_summary": object(),
            "print": object(),
        }

        self.mod.print_submission_metadata(bindings, {"branch": "feature/topic"})
        self.assertIs(captured["print"][1]["short_sha_fn"], bindings["short_sha"])
        self.assertIs(captured["print"][1]["provenance_summary_fn"], bindings["provenance_summary"])
        self.assertIs(captured["print"][1]["print_fn"], bindings["print"])

    def test_install_target_submission_print_helpers_wires_named_exports(self) -> None:
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_target_submission_print_helpers(
                bindings,
                ("print_submission_metadata", "custom_submission_print"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("print_submission_metadata",)),
                mock.call(bindings, self.mod.__dict__, ("custom_submission_print",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
