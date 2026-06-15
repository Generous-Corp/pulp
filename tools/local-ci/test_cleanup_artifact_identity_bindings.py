#!/usr/bin/env python3
"""Tests for cleanup artifact identity facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
from pathlib import Path
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("cleanup_artifact_identity_bindings.py")


class CleanupArtifactIdentityBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.cleanup = mock.Mock()
        self.bindings = {"_cleanup": self.cleanup}

    def test_artifact_identity_exports_match_facade_helpers(self) -> None:
        expected = (
            "result_file_job_id",
            "artifact_entry_sort_key",
        )

        self.assertEqual(self.mod.CLEANUP_ARTIFACT_IDENTITY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_artifact_identity_helpers_delegate_to_cleanup_module(self) -> None:
        path = Path("/state/results/job1.json")
        entry = {"mtime": 1.0}
        self.cleanup.result_file_job_id.return_value = "job1"
        self.cleanup.artifact_entry_sort_key.return_value = (1.0, "job1")

        self.assertEqual(self.mod.result_file_job_id(self.bindings, path), "job1")
        self.assertEqual(self.mod.artifact_entry_sort_key(self.bindings, entry), (1.0, "job1"))
        self.cleanup.result_file_job_id.assert_called_once_with(path)
        self.cleanup.artifact_entry_sort_key.assert_called_once_with(entry)

    def test_install_cleanup_artifact_identity_helpers_wires_named_exports(self) -> None:
        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_cleanup_artifact_identity_helpers(
                self.bindings,
                ("result_file_job_id", "custom_cleanup_identity"),
            )

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(self.bindings, self.mod.__dict__, ("result_file_job_id",)),
                mock.call(self.bindings, self.mod.__dict__, ("custom_cleanup_identity",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
