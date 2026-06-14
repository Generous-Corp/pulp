#!/usr/bin/env python3
"""Tests for target submission metadata construction bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
from pathlib import Path
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("target_submission_build_bindings.py")


class TargetSubmissionBuildBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, preflight):
        bindings = {
            "_target_preflight": preflight,
            "ROOT": Path("/repo"),
            "os": types.SimpleNamespace(environ={"PULP_LOCAL_CI_CONFIG": "/config"}),
        }
        for name in [
            "git_root_for",
            "config_path",
            "config_source_name",
            "preflight_target_host_state",
            "find_material_config_drift",
            "normalize_provenance",
        ]:
            bindings[name] = object()
        return bindings

    def test_submission_build_exports_are_declared(self) -> None:
        self.assertEqual(self.mod.TARGET_SUBMISSION_BUILD_EXPORTS, ("build_submission_metadata",))

    def test_build_submission_metadata_delegates_facade_dependencies(self) -> None:
        captured = {}

        preflight = types.SimpleNamespace(
            build_submission_metadata=lambda *args, **kwargs: captured.setdefault("metadata", (args, kwargs)) and {"branch": args[1]},
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

    def test_install_target_submission_build_helpers_wires_named_exports(self) -> None:
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_target_submission_build_helpers(
                bindings,
                ("build_submission_metadata", "custom_submission_build"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("build_submission_metadata",)),
                mock.call(bindings, self.mod.__dict__, ("custom_submission_build",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
