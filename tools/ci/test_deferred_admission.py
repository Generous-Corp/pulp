#!/usr/bin/env python3
"""Tests for tools/ci/deferred_admission.py and its workflow wiring.

The decision rules run against a fake GitHub; the workflow contract runs
against the real files and against planted breaks, each of which must be
caught.
"""

from __future__ import annotations

import contextlib
import io
import json
import re
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ci"))

import deferred_admission as da  # noqa: E402

DISPATCHER_YML = REPO / ".github/workflows/deferred-tests-admission.yml"
BUILD_YML = REPO / ".github/workflows/build.yml"
SHIPYARD_CONFIG = REPO / ".shipyard" / "config.toml"
RULESET = REPO / ".github/rulesets/main-protection.json"

REPO_NAME = "Generous-Corp/pulp"
HEAD = "a" * 40
OTHER = "b" * 40
JOB_ID = 9001
RUN_ID = 700


def summary(**over) -> dict:
    record = {"schema": da.SUMMARY_SCHEMA, "admission": False, "skipped_for_budget": 111,
              "deferred_to_merge_queue": 111, "passed_tests": ["p1"], "tested_sha": "c" * 40}
    record.update(over)
    return record


def zipped(record: dict | None) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        if record is not None:
            archive.writestr(da.SUMMARY_FILE, json.dumps(record))
    return buffer.getvalue()


class FakeGitHub:
    """The handful of endpoints the decision reads, plus a write log."""

    def __init__(self, *, state="open", head=HEAD, fork=False, verdict="success",
                 verdict_status="completed", armed=False, queued=False, runs=True,
                 attempt=1, job_status="completed", job_conclusion="success",
                 summaries=None, graphql_ok=True, fail_paths=()):
        self.state, self.head, self.fork = state, head, fork
        self.verdict, self.verdict_status = verdict, verdict_status
        self.armed, self.queued, self.runs = armed, queued, runs
        self.attempt, self.job_status, self.job_conclusion = attempt, job_status, job_conclusion
        self.summaries = {1: summary()} if summaries is None else summaries
        self.graphql_ok, self.fail_paths = graphql_ok, fail_paths
        self.writes: list[tuple[str, str]] = []

    def __call__(self, method: str, path: str, body):
        if any(f in path for f in self.fail_paths):
            raise da.ApiError(f"planted failure on {path}")
        if method == "POST" and path == "graphql":
            if not self.graphql_ok:
                return {"data": {"repository": {"pullRequest": None}}}
            return {"data": {"repository": {"pullRequest": {
                "autoMergeRequest": {"enabledAt": "x"} if self.armed else None,
                "mergeQueueEntry": {"state": "QUEUED"} if self.queued else None}}}}
        if method == "POST":
            self.writes.append((method, path))
            return {}
        if re.fullmatch(rf"repos/{REPO_NAME}/pulls/\d+", path):
            return {"state": self.state, "head": {"sha": self.head, "repo": {
                "full_name": "someone/fork" if self.fork else REPO_NAME}}}
        if "/check-runs?check_name=review-verdict" in path:
            if self.verdict is None:
                return {"check_runs": []}
            return {"check_runs": [
                {"id": 1, "app": {"slug": "github-actions"}, "status": "completed",
                 "conclusion": "failure"},
                {"id": 2, "app": {"slug": "github-actions"}, "status": self.verdict_status,
                 "conclusion": self.verdict},
                # Another app's green must never count.
                {"id": 3, "app": {"slug": "someone-else"}, "status": "completed",
                 "conclusion": "success" if self.verdict != "success" else "failure"}]}
        if "/commits/" in path and path.endswith("/pulls"):
            return [{"number": 42, "state": "open", "head": {"sha": self.head,
                                                             "repo": {"full_name": REPO_NAME}}}]
        if f"/workflows/{da.BUILD_WORKFLOW}/runs" in path:
            if not self.runs:
                return {"workflow_runs": []}
            return {"workflow_runs": [{"id": RUN_ID - 1, "run_attempt": 1},
                                      {"id": RUN_ID, "run_attempt": self.attempt}]}
        if f"/runs/{RUN_ID}/attempts/{self.attempt}/jobs" in path:
            return {"jobs": [{"id": 1, "name": "linux", "status": "completed", "conclusion": "success"},
                             {"id": JOB_ID, "name": "macos", "status": self.job_status,
                              "conclusion": self.job_conclusion}]}
        match = re.search(rf"/runs/{RUN_ID}/artifacts\?name={da.ARTIFACT_PREFIX}(\d+)", path)
        if match:
            n = int(match.group(1))
            if n not in self.summaries:
                return {"artifacts": []}
            return {"artifacts": [{"id": 5000 + n, "name": f"{da.ARTIFACT_PREFIX}{n}",
                                   "expired": False}]}
        match = re.search(r"/actions/artifacts/(\d+)/zip$", path)
        if match and method == "GET_RAW":
            return zipped(self.summaries[int(match.group(1)) - 5000])
        raise AssertionError(f"unexpected call {method} {path}")


def comment_event(head=HEAD, login=da.FLEET_APP, body=None) -> dict:
    body = body if body is not None else f"looks right\nreviewer: pulp-review\nverdict: APPROVE head:{head}"
    return {"issue": {"number": 42, "pull_request": {"url": "x"}},
            "comment": {"user": {"login": login}, "body": body}}


def run_event(head=HEAD, prs=(42,)) -> dict:
    return {"workflow_run": {"head_sha": head, "pull_requests": [{"number": n} for n in prs]}}


def decide_with(fake: FakeGitHub, event_name="workflow_run", event=None, mode="enforce"):
    event = event if event is not None else run_event()
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "event.json"
        path.write_text(json.dumps(event), encoding="utf-8")
        args = da.main.__globals__["argparse"].Namespace(
            repo=REPO_NAME, event_name=event_name, event_path=str(path), mode=mode)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), mock.patch.dict("os.environ", {}, clear=False):
            da.cmd_decide(args, fetch=fake)
    line = [l for l in out.getvalue().splitlines() if l.startswith(f"::notice title={da.NOTICE_TITLE}::")]
    assert len(line) == 1, out.getvalue()
    return json.loads(line[0].split("::", 2)[2])


class DecisionTests(unittest.TestCase):
    def test_an_approved_head_that_deferred_tests_is_rerun(self) -> None:
        fake = FakeGitHub()
        record = decide_with(fake)
        self.assertEqual(record["action"], "rerun", record)
        self.assertEqual(fake.writes, [("POST", f"repos/{REPO_NAME}/actions/jobs/{JOB_ID}/rerun")])
        self.assertEqual(record["deferred"], 111)

    def test_the_approve_comment_alone_starts_it(self) -> None:
        fake = FakeGitHub(verdict=None)  # review-verdict has not refreshed yet
        record = decide_with(fake, "issue_comment", comment_event())
        self.assertEqual(record["action"], "rerun", record)

    def test_shadow_mode_reports_and_writes_nothing(self) -> None:
        fake = FakeGitHub()
        record = decide_with(fake, mode="")
        self.assertEqual((record["action"], record["mode"]), ("would-rerun", "shadow"))
        self.assertEqual(fake.writes, [])

    def _none(self, fake: FakeGitHub, needle: str, **kw) -> None:
        record = decide_with(fake, **kw)
        self.assertEqual(record["action"], "none", record)
        self.assertIn(needle, record["reason"])
        self.assertEqual(fake.writes, [])

    def test_admission_attempt_is_never_rerun_again(self) -> None:
        self._none(FakeGitHub(attempt=2, summaries={2: summary(admission=True)}),
                   "already the admission run")

    def test_a_red_macos_is_terminal(self) -> None:
        self._none(FakeGitHub(job_conclusion="failure"), "terminal verdict")

    def test_a_running_macos_waits_for_its_completion(self) -> None:
        self._none(FakeGitHub(job_status="in_progress"), "its completion decides")

    def test_nothing_deferred_needs_nothing(self) -> None:
        self._none(FakeGitHub(summaries={1: summary(skipped_for_budget=0,
                                                    deferred_to_merge_queue=0)}),
                   "deferred nothing")

    def test_an_unapproved_head_is_left_alone(self) -> None:
        self._none(FakeGitHub(verdict="failure"), "not approved")

    def test_a_pending_verdict_is_not_approval(self) -> None:
        self._none(FakeGitHub(verdict="success", verdict_status="in_progress"), "not approved")

    def test_an_armed_head_is_not_touched(self) -> None:
        self._none(FakeGitHub(armed=True), "armed or in the merge queue")

    def test_a_queued_head_is_not_touched(self) -> None:
        # REST auto_merge reads null for a queued pull request (contract row 11).
        self._none(FakeGitHub(queued=True), "armed or in the merge queue")

    def test_an_unreadable_queue_state_is_not_touched(self) -> None:
        self._none(FakeGitHub(graphql_ok=False), "cannot tell whether the merge queue")

    def test_a_fork_is_never_rerun(self) -> None:
        self._none(FakeGitHub(fork=True), "fork")

    def test_a_closed_pull_request_is_left_alone(self) -> None:
        self._none(FakeGitHub(state="closed"), "not open")

    def test_a_stale_event_head_is_left_alone(self) -> None:
        self._none(FakeGitHub(head=OTHER), "not the current head")

    def test_a_missing_summary_fails_open(self) -> None:
        self._none(FakeGitHub(summaries={}), "uploaded no")

    def test_a_summary_of_another_schema_is_ignored(self) -> None:
        self._none(FakeGitHub(summaries={1: summary(schema="other/v9")}), "unknown schema")

    def test_the_rerun_limit_holds(self) -> None:
        self._none(FakeGitHub(attempt=da.MAX_ATTEMPT, summaries={da.MAX_ATTEMPT: summary()}),
                   "re-run limit")

    def test_a_read_error_decides_no_rerun(self) -> None:
        self._none(FakeGitHub(fail_paths=("/attempts/",)), "could not read")

    def test_no_build_run_on_the_head(self) -> None:
        self._none(FakeGitHub(runs=False), "no pull_request macos job")

    def test_a_comment_approving_another_head_does_not_count(self) -> None:
        self._none(FakeGitHub(verdict="failure"), "not approved", event_name="issue_comment",
                   event=comment_event(head=OTHER))

    def test_control_the_rerun_targets_the_gate_job_not_the_first_job(self) -> None:
        fake = FakeGitHub()
        decide_with(fake)
        self.assertNotIn(("POST", f"repos/{REPO_NAME}/actions/jobs/1/rerun"), fake.writes)

    def test_an_app_opened_run_finds_its_pull_request_by_head(self) -> None:
        fake = FakeGitHub()
        record = decide_with(fake, event=run_event(prs=()))
        self.assertEqual((record["action"], record["pr"]), ("rerun", 42))


class CommentTests(unittest.TestCase):
    def test_only_the_fleet_apps_signed_last_line_counts(self) -> None:
        self.assertTrue(da.comment_approves(comment_event()["comment"], HEAD))
        self.assertFalse(da.comment_approves(comment_event(login="someone")["comment"], HEAD))
        self.assertFalse(da.comment_approves(
            comment_event(body=f"verdict: APPROVE head:{HEAD}")["comment"], HEAD))  # unsigned
        self.assertFalse(da.comment_approves(
            comment_event(body=f"reviewer: x\nverdict: APPROVE head:{HEAD}\nmore")["comment"], HEAD))
        self.assertFalse(da.comment_approves(
            comment_event(body=f"reviewer: x\nverdict: HOLD head:{HEAD}")["comment"], HEAD))


class ProbeAndPriorTests(unittest.TestCase):
    def _probe(self, fake, mode) -> int:
        args = da.main.__globals__["argparse"].Namespace(repo=REPO_NAME, head=HEAD, mode=mode)
        with contextlib.redirect_stdout(io.StringIO()):
            return da.cmd_probe(args, fetch=fake)

    def test_probe_is_admission_only_when_approved_and_enforced(self) -> None:
        self.assertEqual(self._probe(FakeGitHub(), "enforce"), 0)
        self.assertEqual(self._probe(FakeGitHub(), "shadow"), 1)
        self.assertEqual(self._probe(FakeGitHub(), ""), 1)
        self.assertEqual(self._probe(FakeGitHub(verdict="failure"), "enforce"), 1)
        self.assertEqual(self._probe(FakeGitHub(verdict=None), "enforce"), 1)
        self.assertEqual(self._probe(FakeGitHub(fail_paths=("check-runs",)), "enforce"), 1)

    def test_prior_takes_the_newest_earlier_attempt(self) -> None:
        fake = FakeGitHub(attempt=3, summaries={1: summary(passed_tests=["old"]),
                                                2: summary(passed_tests=["newer"])})
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "prior.json"
            args = da.main.__globals__["argparse"].Namespace(
                repo=REPO_NAME, run_id=RUN_ID, attempt=3, out=str(out))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(da.cmd_prior(args, fetch=fake), 0)
            self.assertEqual(json.loads(out.read_text())["passed_tests"], ["newer"])

    def test_prior_without_an_earlier_summary_runs_everything(self) -> None:
        fake = FakeGitHub(attempt=2, summaries={})
        with tempfile.TemporaryDirectory() as tmp:
            args = da.main.__globals__["argparse"].Namespace(
                repo=REPO_NAME, run_id=RUN_ID, attempt=2, out=str(Path(tmp) / "p.json"))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(da.cmd_prior(args, fetch=fake), 1)

    def test_an_artifact_without_the_summary_file_is_an_error_not_a_pass(self) -> None:
        fake = FakeGitHub(summaries={1: None})
        with self.assertRaises(da.ApiError):
            da.artifact_summary(fake, REPO_NAME, RUN_ID, 1)


# --- workflow contract ---------------------------------------------------------

def step_block(text: str, name: str) -> str:
    start = text.index(f"- name: {name}")
    end = text.find("\n      - name:", start + 10)
    return text[start:end if end != -1 else len(text)]


def contract_problems(dispatcher: str, build: str, required: list[str]) -> list[str]:
    """Everything the two workflows must keep for this to be safe."""
    problems = []
    if "issue_comment:" not in dispatcher or 'workflows: ["Build and Test"]' not in dispatcher:
        problems.append("dispatcher must trigger on the approve comment and on Build and Test")
    if re.search(r"\n\s+checks:\s*write", dispatcher) or re.search(r"\n\s+contents:\s*write", dispatcher):
        problems.append("dispatcher may not write checks or contents")
    if not re.search(r"\n\s+actions:\s*write", dispatcher):
        problems.append("dispatcher needs actions: write to re-run the job")
    if "pull-requests: write" in dispatcher:
        problems.append("dispatcher may not write pull requests (it never arms or dequeues)")
    if re.search(r"\n\s+ref:", dispatcher) or "head_sha }}" in dispatcher.split("steps:")[1]:
        problems.append("dispatcher may not check out the pull request's code")
    if "head_repository.full_name == github.repository" not in dispatcher:
        problems.append("dispatcher must ignore fork runs")
    if "github.event.comment.user.login == 'shipyard-local[bot]'" not in dispatcher:
        problems.append("dispatcher must only wake on the fleet App's comments")
    if "pull_request_target" in dispatcher:
        problems.append("dispatcher must not run on pull_request_target")
    if "deferred_admission.py decide" not in dispatcher:
        problems.append("dispatcher must call the tested decision")
    names = re.findall(r"\n    name: (.+)", dispatcher)
    if any(n.strip() in required for n in names):
        problems.append("dispatcher job name collides with a required context")
    try:
        affected = step_block(build, "Test what this pull request reaches (pull request head)")
        upload = step_block(build, "Upload the affected-test summary (pull request head)")
    except ValueError as exc:
        return problems + [f"build.yml step missing: {exc}"]
    if "continue-on-error" in affected:
        problems.append("the affected step must stay gating")
    if "deferred_admission.py probe" not in affected or "--admission" not in affected:
        problems.append("the affected step must probe for an admission run")
    if '--summary-json "$GITHUB_WORKSPACE/$PULP_BUILD_DIR/pr-affected-summary/pr-affected-summary.json"' not in affected:
        problems.append("the affected step must write the summary the dispatcher reads")
    cleanup = 'rm -f "$PULP_BUILD_DIR/pr-affected-summary/pr-affected-summary.json"'
    if cleanup not in affected:
        problems.append("a warm build dir's old summary must be removed first")
    elif affected.index(cleanup) > affected.index("python3 tools/ci/pr_head_affected_tests.py"):
        problems.append("the old summary must be removed before the selector runs")
    if "--budget-secs 600" not in affected:
        problems.append("the budgeted run keeps its ten-minute budget")
    if f"name: {da.ARTIFACT_PREFIX}${{{{ github.run_attempt }}}}" not in upload:
        problems.append("the artifact name must carry the run attempt the dispatcher asks for")
    if f"pr-affected-summary/{da.SUMMARY_FILE}" not in upload:
        problems.append("the artifact must hold the summary file the dispatcher unzips")
    if "always()" not in upload:
        problems.append("the summary must upload when a test failed too")
    if build.index("- name: Upload the affected-test summary") < build.index(
            "- name: Test what this pull request reaches"):
        problems.append("the upload must follow the affected step")
    return problems


def required_contexts() -> list[str]:
    text = SHIPYARD_CONFIG.read_text(encoding="utf-8")
    block = text[text.index("required_status_checks = ["):]
    return re.findall(r'"([^"]+)"', block[:block.index("]")])


class WorkflowContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.dispatcher = DISPATCHER_YML.read_text(encoding="utf-8")
        self.build = BUILD_YML.read_text(encoding="utf-8")
        self.required = required_contexts()

    def test_the_real_workflows_keep_the_contract(self) -> None:
        self.assertEqual(contract_problems(self.dispatcher, self.build, self.required), [])

    def test_no_new_required_context(self) -> None:
        # The admission signal is the existing `macos` check; nothing new is required.
        self.assertIn("macos", self.required)
        self.assertNotIn("Re-run an approved head's deferred tests", self.required)
        ruleset = RULESET.read_text(encoding="utf-8") if RULESET.exists() else ""
        self.assertNotIn("deferred", ruleset.lower())

    def test_the_constants_match_the_selector(self) -> None:
        # Read as text: importing the selector would pull its whole closure in.
        selector = (REPO / "tools/ci/pr_head_affected_tests.py").read_text(encoding="utf-8")
        self.assertIn(f'SCHEMA = "{da.SUMMARY_SCHEMA}"', selector)
        self.assertIn('"admission": bool(args.admission)', selector)
        for key in ("skipped_for_budget", "deferred_to_merge_queue", "passed_tests", "tested_sha"):
            self.assertIn(f'"{key}"', selector)

    def _broken(self, *, dispatcher=None, build=None) -> list[str]:
        return contract_problems(dispatcher if dispatcher is not None else self.dispatcher,
                                 build if build is not None else self.build, self.required)

    def test_planted_breaks_are_caught(self) -> None:
        d, b = self.dispatcher, self.build
        breaks = {
            "checks write": dict(dispatcher=d.replace("checks: read", "checks: write")),
            "pull request write": dict(dispatcher=d.replace("pull-requests: read", "pull-requests: write")),
            "no actions write": dict(dispatcher=d.replace("actions: write", "actions: read")),
            "fork guard dropped": dict(dispatcher=d.replace(
                "&& github.event.workflow_run.head_repository.full_name == github.repository", "")),
            "any commenter": dict(dispatcher=d.replace(
                "&& github.event.comment.user.login == 'shipyard-local[bot]'", "")),
            "checks out the head": dict(dispatcher=d.replace(
                "persist-credentials: false",
                "persist-credentials: false\n          ref: ${{ github.event.workflow_run.head_sha }}")),
            "name collides": dict(dispatcher=d.replace(
                "name: Re-run an approved head's deferred tests", "name: macos")),
            "no probe": dict(build=b.replace("deferred_admission.py probe", "deferred_admission.py nope")),
            "no summary": dict(build=b.replace("--summary-json ", "--no-summary ")),
            "no stale cleanup": dict(build=b.replace(
                'rm -f "$PULP_BUILD_DIR/pr-affected-summary/pr-affected-summary.json"', ":")),
            "artifact without attempt": dict(build=b.replace(
                "name: pr-head-affected-tests-a${{ github.run_attempt }}",
                "name: pr-head-affected-tests")),
            "upload only on success": dict(build=b.replace(
                "if: always() && steps.pr_affected_tests.outcome != 'skipped'",
                "if: steps.pr_affected_tests.outcome == 'success'")),
            "affected step non-gating": dict(build=b.replace(
                "        id: pr_affected_tests\n",
                "        id: pr_affected_tests\n        continue-on-error: true\n")),
            "budget dropped": dict(build=b.replace("--budget-secs 600 --jobs 4", "--jobs 4")),
        }
        for label, change in breaks.items():
            with self.subTest(label):
                variant = change.get("dispatcher", d), change.get("build", b)
                self.assertNotEqual(variant, (d, b), f"planted break {label!r} changed nothing")
                self.assertNotEqual(self._broken(**change), [], f"planted break {label!r} was not caught")


if __name__ == "__main__":
    unittest.main()
