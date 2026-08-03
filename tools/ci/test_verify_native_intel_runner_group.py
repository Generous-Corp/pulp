#!/usr/bin/env python3
"""Focused policy tests for the native Intel runner group verifier."""

from __future__ import annotations

import importlib.util
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).with_name("verify_native_intel_runner_group.py")
SPEC = importlib.util.spec_from_file_location("verify_native_intel_runner_group", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
REPO = "Generous-Corp/pulp"
WORKFLOW = REPO + "/.github/workflows/nightly-intel.yml@refs/heads/main"


class RunnerGroupPolicyTests(unittest.TestCase):
    def valid_group(self):
        return {
            "default": False,
            "visibility": "selected",
            "allows_public_repositories": True,
            "restricted_to_workflows": True,
            "selected_workflows": [WORKFLOW],
        }

    def valid_repositories(self):
        return {"total_count": 1, "repositories": [{"full_name": REPO}]}

    def test_exact_policy_passes(self):
        self.assertEqual(
            MODULE.validate_policy(
                self.valid_group(), self.valid_repositories(), REPO
            ),
            [],
        )

    def test_unrestricted_or_extra_scope_fails(self):
        group = self.valid_group()
        group["restricted_to_workflows"] = False
        group["selected_workflows"] = [WORKFLOW, REPO + "/.github/workflows/build.yml"]
        repositories = {
            "total_count": 2,
            "repositories": [{"full_name": REPO}, {"full_name": "Generous-Corp/other"}],
        }
        failures = MODULE.validate_policy(group, repositories, REPO)
        self.assertEqual(len(failures), 3)

    def test_missing_fields_fail_closed(self):
        failures = MODULE.validate_policy({}, {}, REPO)
        self.assertEqual(len(failures), 6)


if __name__ == "__main__":
    unittest.main()
