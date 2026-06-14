#!/usr/bin/env python3
"""Tests for queue claim facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_claim_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueClaimBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, lifecycle=None, orchestrator=None):
        bindings = {
            "_queue_lifecycle": lifecycle or types.SimpleNamespace(),
            "_queue_orchestrator": orchestrator or types.SimpleNamespace(),
            "ROOT": Path("/repo"),
            "os": types.SimpleNamespace(getpid=object()),
        }
        for name in [
            "queue_lock_path",
            "file_lock",
            "load_queue_unlocked",
            "reconcile_running_jobs_unlocked",
            "save_queue_unlocked",
            "normalize_job",
            "now_iso",
        ]:
            bindings[name] = object()
        return bindings

    def test_claim_exports_match_facade_helpers(self) -> None:
        self.assertEqual(self.mod.QUEUE_CLAIM_EXPORTS, ("claim_next_job",))

    def test_claim_next_job_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def claim_next_job_locked(**kwargs):
            captured["claim"] = kwargs
            return {"id": "claimed"}

        lifecycle = types.SimpleNamespace(claim_next_job_locked=claim_next_job_locked)
        bindings = self._bindings(lifecycle=lifecycle)
        deps = {"root": object(), "pid_fn": object()}

        with mock.patch.object(self.mod, "queue_claim_dependencies", return_value=deps):
            self.assertEqual(self.mod.claim_next_job(bindings), {"id": "claimed"})

        self.assertIs(captured["claim"]["root"], deps["root"])
        self.assertIs(captured["claim"]["pid_fn"], deps["pid_fn"])

    def test_install_queue_claim_helpers_wires_named_export(self) -> None:
        bindings = self._bindings()

        self.mod.install_queue_claim_helpers(bindings, ("claim_next_job",))

        self.assertIn("claim_next_job", bindings)
        self.assertEqual(bindings["claim_next_job"].__name__, "claim_next_job")


if __name__ == "__main__":
    unittest.main()
