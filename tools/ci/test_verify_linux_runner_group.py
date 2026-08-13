#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import contextlib
import io
import pathlib
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).with_name("verify_linux_runner_group.py")
SPEC = importlib.util.spec_from_file_location("verify_linux_runner_group", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
REPO = "Generous-Corp/pulp"
WORKFLOW = REPO + "/.github/workflows/build.yml@refs/heads/main"


class LinuxRunnerGroupPolicyTests(unittest.TestCase):
    def valid_group(self):
        return {
            "name": "pulp-linux-disposable",
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
            MODULE.validate_policy(self.valid_group(), self.valid_repositories(), REPO),
            [],
        )

    def test_default_or_unrestricted_group_fails(self):
        group = self.valid_group()
        group["default"] = True
        group["restricted_to_workflows"] = False
        self.assertEqual(
            len(MODULE.validate_policy(group, self.valid_repositories(), REPO)), 2
        )

    def test_extra_repo_or_workflow_fails(self):
        group = self.valid_group()
        group["selected_workflows"].append(REPO + "/.github/workflows/iwyu.yml@refs/heads/main")
        repos = {
            "total_count": 2,
            "repositories": [{"full_name": REPO}, {"full_name": "Generous-Corp/other"}],
        }
        self.assertEqual(len(MODULE.validate_policy(group, repos, REPO)), 2)

    def test_missing_fields_fail_closed(self):
        self.assertEqual(len(MODULE.validate_policy({}, {}, REPO)), 7)

    def test_api_failure_keeps_the_group_offline(self):
        stderr = io.StringIO()
        with mock.patch.object(MODULE, "api_json", side_effect=RuntimeError("denied")):
            with contextlib.redirect_stderr(stderr):
                result = MODULE.main(
                    ["--gh", "gh", "--repo", REPO, "--group-id", "42"]
                )
        self.assertEqual(result, 1)
        self.assertIn("cannot verify runner group policy: denied", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
