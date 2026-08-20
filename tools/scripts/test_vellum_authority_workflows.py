#!/usr/bin/env python3
"""Security-contract tests for the Vellum authority workflows."""

from __future__ import annotations

from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[2]
PINNED_TOKEN_ACTION = (
    "actions/create-github-app-token@"
    "fee1f7d63c2ff003460e3d139729b119787bc349"
)
PINNED_CHECKOUT_ACTION = (
    "actions/checkout@"
    "11d5960a326750d5838078e36cf38b85af677262"
)


def workflow(name: str) -> dict[str, object]:
    value = yaml.safe_load(
        (ROOT / ".github/workflows" / name).read_text(encoding="utf-8")
    )
    if not isinstance(value, dict):
        raise AssertionError(f"{name} is not a workflow mapping")
    return value


def step_named(job: dict[str, object], name: str) -> dict[str, object]:
    steps = job.get("steps")
    if not isinstance(steps, list):
        raise AssertionError("workflow job lacks steps")
    matches = [
        step
        for step in steps
        if isinstance(step, dict) and step.get("name") == name
    ]
    if len(matches) != 1:
        raise AssertionError(f"expected one workflow step named {name}")
    return matches[0]


class VellumAuthorityWorkflowTests(unittest.TestCase):
    def test_privileged_pr_gate_checks_out_only_resolved_protected_main(self) -> None:
        value = workflow("vellum-trusted-gate.yml")
        trigger = value.get(True) or value["on"]
        self.assertEqual(trigger["pull_request_target"]["branches"], ["main"])

        job = value["jobs"]["trusted-gate"]
        checkout = step_named(job, "Check out trusted base controls")
        self.assertEqual(checkout["uses"], PINNED_CHECKOUT_ACTION)
        self.assertEqual(
            checkout["with"],
            {
                "ref": "${{ steps.pr.outputs.base_sha }}",
                "fetch-depth": 0,
                "persist-credentials": False,
            },
        )

        steps = job["steps"]
        verify_index = next(
            i
            for i, step in enumerate(steps)
            if step.get("name") == "Verify trusted controls match live PR base"
        )
        token_index = next(
            i
            for i, step in enumerate(steps)
            if step.get("name") == "Mint one-repository Vellum reader token"
        )
        self.assertLess(verify_index, token_index)
        verify = steps[verify_index]
        self.assertEqual(
            verify["env"]["EXPECTED_BASE"],
            "${{ steps.pr.outputs.base_sha }}",
        )
        self.assertIn('actual_base="$(git rev-parse HEAD)"', verify["run"])
        self.assertIn('[ "$actual_base" != "$EXPECTED_BASE" ]', verify["run"])
        resolve = step_named(job, "Resolve pull request")["run"]
        self.assertIn('git/ref/heads/main" --jq .object.sha', resolve)
        self.assertNotIn("jq -r .base.sha", resolve)
        target_check = resolve.index('[ "$base_repository" != "$GITHUB_REPOSITORY" ]')
        protected_ref = resolve.index("git/ref/heads/main")
        self.assertLess(target_check, protected_ref)

    def test_proposed_worktree_is_data_only(self) -> None:
        value = workflow("vellum-trusted-gate.yml")
        validation = step_named(
            value["jobs"]["trusted-gate"],
            "Validate proposed data and publish head status",
        )["run"]
        self.assertNotIn('working-directory: "$proposed_tree"', validation)
        self.assertNotRegex(
            validation,
            r'(?m)^\s*(?:bash|sh|python3)\s+"?\$proposed_tree(?:/|\")',
        )

    def test_merge_result_is_bound_to_resolved_base_and_source_head(self) -> None:
        value = workflow("vellum-trusted-gate.yml")
        validation = step_named(
            value["jobs"]["trusted-gate"],
            "Validate proposed data and publish head status",
        )["run"]
        self.assertIn('merge_base=$(git rev-parse "$proposed_head^1")', validation)
        self.assertIn('merge_source=$(git rev-parse "$proposed_head^2")', validation)
        coherence = validation.index('[ "$merge_base" != "$PR_BASE" ]')
        pending = validation.index("post_status pending")
        validators = validation.index("run_validator python3")
        self.assertLess(coherence, pending)
        self.assertLess(coherence, validators)
        for trusted_script in (
            "vellum_freeze_check.py",
            "vellum_expansion_watch_check.py",
            "pulp_tooling_disposition.py",
            "generate_vellum_cut_manifest.py",
            "generate_vellum_ownership_projection.py",
        ):
            self.assertIn(f'"$trusted_root/tools/scripts/{trusted_script}"', validation)

    def test_trusted_lanes_mint_exact_reader_identity_and_one_repo_token(self) -> None:
        value = workflow("vellum-trusted-gate.yml")
        jobs = value["jobs"]
        for job_name in ("trusted-gate", "trusted-merge-group"):
            with self.subTest(job=job_name):
                job = jobs[job_name]
                token = step_named(job, "Mint one-repository Vellum reader token")
                self.assertEqual(token["uses"], PINNED_TOKEN_ACTION)
                self.assertEqual(
                    token["with"],
                    {
                        "app-id": "3878000",
                        "private-key": "${{ secrets.VELLUM_READER_APP_PRIVATE_KEY }}",
                        "owner": "Generous-Corp",
                        "repositories": "vellum",
                        "permission-checks": "read",
                        "permission-contents": "read",
                    },
                )
                jwt = step_named(
                    job, "Mint short-lived Vellum reader App identity proof"
                )
                self.assertIn("github_app_jwt.py", jwt["run"])
                validation_name = (
                    "Validate proposed data and publish head status"
                    if job_name == "trusted-gate"
                    else "Revalidate exact merge result with trusted base controls"
                )
                validation = step_named(job, validation_name)
                self.assertEqual(
                    validation["env"]["VELLUM_READER_TOKEN"],
                    "${{ steps.vellum-reader-token.outputs.token }}",
                )
                self.assertEqual(
                    validation["env"]["VELLUM_READER_APP_JWT"],
                    "${{ steps.vellum-reader-jwt.outputs.app_jwt }}",
                )

    def test_dispatcher_has_no_checkout_and_only_one_repo_write_token(self) -> None:
        value = workflow("vellum-observatory-dispatch.yml")
        dispatch = value["jobs"]["dispatch"]
        steps = dispatch["steps"]
        self.assertFalse(any("checkout@" in str(step.get("uses", "")) for step in steps))
        token = step_named(dispatch, "Mint one-repository Vellum dispatcher token")
        self.assertEqual(token["uses"], PINNED_TOKEN_ACTION)
        self.assertEqual(token["with"]["owner"], "Generous-Corp")
        self.assertEqual(token["with"]["repositories"], "vellum")
        self.assertEqual(token["with"]["permission-contents"], "write")
        send = step_named(dispatch, "Send compact verified outbox record")
        self.assertEqual(
            send["env"]["GH_TOKEN"],
            "${{ steps.vellum-dispatcher-token.outputs.token }}",
        )
        serialized = (ROOT / ".github/workflows/vellum-observatory-dispatch.yml").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("VELLUM_OBSERVATORY_TOKEN", serialized)


if __name__ == "__main__":
    unittest.main()
