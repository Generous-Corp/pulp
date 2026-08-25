#!/usr/bin/env python3
"""The Vellum gates must recover or resolve PR refs without widening trust.

Both gates post or gate a REQUIRED check, and both can recover a cancelled run
without pushing a commit. The ordinary freeze check may execute PR code, so it
must expose only `pull_request` and `merge_group`. Its separate hosted recovery
workflow may expose only `workflow_dispatch`, may not check out or execute code,
and may only re-run an existing exact-head pull_request-context run.

So this extracts the resolve step from each workflow and runs it, with `gh` and
`git` faked. Re-implementing the branch here would pass forever after the
workflow stopped doing it.
"""

from __future__ import annotations

import copy
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = REPO_ROOT / ".github" / "workflows"

FAKE_GH = textwrap.dedent(
    """\
    #!/bin/sh
    [ -z "${GH_CALL_LOG:-}" ] || printf '%s\\n' "$*" >> "$GH_CALL_LOG"
    body=''
    case "$*" in
      *git/ref/heads/main*) body='{"object":{"sha":"CURRENT_MAIN"}}' ;;
      *pulls/111*) body='{"state":"open","base":{"sha":"STALE_PR_BASE","ref":"main","repo":{"full_name":"Generous-Corp/pulp"}},"head":{"sha":"HEAD_OPEN"}}' ;;
      *pulls/112*) body='{"state":"open","base":{"sha":"BASE_112","ref":"main","repo":{"full_name":"Generous-Corp/pulp"}},"head":{"sha":"HEAD_112"}}' ;;
      *pulls/113*) body='{"state":"open","base":{"sha":"BASE_113","ref":"main","repo":{"full_name":"Generous-Corp/pulp"}},"head":{"sha":"HEAD_113"}}' ;;
      *pulls/114*) body='{"state":"open","base":{"sha":"BASE_114","ref":"main","repo":{"full_name":"Generous-Corp/pulp"}},"head":{"sha":"HEAD_114"}}' ;;
      *pulls/115*) body='{"state":"open","base":{"sha":"BASE_115","ref":"main","repo":{"full_name":"Generous-Corp/pulp"}},"head":{"sha":"HEAD_115"}}' ;;
      *pulls/116*) body='{"state":"open","base":{"sha":"BASE_116","ref":"main","repo":{"full_name":"Generous-Corp/pulp"}},"head":{"sha":"HEAD_116"}}' ;;
      *pulls/117*) body='{"state":"open","base":{"sha":"BASE_117","ref":"main","repo":{"full_name":"Generous-Corp/pulp"}},"head":{"sha":"HEAD_117"}}' ;;
      *pulls/222*) body='{"state":"closed","base":{"sha":"BASE_SHUT","ref":"main","repo":{"full_name":"Generous-Corp/pulp"}},"head":{"sha":"HEAD_SHUT"}}' ;;
      *pulls/333*) body='{"state":"open","base":{"sha":"BASE_OTHER","ref":"main","repo":{"full_name":"attacker/pulp"}},"head":{"sha":"HEAD_OTHER"}}' ;;
      *pulls/444*) body='{"state":"open","base":{"sha":"BASE_OTHER","ref":"develop","repo":{"full_name":"Generous-Corp/pulp"}},"head":{"sha":"HEAD_OTHER"}}' ;;
      *vellum-freeze-check.yml/runs*HEAD_OPEN*) body='{"workflow_runs":[{"id":9001,"event":"pull_request","head_sha":"HEAD_OPEN","status":"completed","conclusion":"failure","created_at":"2026-08-25T00:00:00Z","pull_requests":[{"number":111,"head":{"sha":"HEAD_OPEN"},"base":{"sha":"STALE_PR_BASE"}}]}]}' ;;
      *vellum-freeze-check.yml/runs*HEAD_112*) body='{"workflow_runs":[{"id":9012,"event":"workflow_dispatch","head_sha":"HEAD_112","status":"completed","conclusion":"failure","created_at":"2026-08-25T00:00:00Z","pull_requests":[{"number":112,"head":{"sha":"HEAD_112"},"base":{"sha":"BASE_112"}}]}]}' ;;
      *vellum-freeze-check.yml/runs*HEAD_113*) body='{"workflow_runs":[{"id":9013,"event":"pull_request","head_sha":"HEAD_113","status":"completed","conclusion":"failure","created_at":"2026-08-25T00:00:00Z","pull_requests":[{"number":999,"head":{"sha":"HEAD_113"},"base":{"sha":"BASE_113"}}]}]}' ;;
      *vellum-freeze-check.yml/runs*HEAD_114*) body='{"workflow_runs":[{"id":9014,"event":"pull_request","head_sha":"OTHER_HEAD","status":"completed","conclusion":"failure","created_at":"2026-08-25T00:00:00Z","pull_requests":[{"number":114,"head":{"sha":"OTHER_HEAD"},"base":{"sha":"BASE_114"}}]}]}' ;;
      *vellum-freeze-check.yml/runs*HEAD_115*) body='{"workflow_runs":[{"id":9015,"event":"pull_request","head_sha":"HEAD_115","status":"completed","conclusion":"failure","created_at":"2026-08-25T00:00:00Z","pull_requests":[{"number":115,"head":{"sha":"HEAD_115"},"base":{"sha":"OTHER_BASE"}}]}]}' ;;
      *vellum-freeze-check.yml/runs*HEAD_116*) body='{"workflow_runs":[{"id":9016,"event":"pull_request","head_sha":"HEAD_116","status":"in_progress","conclusion":null,"created_at":"2026-08-25T00:00:00Z","pull_requests":[{"number":116,"head":{"sha":"HEAD_116"},"base":{"sha":"BASE_116"}}]}]}' ;;
      *vellum-freeze-check.yml/runs*HEAD_117*) body='{"workflow_runs":[{"id":9017,"event":"pull_request","head_sha":"HEAD_117","status":"completed","conclusion":"success","created_at":"2026-08-25T00:00:00Z","pull_requests":[{"number":117,"head":{"sha":"HEAD_117"},"base":{"sha":"BASE_117"}}]}]}' ;;
      *actions/runs/*/rerun*) exit 0 ;;
    esac
    prev=''
    for a in "$@"; do
      if [ "$prev" = "--jq" ]; then printf '%s' "$body" | jq -r "$a"; exit 0; fi
      prev="$a"
    done
    printf '%s\\n' "$body"
    """
)

# The dispatch path fetches the base and reads the already-checked-out merge
# commit; neither should reach the network in a test.
FAKE_GIT = textwrap.dedent(
    """\
    #!/bin/sh
    case "$1" in
      rev-parse) echo MERGE_COMMIT ;;
      *) exit 0 ;;
    esac
    """
)


def _step_script(workflow: str, job: str, step_id: str) -> str:
    try:
        import yaml
    except ImportError:  # pragma: no cover - environment-dependent
        raise unittest.SkipTest("PyYAML not installed")
    doc = yaml.safe_load((WORKFLOWS / workflow).read_text())
    steps = doc["jobs"][job]["steps"]
    step = next((s for s in steps if s.get("id") == step_id), None)
    if step is None:
        raise AssertionError(
            f"{workflow}: no step id={step_id!r} in job {job!r} — the dispatch "
            "plumbing was renamed or removed"
        )
    return step["run"]


def _run(
    script: str,
    env: dict[str, str],
    *,
    calls: list[str] | None = None,
) -> tuple[int, dict[str, str], str]:
    with tempfile.TemporaryDirectory() as tmp:
        binp = Path(tmp) / "bin"
        binp.mkdir()
        (binp / "gh").write_text(FAKE_GH)
        (binp / "git").write_text(FAKE_GIT)
        for tool in ("gh", "git"):
            (binp / tool).chmod(0o755)

        body = Path(tmp) / "step.sh"
        body.write_text(script)
        out = Path(tmp) / "github_output"
        out.write_text("")
        call_log = Path(tmp) / "gh-calls"
        call_log.write_text("")

        # Drop the ambient GITHUB_* namespace before building the fake one.
        # These tests run INSIDE Actions, where every one of these is already
        # set to the real run's values — so a `setdefault` silently keeps the
        # runner's value and the case under test never happens. That is not
        # hypothetical: GITHUB_SHA defaulted to "EVENT_MERGE" locally and to the
        # real head SHA on CI, so `test_pull_request_path_unchanged` passed on a
        # laptop and failed in CI on the same commit. Scrubbing the prefix
        # rather than overwriting the four names read today keeps a step that
        # starts reading a fifth from inheriting the runner's.
        full = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
        full["PATH"] = f"{binp}:{os.environ['PATH']}"
        full["GITHUB_OUTPUT"] = str(out)
        full["GITHUB_REPOSITORY"] = "Generous-Corp/pulp"
        full["GH_TOKEN"] = "x"
        full["GH_CALL_LOG"] = str(call_log)
        full["GITHUB_SHA"] = "EVENT_MERGE"
        for key in ("DISPATCH_PR", "PR_BASE", "PR_SOURCE_HEAD", "MERGE_BASE",
                    "MERGE_HEAD", "EVENT_BASE", "EVENT_HEAD", "EVENT_NUMBER",
                    "EVENT_STATE"):
            full[key] = ""
        full.update(env)

        proc = subprocess.run(["bash", str(body)], env=full, cwd=tmp,
                              capture_output=True, text=True)
        parsed = dict(
            line.split("=", 1)
            for line in out.read_text().splitlines()
            if "=" in line
        )
        if calls is not None:
            calls.extend(call_log.read_text().splitlines())
        return proc.returncode, parsed, proc.stderr


class TrustedGateResolve(unittest.TestCase):
    def script(self) -> str:
        return _step_script("vellum-trusted-gate.yml", "trusted-gate", "pr")

    def test_event_path_refreshes_live_pr_state(self):
        rc, out, _ = _run(self.script(), {
            "GITHUB_EVENT_NAME": "pull_request_target",
            "EVENT_BASE": "STALE_B", "EVENT_HEAD": "STALE_H",
            "EVENT_NUMBER": "111",
            "EVENT_STATE": "open",
        })
        self.assertEqual(rc, 0)
        self.assertEqual(out, {
            "active": "true", "base_sha": "CURRENT_MAIN",
            "head_sha": "HEAD_OPEN", "number": "111",
        })

    def test_behind_pr_uses_protected_main_not_stale_pr_base(self):
        rc, out, _ = _run(self.script(), {
            "GITHUB_EVENT_NAME": "workflow_dispatch", "DISPATCH_PR": "111",
        })
        self.assertEqual(rc, 0)
        self.assertEqual(out["base_sha"], "CURRENT_MAIN")
        self.assertNotEqual(out["base_sha"], "STALE_PR_BASE")

    def test_delayed_event_for_closed_pr_is_an_inert_success(self):
        rc, out, err = _run(self.script(), {
            "GITHUB_EVENT_NAME": "pull_request_target",
            "EVENT_BASE": "B", "EVENT_HEAD": "H", "EVENT_NUMBER": "222",
            "EVENT_STATE": "open",
        })
        self.assertEqual(rc, 0)
        self.assertEqual(out, {"active": "false"})
        self.assertIn("stale pull_request_target event", err)

    def test_dispatch_path_resolves_from_the_api(self):
        rc, out, _ = _run(self.script(), {
            "GITHUB_EVENT_NAME": "workflow_dispatch", "DISPATCH_PR": "111",
        })
        self.assertEqual(rc, 0)
        self.assertEqual(out["active"], "true")
        self.assertEqual(out["base_sha"], "CURRENT_MAIN")
        self.assertEqual(out["head_sha"], "HEAD_OPEN")
        self.assertEqual(out["number"], "111")

    def test_dispatch_refuses_a_closed_pr(self):
        # This job posts a commit status. Doing that for a merged PR would put a
        # fresh pending/failure row on history nobody can act on.
        rc, out, err = _run(self.script(), {
            "GITHUB_EVENT_NAME": "workflow_dispatch", "DISPATCH_PR": "222",
        })
        self.assertEqual(rc, 1)
        self.assertEqual(out, {})
        self.assertIn("closed", err)

    def test_dispatch_refuses_a_pr_targeting_another_repository(self):
        rc, out, err = _run(self.script(), {
            "GITHUB_EVENT_NAME": "workflow_dispatch", "DISPATCH_PR": "333",
        })
        self.assertEqual(rc, 1)
        self.assertEqual(out, {})
        self.assertIn("only Generous-Corp/pulp:main is trusted", err)

    def test_dispatch_refuses_a_pr_targeting_another_branch(self):
        rc, out, err = _run(self.script(), {
            "GITHUB_EVENT_NAME": "workflow_dispatch", "DISPATCH_PR": "444",
        })
        self.assertEqual(rc, 1)
        self.assertEqual(out, {})
        self.assertIn("only Generous-Corp/pulp:main is trusted", err)


class FreezeCheckResolve(unittest.TestCase):
    def script(self) -> str:
        return _step_script("vellum-freeze-check.yml", "freeze-check", "comparison")

    def test_pull_request_path_unchanged(self):
        rc, out, _ = _run(self.script(), {
            "GITHUB_EVENT_NAME": "pull_request",
            "PR_BASE": "B", "PR_SOURCE_HEAD": "SH",
        })
        self.assertEqual(rc, 0)
        self.assertEqual(out, {"base": "B", "head": "EVENT_MERGE", "source_head": "SH"})

    def test_merge_group_path_unchanged(self):
        rc, out, _ = _run(self.script(), {
            "GITHUB_EVENT_NAME": "merge_group",
            "MERGE_BASE": "MB", "MERGE_HEAD": "MH",
        })
        self.assertEqual(rc, 0)
        self.assertEqual(out, {"base": "MB", "head": "MH", "source_head": "MH"})

    def test_validation_workflow_has_only_untrusted_validation_events(self):
        doc = _workflow("vellum-freeze-check.yml")
        on = doc.get(True) or doc.get("on")
        self.assertEqual(set(on), {"pull_request", "merge_group"})
        self.assertNotIn("if", doc["jobs"]["freeze-check"])
        self.assertEqual(doc["jobs"]["freeze-check"]["name"], "Vellum freeze")


def _workflow(name: str) -> dict:
    try:
        import yaml
    except ImportError:  # pragma: no cover - environment-dependent
        raise unittest.SkipTest("PyYAML not installed")
    return yaml.safe_load((WORKFLOWS / name).read_text())


class FreezeCheckDispatchRecovery(unittest.TestCase):
    def script(self) -> str:
        doc = _workflow("vellum-freeze-recovery.yml")
        steps = doc["jobs"]["rerun-pr-context"]["steps"]
        return next(step["run"] for step in steps if "run" in step)

    def test_dispatch_reruns_existing_exact_pull_request_context(self):
        calls: list[str] = []
        rc, _, err = _run(self.script(), {
            "GITHUB_EVENT_NAME": "workflow_dispatch", "DISPATCH_PR": "111",
        }, calls=calls)
        self.assertEqual(rc, 0, err)
        self.assertTrue(any("actions/runs/9001/rerun" in call for call in calls))

    def test_dispatch_refuses_a_closed_pr(self):
        rc, _, err = _run(self.script(), {
            "GITHUB_EVENT_NAME": "workflow_dispatch", "DISPATCH_PR": "222",
        })
        self.assertEqual(rc, 1)
        self.assertIn("closed", err)

    def test_dispatch_rejects_every_inexact_or_competing_candidate(self):
        cases = {
            "112": "wrong event",
            "113": "wrong PR number",
            "114": "wrong source head",
            "115": "wrong base head",
            "116": "already in progress",
            "117": "already successful",
        }
        for number, reason in cases.items():
            with self.subTest(reason=reason):
                calls: list[str] = []
                rc, _, _ = _run(self.script(), {
                    "GITHUB_EVENT_NAME": "workflow_dispatch",
                    "DISPATCH_PR": number,
                }, calls=calls)
                self.assertEqual(rc, 1)
                self.assertFalse(any("/rerun" in call for call in calls), calls)

    def test_dispatch_refuses_wrong_repository_or_base_branch(self):
        for number in ("333", "444"):
            with self.subTest(number=number):
                calls: list[str] = []
                rc, _, err = _run(self.script(), {
                    "GITHUB_EVENT_NAME": "workflow_dispatch",
                    "DISPATCH_PR": number,
                }, calls=calls)
                self.assertEqual(rc, 1)
                self.assertIn("only Generous-Corp/pulp:main is trusted", err)
                self.assertFalse(any("/rerun" in call for call in calls), calls)

    def test_dispatch_job_cannot_checkout_or_execute_pr_code(self):
        doc = _workflow("vellum-freeze-recovery.yml")
        job = doc["jobs"]["rerun-pr-context"]
        self.assertEqual(_dispatch_safety_errors(job), [])
        self.assertEqual(job["if"], "github.ref == 'refs/heads/main'")

    def test_recovery_serializes_one_pr_without_cancelling_active_work(self):
        doc = _workflow("vellum-freeze-recovery.yml")
        self.assertEqual(
            doc["concurrency"]["group"],
            "vellum-freeze-recovery-${{ inputs.pr_number }}",
        )
        self.assertIs(doc["concurrency"]["cancel-in-progress"], False)

    def test_security_contract_detects_checkout_mutation(self):
        doc = _workflow("vellum-freeze-recovery.yml")
        mutated = copy.deepcopy(doc["jobs"]["rerun-pr-context"])
        mutated["steps"].insert(0, {"uses": "actions/checkout@v4"})
        self.assertIn(
            "dispatch controller must not check out code",
            _dispatch_safety_errors(mutated),
        )

    def test_security_contract_detects_untrusted_command_mutation(self):
        doc = _workflow("vellum-freeze-recovery.yml")
        mutated = copy.deepcopy(doc["jobs"]["rerun-pr-context"])
        mutated["steps"][0]["run"] += "\nbash -c '${{ inputs.pr_number }}'\n"
        self.assertIn(
            "dispatch controller must not interpolate inputs into commands",
            _dispatch_safety_errors(mutated),
        )


def _dispatch_safety_errors(job: dict) -> list[str]:
    errors = []
    if job.get("runs-on") != "ubuntu-latest":
        errors.append("dispatch controller must use an ephemeral hosted runner")
    if job.get("permissions") != {"actions": "write", "contents": "read"}:
        errors.append("dispatch controller permissions drifted")
    serialized = str(job)
    if "actions/checkout" in serialized or "refs/pull/" in serialized:
        errors.append("dispatch controller must not check out code")
    steps = job.get("steps", [])
    if len(steps) != 1 or any("uses" in step for step in steps):
        errors.append("dispatch controller must contain one local controller step")
    run = "\n".join(str(step.get("run", "")) for step in steps)
    if "${{" in run:
        errors.append("dispatch controller must not interpolate inputs into commands")
    if any(token in run for token in ("curl ", "wget ", "eval ", "bash -c", "git ")):
        errors.append("dispatch controller must not fetch or execute untrusted code")
    if "actions/runs/$run_id/rerun" not in serialized:
        errors.append("dispatch controller must recover the restricted PR run")
    return errors


class DispatchIsDeclared(unittest.TestCase):
    def test_recovery_and_validation_triggers_are_disjoint(self):
        validation = _workflow("vellum-freeze-check.yml")
        recovery = _workflow("vellum-freeze-recovery.yml")
        validation_on = validation.get(True) or validation.get("on")
        recovery_on = recovery.get(True) or recovery.get("on")
        self.assertEqual(set(validation_on), {"pull_request", "merge_group"})
        self.assertEqual(set(recovery_on), {"workflow_dispatch"})
        self.assertTrue(set(validation_on).isdisjoint(recovery_on))
        self.assertIn("pr_number", recovery_on["workflow_dispatch"]["inputs"])

    def test_trusted_gate_still_has_its_own_manual_dispatch(self):
        trusted = _workflow("vellum-trusted-gate.yml")
        on = trusted.get(True) or trusted.get("on")
        self.assertIn("workflow_dispatch", on)
        self.assertIn("pr_number", on["workflow_dispatch"]["inputs"])


class TrustedGateScheduling(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import yaml
        except ImportError:  # pragma: no cover
            raise unittest.SkipTest("PyYAML not installed")
        cls.workflow = yaml.safe_load(
            (WORKFLOWS / "vellum-trusted-gate.yml").read_text()
        )

    def test_concurrency_deduplicates_one_pr_without_cancelling_active_gate(self):
        concurrency = self.workflow["concurrency"]
        self.assertEqual(
            concurrency["group"],
            "vellum-trusted-${{ github.event.pull_request.number || "
            "inputs.pr_number || github.ref }}",
        )
        self.assertIs(concurrency["cancel-in-progress"], False)

    def test_closed_target_event_is_filtered_before_any_step_runs(self):
        condition = self.workflow["jobs"]["trusted-gate"]["if"]
        self.assertIn("github.event.pull_request.state == 'open'", condition)
        self.assertIn("github.event_name == 'workflow_dispatch'", condition)
        self.assertIn("github.ref == 'refs/heads/main'", condition)

    def test_only_live_open_prs_reach_status_publishing_steps(self):
        steps = self.workflow["jobs"]["trusted-gate"]["steps"]
        resolve_index = next(i for i, step in enumerate(steps) if step.get("id") == "pr")
        for step in steps[resolve_index + 1:]:
            self.assertEqual(step.get("if"), "steps.pr.outputs.active == 'true'")

    def test_pending_status_is_published_only_after_merge_ref_exists(self):
        steps = self.workflow["jobs"]["trusted-gate"]["steps"]
        script = next(
            step["run"]
            for step in steps
            if step.get("name") == "Validate proposed data and publish head status"
        )
        fetch = script.index('"refs/pull/$PR_NUMBER/merge:refs/vellum/pr-merge"')
        live_state = script.index('live_state=$(gh api', fetch)
        pending = script.index("post_status pending")
        self.assertLess(fetch, live_state)
        self.assertLess(live_state, pending)
        self.assertIn('if [ "$live_state" != "open" ]', script)
        self.assertIn("trap - EXIT", script[live_state:pending])

    def test_each_status_write_rechecks_live_pr_state(self):
        steps = self.workflow["jobs"]["trusted-gate"]["steps"]
        script = next(
            step["run"]
            for step in steps
            if step.get("name") == "Validate proposed data and publish head status"
        )
        function = script[script.index("post_status() {"):script.index("# A permanently")]
        state_check = function.index('pulls/$PR_NUMBER" --jq .state')
        status_write = function.index('statuses/$PR_HEAD"')
        self.assertLess(state_check, status_write)
        self.assertIn('if [ "$live_state" != "open" ]', function)
        self.assertIn("suppressing stale $1 status", function)

    def test_merge_group_job_remains_independent(self):
        merge_job = self.workflow["jobs"]["trusted-merge-group"]
        self.assertEqual(merge_job["if"], "github.event_name == 'merge_group'")


if __name__ == "__main__":
    unittest.main(verbosity=2)
