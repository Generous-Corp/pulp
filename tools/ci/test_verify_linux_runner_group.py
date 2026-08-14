"""Focused policy tests for the trusted Linux runner group verifier."""

from __future__ import annotations

import importlib.util
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).with_name("verify_linux_runner_group.py")
SPEC = importlib.util.spec_from_file_location("verify_linux_runner_group", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


REPO = "Generous-Corp/pulp"
EXPECTED = [
    f"{REPO}/.github/workflows/build.yml@refs/heads/main",
    f"{REPO}/.github/workflows/pr-safe-linux.yml@refs/heads/main",
    f"{REPO}/.github/workflows/vellum-freeze-check.yml@refs/heads/main",
    f"{REPO}/.github/workflows/version-skill-check.yml@refs/heads/main",
]


def valid_group() -> dict:
    return {
        "name": "pulp-trusted-build",
        "default": False,
        "visibility": "selected",
        "allows_public_repositories": True,
        "restricted_to_workflows": True,
        "selected_workflows": EXPECTED,
    }


def valid_repositories() -> dict:
    return {"total_count": 1, "repositories": [{"full_name": REPO}]}


class VerifyLinuxRunnerGroupTests(unittest.TestCase):
    def test_exact_trusted_scope_passes(self) -> None:
        self.assertEqual(MODULE.validate_policy(valid_group(), valid_repositories(), REPO), [])

    def test_secret_workflow_is_not_allowed(self) -> None:
        group = valid_group()
        group["selected_workflows"] = EXPECTED + [
            f"{REPO}/.github/workflows/wclap-cloudflare.yml@refs/heads/main"
        ]
        self.assertTrue(MODULE.validate_policy(group, valid_repositories(), REPO))

    def test_pull_request_target_workflow_is_not_allowed(self) -> None:
        group = valid_group()
        group["selected_workflows"] = [
            f"{REPO}/.github/workflows/vellum-trusted-gate.yml@refs/heads/main"
        ]
        self.assertTrue(MODULE.validate_policy(group, valid_repositories(), REPO))

    def test_wrong_repository_is_not_allowed(self) -> None:
        repositories = {"total_count": 1, "repositories": [{"full_name": "other/repo"}]}
        self.assertTrue(MODULE.validate_policy(valid_group(), repositories, REPO))


if __name__ == "__main__":
    unittest.main()
