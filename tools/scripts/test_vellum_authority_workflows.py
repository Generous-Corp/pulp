#!/usr/bin/env python3
"""Security-contract tests for the Vellum authority workflows."""

from __future__ import annotations

from copy import deepcopy
import json
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
RUNNER_EXPRESSION = (
    "${{ fromJSON(vars.PULP_VELLUM_TRUSTED_RUNS_ON_JSON "
    "|| '\"ubuntu-latest\"') }}"
)
MACPRO_SELECTOR = [
    "self-hosted",
    "Linux",
    "X64",
    "pulp-build-linux-x64",
    "pulp-host-macpro",
]


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
    def assert_privileged_pr_gate_uses_literal_trusted_checkout(
        self, value: dict[str, object]
    ) -> None:
        job = value["jobs"]["trusted-gate"]
        checkout = step_named(job, "Check out trusted base controls")
        self.assertEqual(checkout["uses"], PINNED_CHECKOUT_ACTION)
        self.assertEqual(
            checkout["with"],
            {
                "ref": "main",
                "fetch-depth": 0,
                "persist-credentials": False,
            },
        )

        steps = job["steps"]
        checkout_index = steps.index(checkout)
        bind_index = next(
            i
            for i, step in enumerate(steps)
            if step.get("name") == "Bind checked-out protected base"
        )
        token_index = next(
            i
            for i, step in enumerate(steps)
            if step.get("name") == "Mint one-repository Vellum reader token"
        )
        self.assertLess(checkout_index, bind_index)
        self.assertLess(bind_index, token_index)
        bind = steps[bind_index]
        self.assertEqual(bind["id"], "trusted-base")
        self.assertIn('base="$(git rev-parse HEAD)"', bind["run"])

        validation = step_named(
            job, "Validate proposed data and publish head status"
        )
        self.assertEqual(
            validation["env"]["PR_BASE"],
            "${{ steps.trusted-base.outputs.base_sha }}",
        )

        resolve = step_named(job, "Resolve pull request")["run"]
        self.assertNotIn("git/ref/heads/main", resolve)
        self.assertNotIn("jq -r .base.sha", resolve)
        self.assertNotIn("base_sha=", resolve)

    def test_privileged_pr_gate_checks_out_only_literal_protected_main(self) -> None:
        value = workflow("vellum-trusted-gate.yml")
        trigger = value.get(True) or value["on"]
        self.assertEqual(trigger["pull_request_target"]["branches"], ["main"])
        self.assert_privileged_pr_gate_uses_literal_trusted_checkout(value)

        # Mutation control: prove the structural guard rejects the exact
        # API-derived ref shape reported by CodeQL alert 42.
        unsafe = deepcopy(value)
        checkout = step_named(
            unsafe["jobs"]["trusted-gate"], "Check out trusted base controls"
        )
        checkout["with"]["ref"] = "${{ steps.pr.outputs.base_sha }}"
        with self.assertRaises(AssertionError):
            self.assert_privileged_pr_gate_uses_literal_trusted_checkout(unsafe)

    def test_trusted_jobs_use_default_preserving_runner_selector(self) -> None:
        value = workflow("vellum-trusted-gate.yml")
        jobs = value["jobs"]
        for job_name in ("trusted-gate", "trusted-merge-group"):
            with self.subTest(job=job_name):
                self.assertEqual(
                    " ".join(jobs[job_name]["runs-on"].split()),
                    " ".join(RUNNER_EXPRESSION.split()),
                )

        self.assertEqual(json.loads('"ubuntu-latest"'), "ubuntu-latest")
        selector = json.dumps(MACPRO_SELECTOR, separators=(",", ":"))
        self.assertEqual(json.loads(selector), MACPRO_SELECTOR)

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
