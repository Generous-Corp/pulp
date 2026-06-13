#!/usr/bin/env python3
"""Tests for queue claim facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


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

    def test_claim_next_job_binds_queue_and_runner_dependencies(self) -> None:
        captured = {}

        def claim_next_job_locked(**kwargs):
            captured["claim"] = kwargs
            return {"id": "claimed"}

        def claim_next_job_unlocked(queue, *, runner, now_iso_fn):
            captured["claim_unlocked"] = (queue, runner, now_iso_fn)
            return {"id": "claimed"}

        lifecycle = types.SimpleNamespace(claim_next_job_locked=claim_next_job_locked)
        orchestrator = types.SimpleNamespace(claim_next_job_unlocked=claim_next_job_unlocked)
        bindings = self._bindings(lifecycle=lifecycle, orchestrator=orchestrator)

        self.assertEqual(self.mod.claim_next_job(bindings), {"id": "claimed"})
        self.assertIs(captured["claim"]["root"], bindings["ROOT"])
        self.assertIs(captured["claim"]["queue_lock_path_fn"], bindings["queue_lock_path"])
        self.assertIs(captured["claim"]["file_lock_fn"], bindings["file_lock"])
        self.assertIs(captured["claim"]["load_queue_unlocked_fn"], bindings["load_queue_unlocked"])
        self.assertIs(captured["claim"]["reconcile_running_jobs_unlocked_fn"], bindings["reconcile_running_jobs_unlocked"])
        self.assertIs(captured["claim"]["save_queue_unlocked_fn"], bindings["save_queue_unlocked"])
        self.assertIs(captured["claim"]["normalize_job_fn"], bindings["normalize_job"])
        self.assertIs(captured["claim"]["pid_fn"], bindings["os"].getpid)
        self.assertEqual(captured["claim"]["claim_next_job_unlocked_fn"]([], runner={"pid": 1}), {"id": "claimed"})
        self.assertEqual(captured["claim_unlocked"], ([], {"pid": 1}, bindings["now_iso"]))

    def test_install_queue_claim_helpers_wires_named_export(self) -> None:
        bindings = self._bindings()

        self.mod.install_queue_claim_helpers(bindings, ("claim_next_job",))

        self.assertIn("claim_next_job", bindings)
        self.assertEqual(bindings["claim_next_job"].__name__, "claim_next_job")

    def test_install_queue_claim_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_queue_claim_helper = lambda _bindings: "future"

        self.mod.install_queue_claim_helpers(bindings, ("future_queue_claim_helper",))

        self.assertEqual(bindings["future_queue_claim_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
