#!/usr/bin/env python3
"""Tests for SSH bundle build/probe dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
from pathlib import Path
import types
import unittest



def load_module():
    return load_local_ci_module("ssh_bundle_build_bindings.py")


class SshBundleBuildBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_match_build_helpers(self) -> None:
        self.assertEqual(
            self.mod.SSH_BUNDLE_BUILD_EXPORTS,
            (
                "create_job_bundle",
                "config_for_bundle_probe",
            ),
        )
        self.assertEqual(len(self.mod.SSH_BUNDLE_BUILD_EXPORTS), len(set(self.mod.SSH_BUNDLE_BUILD_EXPORTS)))

    def test_create_job_bundle_binds_build_dependencies(self) -> None:
        captured = {}

        def create_job_bundle(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return Path("/state/job1.bundle")

        run_fn = object()
        bindings = {
            "_ssh_bundle": types.SimpleNamespace(create_job_bundle=create_job_bundle),
            "ROOT": Path("/repo"),
            "_BUNDLE_BUILD_LOCK": object(),
            "ensure_state_dirs": object(),
            "bundles_dir": object(),
            "subprocess": types.SimpleNamespace(run=run_fn),
        }

        self.assertEqual(self.mod.create_job_bundle(bindings, {"id": "job1"}), Path("/state/job1.bundle"))
        self.assertEqual(captured["args"], ({"id": "job1"},))
        self.assertIs(captured["kwargs"]["ensure_state_dirs_fn"], bindings["ensure_state_dirs"])
        self.assertIs(captured["kwargs"]["bundles_dir_fn"], bindings["bundles_dir"])
        self.assertIs(captured["kwargs"]["bundle_build_lock"], bindings["_BUNDLE_BUILD_LOCK"])
        self.assertEqual(captured["kwargs"]["root"], Path("/repo"))
        self.assertIs(captured["kwargs"]["run_fn"], run_fn)

    def test_config_for_bundle_probe_binds_config_dependencies(self) -> None:
        captured = {}

        def config_for_bundle_probe(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"targets": {}}

        explicit_config = {"targets": {"ubuntu": {}}}
        bindings = {
            "_ssh_bundle": types.SimpleNamespace(config_for_bundle_probe=config_for_bundle_probe),
            "load_config_file": object(),
            "load_optional_config": object(),
        }

        self.assertEqual(
            self.mod.config_for_bundle_probe(bindings, {"id": "job1"}, explicit_config),
            {"targets": {}},
        )
        self.assertEqual(captured["args"], ({"id": "job1"}, explicit_config))
        self.assertIs(captured["kwargs"]["load_config_file_fn"], bindings["load_config_file"])
        self.assertIs(captured["kwargs"]["load_optional_config_fn"], bindings["load_optional_config"])


if __name__ == "__main__":
    unittest.main()
