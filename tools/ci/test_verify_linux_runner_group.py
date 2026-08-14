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
    f"{REPO}/.github/workflows/pr-safe-linux.yml@refs/heads/main",
]
PR_SAFE_EXPECTED = [
    f"{REPO}/.github/workflows/pr-safe-linux.yml@refs/heads/main",
]
RELEASE_REF = "refs/tags/v0.806.1"
RELEASE_EXPECTED = [
    f"{REPO}/.github/workflows/release-cli.yml@{RELEASE_REF}",
    f"{REPO}/.github/workflows/sign-and-release.yml@{RELEASE_REF}",
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

    def test_exact_release_control_scope_passes(self) -> None:
        group = valid_group()
        group["name"] = "pulp-release-control"
        group["selected_workflows"] = RELEASE_EXPECTED
        self.assertEqual(
            MODULE.validate_policy(
                group, valid_repositories(), REPO, "release-control", RELEASE_REF
            ),
            [],
        )

    def test_release_control_requires_an_exact_semver_tag(self) -> None:
        group = valid_group()
        group["name"] = "pulp-release-control"
        group["selected_workflows"] = RELEASE_EXPECTED
        for bad_ref in (
            None,
            "refs/heads/main",
            "refs/tags/v*",
            "v0.806.1",
            "refs/tags/v01.2.3",
            "refs/tags/v1.02.3",
            "refs/tags/v1.2.03",
            "refs/tags/v1.2.3-alpha..1",
            "refs/tags/v1.2.3-alpha.01",
        ):
            self.assertTrue(
                MODULE.validate_policy(
                    group, valid_repositories(), REPO, "release-control", bad_ref
                )
            )

    def test_release_control_accepts_complete_semver_tag_forms(self) -> None:
        for workflow_ref in (
            "refs/tags/v0.806.1",
            "refs/tags/v1.2.3-alpha.1",
            "refs/tags/v1.2.3-alpha-1+build.5",
        ):
            self.assertTrue(MODULE.valid_release_tag_ref(workflow_ref))

    def test_release_control_rejects_extra_or_wrong_tag_workflows(self) -> None:
        group = valid_group()
        group["name"] = "pulp-release-control"
        group["selected_workflows"] = RELEASE_EXPECTED + [
            f"{REPO}/.github/workflows/build.yml@{RELEASE_REF}"
        ]
        self.assertTrue(
            MODULE.validate_policy(
                group, valid_repositories(), REPO, "release-control", RELEASE_REF
            )
        )
        group["selected_workflows"] = [
            item.replace("v0.806.1", "v0.806.0") for item in RELEASE_EXPECTED
        ]
        self.assertTrue(
            MODULE.validate_policy(
                group, valid_repositories(), REPO, "release-control", RELEASE_REF
            )
        )


if __name__ == "__main__":
    unittest.main()
