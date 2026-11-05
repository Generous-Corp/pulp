#!/usr/bin/env python3
"""Security-contract tests for the Vellum authority workflows."""

from __future__ import annotations

from copy import deepcopy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "tools/scripts"
sys.path.insert(0, str(SCRIPTS))

import test_vellum_receipt_drift as receipt_fixture  # noqa: E402  (needs the path insert)
PINNED_TOKEN_ACTION = (
    "actions/create-github-app-token@"
    "64ab729a6294000f5aa4a4bc0b73d1f32bc45510"
)
PINNED_CHECKOUT_ACTION = (
    "actions/checkout@"
    "3d3c42e5aac5ba805825da76410c181273ba90b1"
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

    def test_pull_request_freeze_compares_from_live_main_candidate(self) -> None:
        value = workflow("vellum-freeze-check.yml")
        job = value["jobs"]["freeze-check"]
        comparison_step = step_named(job, "Resolve exact comparison")
        comparison = comparison_step["run"]
        self.assertIn("'+refs/heads/main:refs/vellum/live-main'", comparison)
        self.assertIn(
            "live_base=\"$(git rev-parse --verify 'refs/vellum/live-main^{commit}')\"",
            comparison,
        )
        self.assertIn("tools/scripts/vellum_trusted_merge.py", comparison)
        self.assertIn('--base "$live_base"', comparison)
        self.assertIn('--head "$source_head"', comparison)
        self.assertIn('[[ "$parents" != "$candidate $live_base $source_head" ]]', comparison)
        self.assertIn('git checkout --quiet --detach "$candidate"', comparison)
        self.assertIn('echo "base=$live_base"', comparison)
        self.assertIn('echo "head=$candidate"', comparison)
        # A stale GitHub merge ref is no longer a failure, and neither the
        # event's merge ref nor the payload PR base may become the comparison.
        self.assertNotIn("rerun this check", comparison)
        self.assertNotIn("$GITHUB_SHA", comparison)
        self.assertNotIn("FETCH_HEAD", comparison)
        self.assertNotIn("PR_BASE", comparison_step.get("env", {}))

    def test_merge_result_is_bound_to_resolved_base_and_source_head(self) -> None:
        value = workflow("vellum-trusted-gate.yml")
        validation = step_named(
            value["jobs"]["trusted-gate"],
            "Validate proposed data and publish head status",
        )["run"]
        self.assertIn(
            '"$trusted_root/tools/scripts/vellum_trusted_merge.py"', validation
        )
        self.assertIn('--base "$PR_BASE"', validation)
        self.assertIn('--head "$PR_HEAD"', validation)
        self.assertIn('fetched_head=$(git rev-parse refs/vellum/pr-head)', validation)
        self.assertIn('[ "$fetched_head" != "$PR_HEAD" ]', validation)
        self.assertNotIn('refs/pull/$PR_NUMBER/merge', validation)
        coherence = validation.index('[ "$fetched_head" != "$PR_HEAD" ]')
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

    def test_recovery_binds_current_main_and_exact_pr_head(self) -> None:
        value = workflow("vellum-freeze-recovery.yml")
        job = value["jobs"]["rerun-pr-context"]
        checkout = step_named(job, "Check out trusted protected main")
        self.assertEqual(checkout["uses"], PINNED_CHECKOUT_ACTION)
        self.assertEqual(
            checkout["with"],
            {"ref": "main", "fetch-depth": 0, "persist-credentials": False},
        )
        resolve = step_named(job, "Resolve live pull request")["run"]
        self.assertIn('jq -r .base.sha', resolve)
        self.assertIn('echo "historical_base=$historical_base"', resolve)
        self.assertIn('echo "source_head=$source_head"', resolve)
        self.assertIn('base_ref" != "main"', resolve)

        bind = step_named(job, "Bind checked-out protected main")["run"]
        self.assertIn('base="$(git rev-parse --verify HEAD^{commit})"', bind)
        validation = step_named(
            job, "Validate current protected-main candidate and publish head status"
        )
        script = validation["run"]
        self.assertIn('refs/pull/$PR_NUMBER/head:refs/vellum/pr-head', script)
        self.assertIn('fetched_head="$(git rev-parse refs/vellum/pr-head)"', script)
        self.assertIn('"$trusted_root/tools/scripts/vellum_trusted_merge.py"', script)
        self.assertIn('"$trusted_root/tools/scripts/vellum_freeze_check.py"', script)
        self.assertIn('"$trusted_root/tools/scripts/vellum_expansion_watch_check.py"', script)
        self.assertNotIn('actions/runs/$run_id/rerun', script)
        self.assertNotIn('refs/pull/$PR_NUMBER/merge', script)
        merge = script.index("vellum_trusted_merge.py")
        pending = script.index("post_status pending")
        validator = script.index("run_validator python3")
        self.assertLess(merge, pending)
        self.assertLess(pending, validator)

    def test_recovery_status_is_exact_context_and_rechecks_live_head(self) -> None:
        value = workflow("vellum-freeze-recovery.yml")
        script = step_named(
            value["jobs"]["rerun-pr-context"],
            "Validate current protected-main candidate and publish head status",
        )["run"]
        self.assertIn("statuses/$PR_HEAD", script)
        self.assertIn("-f context='Vellum freeze'", script)
        self.assertIn('live_state="$(printf', script)
        self.assertIn('live_head="$(printf', script)
        self.assertIn('[ "$live_state" != "open" ] || [ "$live_head" != "$PR_HEAD" ]', script)
        self.assertIn("suppressing stale", script)
        self.assertIn("post_status failure", script)
        self.assertIn("post_status success", script)

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


# Every privileged lane that posts a freeze status, with the step that runs its
# validators. Each must run receipt mode from the trusted base checkout.
RECEIPT_LANES = (
    ("vellum-trusted-gate.yml", "trusted-gate",
     "Validate proposed data and publish head status"),
    ("vellum-trusted-gate.yml", "trusted-merge-group",
     "Revalidate exact merge result with trusted base controls"),
    ("vellum-freeze-recovery.yml", "rerun-pr-context",
     "Validate current protected-main candidate and publish head status"),
)
FROZEN = "core/state/src/s.cpp"
NEUTERED_DRIFT = """class ReceiptDriftError(RuntimeError):
    pass


def check(repo, base, head):
    return {"status": "pass", "state": "active", "owed": {}, "export_id": "neutered"}


def describe_failure(report):
    return ""
"""


def receipt_command(script: str) -> str:
    """The workflow's receipt-mode validator invocation, continuation lines included."""
    lines = script.splitlines()
    starts = [
        i for i, line in enumerate(lines)
        if "vellum_freeze_check.py" in line
        and "--receipt-mode" in "\n".join(lines[i:i + 3])
    ]
    if len(starts) != 1:
        raise AssertionError(f"expected one receipt-mode invocation, found {len(starts)}")
    block = []
    for line in lines[starts[0]:]:
        block.append(line)
        if not line.rstrip().endswith("\\"):
            break
    return "\n".join(block)


class TrustedReceiptModeTests(unittest.TestCase):
    """A PR that edits a frozen file and neuters the receipt check still fails."""

    def setUp(self) -> None:
        self.fixture = receipt_fixture.Fixture(frozen=[FROZEN])
        root = self.fixture.root
        for name in ("vellum_freeze_check.py", "vellum_receipt_drift.py",
                     "vellum_expansion_watch_check.py"):
            self.fixture.write(f"tools/scripts/{name}", (SCRIPTS / name).read_bytes())
        self.base = self.fixture.commit("trusted controls")
        self.fixture.write(FROZEN, "// edited in Pulp\n")
        self.fixture.write("tools/scripts/vellum_receipt_drift.py", NEUTERED_DRIFT)
        self.head = self.fixture.commit("edit frozen path and neuter the check")
        self.trusted = root / ".trusted"
        self.proposed = root / ".proposed"
        self.fixture.git("worktree", "add", "-q", "--detach", str(self.trusted), self.base)
        self.fixture.git("worktree", "add", "-q", "--detach", str(self.proposed), self.head)
        self.temp = Path(tempfile.mkdtemp())

    def tearDown(self) -> None:
        shutil.rmtree(self.temp, ignore_errors=True)
        self.fixture.close()

    def run_lane(self, command: str, trusted_root: Path) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ)
        env.update({
            "trusted_root": str(trusted_root),
            "proposed_tree": str(self.proposed),
            "proposed_head": self.head,
            "PR_BASE": self.base,
            "MERGE_BASE": self.base,
            "MERGE_HEAD": self.head,
            "RUNNER_TEMP": str(self.temp),
        })
        script = 'set -euo pipefail\nrun_validator() { "$@"; }\n' + command
        return subprocess.run(["bash", "-c", script], env=env, capture_output=True,
                              text=True, encoding="utf-8", check=False)

    def test_each_privileged_lane_fails_a_neutered_frozen_edit(self) -> None:
        for name, job, step in RECEIPT_LANES:
            with self.subTest(workflow=name, job=job):
                command = receipt_command(
                    step_named(workflow(name)["jobs"][job], step)["run"]
                )
                self.assertIn('"$trusted_root/tools/scripts/vellum_freeze_check.py"', command)
                self.assertIn('--repo "$proposed_tree"', command)
                result = self.run_lane(command, self.trusted)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(FROZEN, result.stderr)

    def test_control_the_neutered_scripts_would_have_passed(self) -> None:
        # The same invocation pointed at the PR's own scripts passes, so the
        # failure above comes from where the scripts are read, not the fixture.
        name, job, step = RECEIPT_LANES[0]
        command = receipt_command(step_named(workflow(name)["jobs"][job], step)["run"])
        result = self.run_lane(command, self.proposed)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
