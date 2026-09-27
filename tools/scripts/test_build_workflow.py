#!/usr/bin/env python3
"""Structural invariants for exact protected receipt reuse in build.yml."""

from pathlib import Path
import json
import re
import unittest

import yaml


WORKFLOW = (Path(__file__).parents[2] / ".github/workflows/build.yml").read_text(
    encoding="utf-8"
)


def _workflow() -> dict[str, object]:
    return yaml.safe_load(WORKFLOW)


class ProtectedReceiptWorkflowTest(unittest.TestCase):
    def test_verifier_comes_from_exact_protected_base(self) -> None:
        self.assertIn(
            'git show "$base:tools/scripts/protected_merge_receipt.py"', WORKFLOW
        )
        self.assertIn('--group-sha "$group"', WORKFLOW)

    def test_any_unavailable_receipt_retains_full_target(self) -> None:
        reuse_block = WORKFLOW.split("  protected-receipt-reuse:", 1)[1].split(
            "\n  build:", 1
        )[0]
        # Reuse (dropping the target from the matrix) happens only in the
        # branch where the protected-base copy both downloaded and verified.
        self.assertRegex(
            reuse_block,
            re.compile(
                r'if ! python3 "\$RUNNER_TEMP/protected_merge_receipt.py" download .*'
                r'elif python3 "\$RUNNER_TEMP/protected_merge_receipt.py" verify [^\n]*(\n[^\n]*)*?'
                r'\n\s*matrix="\$\(MATRIX_JSON=.*macos_reused=true.*\n\s*else\n',
                re.DOTALL,
            ),
        )
        # The checked-out copy only renders decision notes; it never decides.
        workspace_calls = re.findall(
            r"python3 tools/scripts/protected_merge_receipt.py (\S+)", reuse_block)
        self.assertEqual(set(workspace_calls), {"note", "publish-notes"})
        self.assertIn("--summary", reuse_block)
        self.assertIn(
            "receipt reuse unavailable for ${target}; full validation retained",
            reuse_block,
        )
        self.assertIn('matrix="$ORIGINAL_MATRIX"', reuse_block)
        build = WORKFLOW.split("\n  build:", 1)[1].split(
            "\n  windows-msvc-release-gate:", 1
        )[0]
        self.assertIn("always()", build)
        self.assertIn("protected-receipt-reuse.result != 'success'", build)
        self.assertIn(
            "protected-receipt-reuse.outputs.matrix_json || needs.resolve-provider.outputs.matrix_json",
            build,
        )

    def test_required_macos_alias_needs_subject_bound_reuse(self) -> None:
        alias = WORKFLOW.split("\n  macos-merge-group:", 1)[1].split(
            "\n  linux:", 1
        )[0]
        self.assertIn("protected-receipt-reuse.outputs.macos_reused == 'true'", alias)
        self.assertIn("protected receipt decision unavailable", alias)

    def test_receipts_are_only_published_after_successful_pr_validation(self) -> None:
        issue = WORKFLOW.split("- name: Issue exact protected-validation receipt", 1)[1].split(
            "\n      - name:", 1
        )[0]
        condition = issue.split("run: |", 1)[0]
        self.assertIn("github.event_name == 'pull_request'", condition)
        self.assertIn("success()", condition)
        # success() alone also holds when the Test step was skipped; the
        # receipt must require that the tests actually ran and passed.
        self.assertIn("steps.ctest.outcome == 'success'", condition)
        for flag in ("--ctest-exit-file", "--ctest-selection", "--ctest-selected-json", "--ctest-junit"):
            self.assertIn(flag, issue)

    def test_ctest_step_records_receipt_evidence(self) -> None:
        test_step = WORKFLOW.split("- name: Test (non-Windows)", 1)[1].split(
            "\n      - name:", 1
        )[0]
        self.assertIn("id: ctest", test_step)
        self.assertIn('"$evidence_dir/selection.json"', test_step)
        self.assertIn("ctest --show-only=json-v1 --test-dir", test_step)
        self.assertIn('"$evidence_dir/exit-code"', test_step)
        self.assertIn("--output-junit", test_step)
        self.assertIn("id: protected_receipt", WORKFLOW)
        self.assertIn("steps.protected_receipt.outcome == 'success'", WORKFLOW)
        self.assertIn("steps.protected_receipt.outputs.path", WORKFLOW)
        self.assertNotIn("steps.protected-receipt", WORKFLOW)
        self.assertIn("retention-days: 2", WORKFLOW)

    def test_receipt_uses_the_checkout_that_was_built_not_event_sha(self) -> None:
        issuer = WORKFLOW.split(
            "      - name: Issue exact protected-validation receipt", 1
        )[1].split("      - name: Publish exact protected-validation receipt", 1)[0]
        self.assertIn(
            "checkout_sha=\"$(git rev-parse --verify 'HEAD^{commit}')\"", issuer
        )
        self.assertIn('--checkout-sha "$checkout_sha"', issuer)
        self.assertNotIn('--checkout-sha "$GITHUB_SHA"', issuer)
        self.assertIn('--base-sha "$PR_BASE_SHA" --head-sha "$PR_HEAD_SHA"', issuer)


class PrGateSettleWorkflowTest(unittest.TestCase):
    """The default-off settle window may delay only PR native builds, never gate them."""

    EVENTS = ("pull_request", "merge_group", "push", "workflow_dispatch")

    def setUp(self) -> None:
        self.jobs = _workflow()["jobs"]
        self.settle = self.jobs["pr-gate-settle"]

    def _settle_runs(self, event: str, var: str, local_proof: bool = False) -> bool:
        """Evaluate the settle job's `if` exactly as written, for one event."""
        expr = " ".join(self.settle["if"].split())
        python = (
            expr.replace("!inputs.local_proof", "(not local_proof)")
            .replace("&&", " and ")
            .replace("github.event_name", "event")
            .replace("vars.PULP_PR_GATE_SETTLE_SECONDS", "var")
        )
        self.assertNotRegex(python, r"\|\||!(?!=)|\$\{\{")
        return bool(eval(python, {}, {"event": event, "var": var, "local_proof": local_proof}))

    def test_unset_or_zero_skips_the_settle_job_for_every_event(self) -> None:
        # GitHub renders an unset repo variable as ''.
        for event in self.EVENTS:
            for var in ("", "0"):
                with self.subTest(event=event, var=var):
                    self.assertFalse(self._settle_runs(event, var))

    def test_only_pull_request_ever_waits(self) -> None:
        for event in self.EVENTS:
            with self.subTest(event=event):
                self.assertEqual(self._settle_runs(event, "180"), event == "pull_request")
        self.assertFalse(self._settle_runs("pull_request", "180", local_proof=True))

    def test_settle_runs_on_the_hosted_preamble_lane_in_parallel(self) -> None:
        self.assertEqual(
            self.settle["runs-on"],
            "${{ fromJSON(vars.PULP_PREAMBLE_RUNS_ON_JSON || '\"ubuntu-latest\"') }}",
        )
        # No `needs`: the wait overlaps resolve-provider/classify rather than
        # stacking after them.
        self.assertNotIn("needs", self.settle)
        self.assertIs(self.settle["continue-on-error"], True)
        self.assertEqual(self.settle["permissions"], {})

    def test_build_waits_on_settle_but_never_reads_its_result(self) -> None:
        build = self.jobs["build"]
        self.assertEqual(
            build["needs"],
            ["resolve-provider", "classify", "protected-receipt-reuse", "pr-gate-settle"],
        )
        condition = " ".join(build["if"].split())
        # A status function is what stops a skipped `needs` entry from
        # skipping the dependent job; without it an unset variable would skip
        # the whole native matrix, including the required macos leg.
        self.assertIn("!cancelled()", condition)
        self.assertNotIn("pr-gate-settle", condition)
        for field in ("name", "runs-on", "strategy"):
            self.assertNotIn("pr-gate-settle", json.dumps(build.get(field)))

    def test_required_macos_bootstraps_do_not_wait(self) -> None:
        for name, job in self.jobs.items():
            if name == "build":
                continue
            with self.subTest(job=name):
                self.assertNotIn("pr-gate-settle", job.get("needs", []) or [])
                self.assertNotIn("pr-gate-settle", json.dumps(job.get("if", "")))
        self.assertEqual(self.jobs["macos"]["needs"], ["resolve-provider", "classify"])
        self.assertEqual(
            self.jobs["macos-merge-group"]["needs"],
            ["resolve-provider", "classify", "protected-receipt-reuse"],
        )

    def _run_step(self, value: str) -> tuple[int, str, list[str]]:
        import os
        import subprocess
        import tempfile

        step = self.settle["steps"][0]
        with tempfile.TemporaryDirectory() as tmp:
            record = Path(tmp) / "sleeps"
            fake = Path(tmp) / "sleep"
            fake.write_text(f'#!/bin/sh\necho "$1" >> "{record}"\n', encoding="utf-8")
            fake.chmod(0o755)
            env = dict(os.environ, **step["env"])
            env["SETTLE_SECONDS"] = value
            env["PATH"] = f"{tmp}:{env['PATH']}"
            proc = subprocess.run(
                ["bash", "-c", step["run"]],
                env=env, text=True, capture_output=True, timeout=30,
            )
            sleeps = record.read_text(encoding="utf-8").split() if record.exists() else []
        return proc.returncode, proc.stdout, sleeps

    def test_valid_values_sleep_that_long(self) -> None:
        for value, expected in (("1", "1"), ("180", "180"), ("0900", "900")):
            with self.subTest(value=value):
                code, _, sleeps = self._run_step(value)
                self.assertEqual(code, 0)
                self.assertEqual(sleeps, [expected])

    def test_invalid_values_are_ignored_with_a_notice(self) -> None:
        for value in ("abc", "-5", "1.5", "60s", " 60", "901", "999999"):
            with self.subTest(value=value):
                code, out, sleeps = self._run_step(value)
                self.assertEqual(code, 0)
                self.assertEqual(sleeps, [])
                self.assertIn("::notice title=PR gate settle ignored::", out)
        code, out, sleeps = self._run_step("00")
        self.assertEqual((code, sleeps), (0, []))
        self.assertNotIn("::notice", out)

    def test_timeout_exceeds_the_cap(self) -> None:
        cap = int(self.settle["steps"][0]["env"]["SETTLE_MAX_SECONDS"])
        self.assertGreater(int(self.settle["timeout-minutes"]) * 60, cap)


class LocalProofWorkflowTest(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = _workflow()
        self.jobs = self.workflow["jobs"]

    def test_local_proof_is_default_off_and_dispatch_only(self) -> None:
        triggers = self.workflow.get("on", self.workflow.get(True, {}))
        local_proof = triggers["workflow_dispatch"]["inputs"]["local_proof"]
        self.assertIs(local_proof["default"], False)
        self.assertEqual(local_proof["type"], "boolean")

        job = self.jobs["local-proof"]
        condition = job["if"]
        self.assertIn("github.event_name == 'workflow_dispatch'", condition)
        self.assertIn("github.ref == 'refs/heads/main'", condition)
        self.assertIn("inputs.local_proof", condition)
        self.assertEqual(job["permissions"], {})

    def test_local_proof_requests_exact_protected_m1_assignment(self) -> None:
        runs_on = self.jobs["local-proof"]["runs-on"]
        self.assertEqual(runs_on["group"], "pulp-trusted-build")
        self.assertEqual(
            runs_on["labels"],
            [
                "self-hosted",
                "macOS",
                "ARM64",
                "pulp-build",
                "pulp-build-vm",
                "pulp-build-merge-group",
            ],
        )

    def test_local_proof_executes_no_repository_source_and_is_bounded(self) -> None:
        job = self.jobs["local-proof"]
        self.assertEqual(job["timeout-minutes"], 12)
        self.assertEqual(len(job["steps"]), 1)
        step = job["steps"][0]
        self.assertNotIn("uses", step)
        self.assertIn("sleep 600", step["run"])
        text = json.dumps(job, sort_keys=True)
        for forbidden in (
            "actions/checkout",
            "github.token",
            "GITHUB_TOKEN",
            "GH_TOKEN",
            "git ",
            "cmake",
            "python",
        ):
            self.assertNotIn(forbidden, text)

    def test_local_proof_has_a_unique_non_cancelling_concurrency_domain(self) -> None:
        concurrency = self.workflow["concurrency"]
        self.assertIn("inputs.local_proof", concurrency["group"])
        self.assertIn("github.run_id", concurrency["group"])
        self.assertIn("github.ref", concurrency["group"])
        self.assertIn("!inputs.local_proof", concurrency["cancel-in-progress"])

    def test_no_job_gates_itself_on_always(self) -> None:
        # `always()` runs a job even when the run has been CANCELLED, so a superseded
        # run keeps building and keeps holding its concurrency group. Every newer head
        # then waits at `pending`, which is indistinguishable from runner starvation,
        # and only a force-cancel (which bypasses `always()`) releases it.
        # `!cancelled()` buys the same thing the `always()` here was for: it still
        # evaluates when an upstream need failed or was skipped.
        offenders = {
            name: " ".join(str(job.get("if", "")).split())
            for name, job in self.jobs.items()
            if "always()" in str(job.get("if", ""))
        }
        self.assertEqual(
            offenders,
            {},
            "job-level `always()` keeps a cancelled run alive and holds the "
            f"concurrency group; use `!cancelled()`. offenders: {offenders}",
        )

    def test_the_always_scan_reaches_the_jobs(self) -> None:
        # Guards the scan above: an empty job map would pass it vacuously.
        self.assertGreater(len(self.jobs), 5)
        self.assertIn("build", self.jobs)
        self.assertEqual(
            {n for n, j in {"a": {"if": "always() && x"}, "b": {"if": "!cancelled()"}}.items()
             if "always()" in str(j.get("if", ""))},
            {"a"},
        )

    def test_cancellation_sensitive_jobs_still_run_on_upstream_failure(self) -> None:
        # `!cancelled()` must not regress the reason the gate was permissive: the
        # alias jobs report an outcome when an upstream need did NOT succeed.
        for name in ("macos", "linux", "windows"):
            with self.subTest(job=name):
                condition = " ".join(str(self.jobs[name].get("if", "")).split())
                self.assertIn("!cancelled()", condition)
                self.assertNotIn("always()", condition)

    def test_local_proof_structurally_suppresses_every_ordinary_job(self) -> None:
        ordinary = set(self.jobs) - {"local-proof"}
        self.assertTrue(ordinary)
        for name in sorted(ordinary):
            with self.subTest(job=name):
                self.assertIn("!inputs.local_proof", str(self.jobs[name].get("if", "")))


class CtestParallelismTest(unittest.TestCase):
    """The non-Windows ctest -j follows a declared tartci lease, capped at 8.

    Runs the derivation lines exactly as build.yml declares them, with a stub
    getconf standing in for the runner's core count. A runner that declares no
    lease keeps the prior -j8, so the change rolls out host by host.
    """

    @classmethod
    def setUpClass(cls) -> None:
        match = re.search(
            r'^( *ctest_jobs=8$.*?)^ *echo "ctest parallelism',
            WORKFLOW,
            re.DOTALL | re.MULTILINE,
        )
        assert match, "build.yml no longer derives ctest_jobs"
        cls.snippet = match.group(1) + 'echo "$ctest_jobs"\n'

    def _jobs(self, cores: str | None, guest: str | None = None) -> int:
        import os
        import subprocess
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            stub = Path(tmp) / "getconf"
            body = f"echo {cores}" if cores is not None else "exit 1"
            stub.write_text(f"#!/bin/bash\n{body}\n", encoding="utf-8")
            stub.chmod(0o755)
            env = {**os.environ, "PATH": f"{tmp}{os.pathsep}{os.environ['PATH']}"}
            env.pop("TARTCI_GUEST_CORES", None)
            if guest is not None:
                env["TARTCI_GUEST_CORES"] = guest
            proc = subprocess.run(
                ["bash", "-c", "set -euo pipefail\n" + self.snippet],
                env=env, capture_output=True, text=True, check=True,
            )
        return int(proc.stdout.strip())

    def test_undeclared_runner_keeps_the_prior_parallelism(self) -> None:
        self.assertEqual(self._jobs("3"), 8)
        self.assertEqual(self._jobs("28"), 8)

    def test_small_guest_is_not_oversubscribed(self) -> None:
        self.assertEqual(self._jobs("3", guest="3"), 3)
        self.assertEqual(self._jobs("6", guest="6"), 6)

    def test_large_guest_is_capped_at_eight(self) -> None:
        self.assertEqual(self._jobs("12", guest="12"), 8)

    def test_declared_lease_can_only_narrow(self) -> None:
        self.assertEqual(self._jobs("12", guest="3"), 3)
        self.assertEqual(self._jobs("6", guest="16"), 6)
        self.assertEqual(self._jobs(None, guest="3"), 3)

    def test_garbage_is_ignored(self) -> None:
        for bad in ("", "0", "abc", "-2"):
            with self.subTest(value=bad):
                self.assertEqual(self._jobs("6", guest=bad), 8)

    def test_non_windows_ctest_uses_the_derived_value(self) -> None:
        self.assertIn('-j"$ctest_jobs" --timeout 120', WORKFLOW)


if __name__ == "__main__":
    unittest.main()
