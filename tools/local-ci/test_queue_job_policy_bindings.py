#!/usr/bin/env python3
"""Tests for queue job policy facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_job_policy_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueJobPolicyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_job_policy_exports_match_facade_helpers(self) -> None:
        expected = (
            "default_priority_for",
            "make_fingerprint",
            "make_job",
            "validate_ci_branch_name",
        )

        self.assertEqual(self.mod.QUEUE_JOB_POLICY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_queue_job_policy_bindings_delegate_to_orchestrator(self) -> None:
        captured = {}

        def make_job(*args, **kwargs):
            captured["make_job"] = (args, kwargs)
            return {"id": "job"}

        orchestrator = types.SimpleNamespace(
            default_priority_for=lambda command, config: f"{command}:{config['priority']}",
            make_fingerprint=lambda branch, sha, targets, validation: "|".join([branch, sha, ",".join(targets), validation]),
            make_job=make_job,
            validate_ci_branch_name=lambda branch: branch.strip(),
        )
        bindings = {
            "_queue_orchestrator": orchestrator,
            "ROOT": Path("/repo"),
            "now_iso": object(),
            "uuid": types.SimpleNamespace(uuid4=lambda: types.SimpleNamespace(hex="uuidhex")),
            "validate_ci_branch_name": object(),
        }

        self.assertEqual(self.mod.default_priority_for(bindings, "ship", {"priority": "high"}), "ship:high")
        self.assertEqual(self.mod.make_fingerprint(bindings, "b", "s", ["mac"], "full"), "b|s|mac|full")
        self.assertEqual(self.mod.make_job(bindings, "b", "s", "normal", ["mac"], "run", "full"), {"id": "job"})
        self.assertIs(captured["make_job"][1]["now_iso_fn"], bindings["now_iso"])
        self.assertEqual(captured["make_job"][1]["uuid_hex_fn"](), "uuidhex")
        self.assertIs(captured["make_job"][1]["root"], bindings["ROOT"])
        self.assertIs(captured["make_job"][1]["validate_branch_fn"], bindings["validate_ci_branch_name"])
        self.assertEqual(self.mod.validate_ci_branch_name(bindings, " branch "), "branch")

    def test_install_queue_job_policy_helpers_wires_named_exports(self) -> None:
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_job_policy_helpers(bindings, ("make_job", "custom_job_policy"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("make_job",)),
                mock.call(bindings, self.mod.__dict__, ("custom_job_policy",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
