#!/usr/bin/env python3
"""Tests for target submission metadata facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("target_submission_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetSubmissionBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, preflight):
        bindings = {
            "_target_preflight": preflight,
            "ROOT": Path("/repo"),
            "os": types.SimpleNamespace(environ={"PULP_LOCAL_CI_CONFIG": "/config"}),
            "print": object(),
        }
        for name in [
            "git_root_for",
            "config_path",
            "config_source_name",
            "preflight_target_host_state",
            "find_material_config_drift",
            "normalize_provenance",
            "short_sha",
            "provenance_summary",
        ]:
            bindings[name] = object()
        return bindings

    def test_submission_and_print_bindings_delegate_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result=None):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        preflight = types.SimpleNamespace(
            build_submission_metadata=lambda *args, **kwargs: captured.setdefault("metadata", (args, kwargs)) and {"branch": args[1]},
            print_submission_metadata=capture("print"),
        )
        bindings = self._bindings(preflight)

        self.assertEqual(
            self.mod.build_submission_metadata(
                bindings,
                {"targets": {}},
                "feature/topic",
                "a" * 40,
                ["mac"],
                "normal",
                "full",
                allow_root_mismatch=False,
                allow_unreachable_targets=True,
            ),
            {"branch": "feature/topic"},
        )
        metadata_kwargs = captured["metadata"][1]
        self.assertIs(metadata_kwargs["root"], bindings["ROOT"])
        self.assertIs(metadata_kwargs["git_root_for_fn"], bindings["git_root_for"])
        self.assertIs(metadata_kwargs["config_path_fn"], bindings["config_path"])
        self.assertIs(metadata_kwargs["config_source_name_fn"], bindings["config_source_name"])
        self.assertIs(metadata_kwargs["preflight_target_host_state_fn"], bindings["preflight_target_host_state"])
        self.assertIs(metadata_kwargs["find_material_config_drift_fn"], bindings["find_material_config_drift"])
        self.assertIs(metadata_kwargs["normalize_provenance_fn"], bindings["normalize_provenance"])
        self.assertIs(metadata_kwargs["environ"], bindings["os"].environ)

        self.mod.print_submission_metadata(bindings, {"branch": "feature/topic"})
        self.assertIs(captured["print"][1]["short_sha_fn"], bindings["short_sha"])
        self.assertIs(captured["print"][1]["provenance_summary_fn"], bindings["provenance_summary"])
        self.assertIs(captured["print"][1]["print_fn"], bindings["print"])


if __name__ == "__main__":
    unittest.main()
