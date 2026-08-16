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
PR_SAFE_EXPECTED = [
    f"{REPO}/.github/workflows/pr-safe-linux.yml@refs/heads/main",
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

    def test_exact_pr_safe_scope_passes(self) -> None:
        group = valid_group()
        group["name"] = "pulp-pr-safe-build"
        group["selected_workflows"] = PR_SAFE_EXPECTED
        self.assertEqual(
            MODULE.validate_policy(group, valid_repositories(), REPO, "pr-safe"), []
        )

    def test_groups_cannot_exchange_names_or_workflows(self) -> None:
        pr_safe = valid_group()
        pr_safe["name"] = "pulp-pr-safe-build"
        pr_safe["selected_workflows"] = PR_SAFE_EXPECTED
        self.assertTrue(
            MODULE.validate_policy(pr_safe, valid_repositories(), REPO, "trusted")
        )
        self.assertTrue(
            MODULE.validate_policy(valid_group(), valid_repositories(), REPO, "pr-safe")
        )

    def test_generic_repository_requires_exact_name_repository_and_workflow(self) -> None:
        repo = "Generous-Corp/vellum"
        workflow = ".github/workflows/build.yml"
        group = valid_group()
        group["name"] = "vellum-pr-safe-build"
        group["selected_workflows"] = [f"{repo}/{workflow}@refs/heads/main"]
        repositories = {"total_count": 1, "repositories": [{"full_name": repo}]}
        self.assertEqual(
            MODULE.validate_policy(
                group,
                repositories,
                repo,
                group_name="vellum-pr-safe-build",
                workflow=workflow,
            ),
            [],
        )
        group["selected_workflows"].append(
            f"{repo}/.github/workflows/release.yml@refs/heads/main"
        )
        self.assertTrue(
            MODULE.validate_policy(
                group,
                repositories,
                repo,
                group_name="vellum-pr-safe-build",
                workflow=workflow,
            )
        )

    def test_generic_scope_arguments_are_atomic(self) -> None:
        self.assertTrue(
            MODULE.validate_policy(
                valid_group(),
                valid_repositories(),
                REPO,
                group_name="orphan-name",
            )
        )

if __name__ == "__main__":
    unittest.main()
