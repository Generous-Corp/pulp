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
