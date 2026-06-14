#!/usr/bin/env python3
"""Tests for queue job construction bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("queue_job_factory_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueJobFactoryBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_job_factory_exports_match_helpers(self) -> None:
        self.assertEqual(self.mod.QUEUE_JOB_FACTORY_EXPORTS, ("make_job",))

    def test_make_job_binds_time_uuid_root_and_branch_validation(self) -> None:
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

        self.assertEqual(self.mod.make_job(bindings, "b", "s", "normal", ["mac"], "run", "full"), {"id": "job"})
        self.assertIs(captured["make_job"][1]["now_iso_fn"], bindings["now_iso"])
        self.assertEqual(captured["make_job"][1]["uuid_hex_fn"](), "uuidhex")
        self.assertIs(captured["make_job"][1]["root"], bindings["ROOT"])
        self.assertIs(captured["make_job"][1]["validate_branch_fn"], bindings["validate_ci_branch_name"])


if __name__ == "__main__":
    unittest.main()
