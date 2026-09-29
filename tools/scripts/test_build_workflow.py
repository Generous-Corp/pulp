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


def _build_steps() -> dict[str, dict[str, object]]:
    steps = _workflow()["jobs"]["build"]["steps"]
    return {step["name"]: step for step in steps if "name" in step}


def _evaluate_if(expression: object, context: dict[str, object]) -> bool:
    """Evaluate a GitHub Actions step condition over a small literal context.

    Supports exactly what these step conditions use: `&&`, `||`, `!`, `==`,
    `!=`, parentheses, single-quoted strings, dotted context names, and the
    status functions. An unknown name is '' (GitHub reads a missing step
    output as an empty string), so a condition that forgets a clause cannot
    pass by raising.
    """
    if isinstance(expression, bool):  # YAML reads a bare `true` as a bool
        return expression
    expression = expression.replace("${{", "").replace("}}", "")
    tokens = re.findall(
        r"\s+|&&|\|\||!=|==|!|\(|\)|'[^']*'|[A-Za-z_][A-Za-z0-9_.\-]*(?:\(\))?",
        expression)
    assert "".join(tokens) == expression, f"unparsed condition: {expression!r}"
    python = []
    for token in tokens:
        if token.isspace():
            python.append(" ")
        elif token in ("&&", "||", "!"):
            python.append({"&&": " and ", "||": " or ", "!": " not "}[token])
        elif token in ("==", "!=", "(", ")") or token.startswith("'"):
            python.append(token)
        elif token in ("true", "false"):
            python.append(repr(token == "true"))
        else:
            python.append(repr(context.get(token, "")))
    return bool(eval("".join(python), {"__builtins__": {}}))  # noqa: S307


class ReadyPullRequestReceiptTest(unittest.TestCase):
    """The receipt is issued from a ready head's full run, and from nothing else."""

    @classmethod
    def setUpClass(cls) -> None:
        steps = _build_steps()
        cls.decide = steps["Decide pull-request test suite"]
        cls.fast = steps["Test fast deterministic tier (pull request head)"]
        cls.full = steps["Test (non-Windows)"]
        cls.issue = steps["Issue exact protected-validation receipt"]
        cls.publish = steps["Publish exact protected-validation receipt"]

    def _context(self, event, suite="", ctest_outcome="", success=True, key="macos",
                 os_name="macOS"):
        return {
            "runner.os": os_name, "github.event_name": event, "matrix.key": key,
            "steps.pr_suite.outputs.suite": suite,
            "steps.ctest.outcome": ctest_outcome,
            "success()": success, "failure()": not success,
        }

    def test_ready_pull_request_runs_the_full_suite_as_evidence(self) -> None:
        ready = self._context("pull_request", suite="full")
        self.assertTrue(_evaluate_if(self.decide["if"], ready))
        self.assertTrue(_evaluate_if(self.fast["if"], ready))
        self.assertTrue(_evaluate_if(self.full["if"], ready))
        self.assertEqual(
            _evaluate_if(self.full["continue-on-error"], ready), True,
            "a ready head's full run must not gate the pull request check")

    def test_ordinary_pull_request_push_keeps_only_the_fast_tier(self) -> None:
        for suite in ("fast", ""):
            ordinary = self._context("pull_request", suite=suite)
            self.assertTrue(_evaluate_if(self.fast["if"], ordinary))
            self.assertFalse(_evaluate_if(self.full["if"], ordinary), suite)

    def test_merge_group_and_push_still_run_the_full_suite_as_the_gate(self) -> None:
        for event in ("merge_group", "push"):
            ctx = self._context(event)
            self.assertTrue(_evaluate_if(self.full["if"], ctx))
            self.assertFalse(_evaluate_if(self.fast["if"], ctx))
            self.assertFalse(_evaluate_if(self.full["continue-on-error"], ctx), event)

    def test_issuer_runs_only_after_a_ready_heads_full_suite_passed(self) -> None:
        passed = self._context("pull_request", suite="full", ctest_outcome="success")
        self.assertTrue(_evaluate_if(self.issue["if"], passed))
        self.assertTrue(_evaluate_if(self.issue["if"], {**passed, "matrix.key": "linux"}))
        refused = {
            "fast tier (Test skipped)": self._context(
                "pull_request", suite="fast", ctest_outcome="skipped"),
            "full suite failed (continue-on-error)": self._context(
                "pull_request", suite="full", ctest_outcome="failure"),
            "earlier step failed": self._context(
                "pull_request", suite="full", ctest_outcome="success", success=False),
            "merge group": self._context("merge_group", ctest_outcome="success"),
            "push": self._context("push", ctest_outcome="success"),
            "windows leg": self._context(
                "pull_request", suite="full", ctest_outcome="success", key="windows"),
        }
        for label, ctx in refused.items():
            self.assertFalse(_evaluate_if(self.issue["if"], ctx), label)

    def test_published_name_is_the_one_the_merge_group_looks_up(self) -> None:
        # The consumer's lookup is the exact-match binding: same target, the
        # PR head the group merged, and the base it merged onto. A receipt
        # published under any other name is simply never found.
        published = self.publish["with"]["name"]
        self.assertEqual(
            published,
            "protected-validation-${{ matrix.key }}-"
            "${{ github.event.pull_request.head.sha }}-"
            "${{ github.event.pull_request.base.sha }}",
        )
        self.assertIn(
            'artifact_name="protected-validation-${target}-${head}-${base}"', WORKFLOW)
        self.assertIn('read -r group base head extra <<< "$parents"', WORKFLOW)


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



class ArtifactUploadResilienceTest(unittest.TestCase):
    """An artifact-service reset after green tests must not red the gate."""

    SDK = "Upload exact GPU-audio SDK (macOS ARM64)"
    WAIT = "Wait before retrying the GPU-audio SDK upload"
    SDK_RETRY = "Upload exact GPU-audio SDK (macOS ARM64, retry)"
    CTEST_LOGS = "Upload ctest logs and JUnit report"

    @classmethod
    def setUpClass(cls) -> None:
        cls.steps = _build_steps()
        cls.order = [step.get("name") for step in _workflow()["jobs"]["build"]["steps"]]

    def test_ctest_log_upload_is_diagnostic_only(self) -> None:
        step = self.steps[self.CTEST_LOGS]
        self.assertIs(step.get("continue-on-error"), True)
        self.assertIn("always()", str(step["if"]))

    def test_sdk_upload_failure_is_retried_after_a_pause(self) -> None:
        first = self.steps[self.SDK]
        step_id = first.get("id")
        self.assertTrue(step_id, "the first SDK upload needs an id the retry can read")
        # Without continue-on-error a failed first attempt fails the job and the
        # retry, which runs only on success(), would never start.
        self.assertIs(first.get("continue-on-error"), True)
        for name in (self.WAIT, self.SDK_RETRY):
            with self.subTest(step=name):
                condition = self.steps[name]["if"]
                self.assertEqual(condition, f"steps.{step_id}.outcome == 'failure'")
                self.assertTrue(_evaluate_if(condition, {f"steps.{step_id}.outcome": "failure"}))
                for outcome in ("success", "skipped", ""):
                    self.assertFalse(
                        _evaluate_if(condition, {f"steps.{step_id}.outcome": outcome}))
        self.assertEqual(
            self.order.index(self.SDK) + 2, self.order.index(self.SDK_RETRY))
        self.assertEqual(self.order.index(self.SDK) + 1, self.order.index(self.WAIT))
        self.assertRegex(self.steps[self.WAIT]["run"], r"sleep [1-9][0-9]+")

    def test_sdk_retry_keeps_the_published_artifact_contract(self) -> None:
        first = self.steps[self.SDK]["with"]
        retry_step = self.steps[self.SDK_RETRY]
        retry = retry_step["with"]
        self.assertEqual(retry_step["uses"], self.steps[self.SDK]["uses"])
        for key in ("name", "path", "if-no-files-found", "retention-days", "compression-level"):
            with self.subTest(key=key):
                self.assertEqual(retry.get(key), first.get(key))
        self.assertIn("pulp-gpu-audio-sdk-${{ github.sha }}-", retry["name"])
        # A reset can land after the artifact record exists.
        self.assertIs(retry.get("overwrite"), True)
        # Consumers rely on the artifact: a job that never published it fails.
        self.assertNotIn("continue-on-error", retry_step)

if __name__ == "__main__":
    unittest.main()
