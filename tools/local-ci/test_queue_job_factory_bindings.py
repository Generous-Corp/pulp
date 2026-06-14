#!/usr/bin/env python3
"""Tests for queue job construction bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("queue_job_factory_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueJobFactoryBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_job_factory_exports_match_helpers(self) -> None:
        self.assertEqual(self.mod.QUEUE_JOB_FACTORY_EXPORTS, ("make_job",))

    def test_make_job_delegates_with_assembled_dependencies(self) -> None:
        captured = {}

        def make_job(*args, **kwargs):
            captured["make_job"] = (args, kwargs)
            return {"id": "job"}

        orchestrator = types.SimpleNamespace(make_job=make_job)
        bindings = {
            "_queue_orchestrator": orchestrator,
            "ROOT": Path("/repo"),
            "now_iso": object(),
            "uuid": types.SimpleNamespace(uuid4=lambda: types.SimpleNamespace(hex="uuidhex")),
            "validate_ci_branch_name": object(),
        }
        deps = {"now_iso_fn": object(), "root": Path("/repo")}

        with mock.patch.object(self.mod, "queue_job_factory_dependencies", return_value=deps):
            self.assertEqual(self.mod.make_job(bindings, "b", "s", "normal", ["mac"], "run", "full"), {"id": "job"})
        self.assertIs(captured["make_job"][1]["now_iso_fn"], deps["now_iso_fn"])
        self.assertIs(captured["make_job"][1]["root"], deps["root"])


if __name__ == "__main__":
    unittest.main()
