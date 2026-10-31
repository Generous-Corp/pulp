#!/usr/bin/env python3
"""The macos tier record: the more conservative of the run and the steps.

The run must be the fleet App's dispatch of an approved head; the steps must
show the full lane. Runs the workflow's own step bash against a fake `gh` that
answers with raw API JSON, so the real jq that reads the jobs, pull requests
and verdict comments is what runs.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github/workflows"
WORKFLOW = WORKFLOWS / "macos-tier-record.yml"
BUILD = WORKFLOWS / "build.yml"
REPO = "Generous-Corp/pulp"
HEAD = "a" * 40
MERGE, BASE = "d" * 40, "c" * 40
TESTED = f"Tested refs/pull/7/merge at {MERGE} onto {BASE}"
APP = "shipyard-local[bot]"
CREATED = "2026-10-25T12:00:00Z"
BEFORE, AFTER = "2026-10-25T11:00:00Z", "2026-10-25T13:00:00Z"
BOOTSTRAP = "macOS PR/dispatch bootstrap (required when native leg is absent)"
# The other steps of that job. On a `pr_number` dispatch the replay record is
# written but not uploaded, so the upload and its announcement are skipped.
BOOTSTRAP_RECORD_STEPS = {
    "Check out the reuse record writer": "success",
    "Record that no suite ran, for reuse replay": "success",
    "Upload reuse replay record": "skipped",
    "Announce a missing reuse replay record": "skipped",
}

# The jobs allowed `checks: write`. Each runs no pull request code: the macOS
# retarget's check publishers and reconciler (no checkout) and the
# required-check machinery report (checks out `main`). The tier record is not
# among them: it is written with the tier-record App's token.
CHECKS_WRITERS = {
    ("build-macos.yml", "publish-macos-pending"),
    ("build-macos.yml", "complete-macos-check"),
    ("build-macos-reconcile.yml", "reconcile"),
    ("required-check-machinery.yml", "report"),
}

# Answers `gh <args>` from a {substring: body} fixture, first match wins, and
# logs every call. A body of "!FAIL" exits non-zero.
FAKE_GH = """#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
fixture = json.load(open(os.environ["FAKE_GH_FIXTURE"], encoding="utf-8"))
with open(os.environ["FAKE_GH_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps(args) + "\\n")
text = " ".join(args)
for key, body in fixture.items():
    if key in text:
        if body == "!FAIL":
            sys.stderr.write("HTTP 502: Bad Gateway\\n")
            sys.exit(1)
        sys.stdout.write(body)
        break
"""


def effective_checks(workflow: dict, body: dict) -> str:
    """The `checks` scope a job's GITHUB_TOKEN gets.

    A job-level block replaces the workflow's; a scope a block leaves out is
    `none`. With no block at all the repository default applies, which this
    treats as write so the lint fails closed.
    """
    block = body["permissions"] if "permissions" in body else workflow.get("permissions")
    if block is None:
        return "write"
    if isinstance(block, str):
        return "write" if block == "write-all" else ("read" if block == "read-all" else "none")
    return block.get("checks", "none")


def job(name="macos", head=HEAD, **steps: str) -> dict:
    return {"name": name, "head_sha": head,
            "steps": [{"name": step, "conclusion": conclusion} for step, conclusion in steps.items()]}


def native(build: str, full_suite: str = "skipped", tested: str | None = TESTED,
           tested_conclusion: str = "success") -> dict:
    """A native macos job; a `pr_number` dispatch's has build.yml's Tested step."""
    steps = {}
    if tested:
        steps[tested] = tested_conclusion
    steps.update({"Build": build, "Test fast deterministic tier (pull request head)": "success",
                  "Test (non-Windows)": full_suite})
    return job(**steps)


def bootstrap(conclusion: str = "success", tested: str | None = TESTED) -> dict:
    """The skip-safe `macos` job as build.yml runs it for a `pr_number` dispatch:
    the bootstrap step, then the Tested step naming the merge commit classify
    bound, then the replay-record steps. A plain pull_request run has no Tested
    step."""
    steps = {BOOTSTRAP: conclusion}
    if tested:
        steps[tested] = "success"
    steps.update(BOOTSTRAP_RECORD_STEPS)
    return job(**steps)


def verdict(word: str = "APPROVE", head: str = HEAD, author: str = APP, at: str = BEFORE,
            reviewer: bool = True) -> dict:
    body = ("reviewer: pulp-review2\n\n" if reviewer else "") + "Looked at it.\n\n" + \
        f"verdict: {word} head:{head}\n"
    return {"user": {"login": author}, "created_at": at, "body": body}


def pages(items: list, key: str | None, count: int = 2) -> str:
    """`gh api --paginate` output: one JSON document per page."""
    size = max(1, -(-len(items) // count))
    chunks = [items[i:i + size] for i in range(0, len(items), size)] or [[]]
    return "".join(json.dumps({"total_count": len(items), key: chunk} if key else chunk) + "\n"
                   for chunk in chunks)


class Harness(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
        cls.job = cls.workflow["jobs"]["tier-record"]
        cls.steps = {step["name"]: step for step in cls.job["steps"]}

    def _bash(self, step: str, env: dict[str, str], fixture: dict[str, str]):
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = Path(tmp) / "bin"
            bin_dir.mkdir()
            gh = bin_dir / "gh"
            gh.write_text(FAKE_GH, encoding="utf-8")
            gh.chmod(0o755)
            (Path(tmp) / "fixture.json").write_text(json.dumps(fixture), encoding="utf-8")
            log, output = Path(tmp) / "log", Path(tmp) / "output"
            log.write_text("", encoding="utf-8")
            output.write_text("", encoding="utf-8")
            full_env = {**os.environ, "PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}",
                        "FAKE_GH_FIXTURE": str(Path(tmp) / "fixture.json"),
                        "FAKE_GH_LOG": str(log), "GITHUB_OUTPUT": str(output),
                        "GITHUB_REPOSITORY": REPO, **env}
            proc = subprocess.run(["bash", "-c", self.steps[step]["run"]], env=full_env,
                                  capture_output=True, text=True, encoding="utf-8", check=False)
            calls = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
            outputs = dict(line.split("=", 1) for line in output.read_text(encoding="utf-8").splitlines())
            return proc, calls, outputs


class TierDecisionTest(Harness):
    def _decide(self, jobs, comments=(), run_prs="7", pulls=None, head_repo=REPO,
                event="workflow_dispatch", actor=APP, parents=f"{BASE} {HEAD}\n"):
        fixture = {
            "/attempts/2/jobs": jobs if isinstance(jobs, str) else pages(list(jobs), "jobs"),
            # The tested merge commit's parents, as `--jq` prints them.
            f"/commits/{MERGE} ": parents,
            "/commits/": json.dumps(pulls if pulls is not None else []),
            "/issues/7/comments": comments if isinstance(comments, str) else pages(list(comments), None),
        }
        env = {"GH_TOKEN": "read-only", "RUN_ID": "4242", "RUN_ATTEMPT": "2",
               "RUN_CREATED_AT": CREATED, "HEAD_SHA": HEAD, "HEAD_REPO": head_repo,
               "RUN_PRS": run_prs, "TRIGGER_EVENT": event, "TRIGGERING_ACTOR": actor}
        return self._bash("Decide the tier", env, fixture)

    def _tier(self, jobs, comments=(), **kwargs) -> str | None:
        proc, _, outputs = self._decide(jobs, comments, **kwargs)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return outputs.get("tier")

    # --- both signals ------------------------------------------------------

    def test_the_apps_dispatch_of_an_approved_head_with_a_build_is_the_full_lane(self) -> None:
        self.assertEqual(self._tier([native("success")], [verdict()]), "fast")
        self.assertEqual(self._tier([native("success", full_suite="success")], [verdict()]), "full")
        # A failed build ran too; the check's own red is what blocks it.
        self.assertEqual(self._tier([native("failure")], [verdict()]), "fast")

    def test_a_pull_request_run_is_deferred_whatever_its_steps_say(self) -> None:
        # Labels no longer start runs: the full lane comes from a dispatch.
        self.assertEqual(self._tier([native("success", full_suite="success")], [verdict()],
                                    event="pull_request"), "deferred")

    def test_a_dispatch_or_rerun_by_anyone_else_is_deferred(self) -> None:
        for actor in ("a-maintainer", "github-actions[bot]", ""):
            with self.subTest(actor=actor):
                self.assertEqual(self._tier([native("success", full_suite="success")], [verdict()],
                                            actor=actor), "deferred")

    def test_an_approved_dispatch_whose_build_was_skipped_is_deferred(self) -> None:
        self.assertEqual(self._tier([native("skipped")], [verdict()]), "deferred")

    def test_only_the_apps_trusted_approve_for_this_head_before_the_run_counts(self) -> None:
        full = [native("success", full_suite="success")]
        for case, comments in (
                ("no verdict at all", []),
                ("approved after the run was created", [verdict(at=AFTER)]),
                ("approved by someone else", [verdict(author="a-maintainer")]),
                ("approved another head", [verdict(head="b" * 40)]),
                ("no reviewer line", [verdict(reviewer=False)]),
                ("a HOLD after the approve", [verdict(at="2026-10-25T10:00:00Z"),
                                              verdict("HOLD", at=BEFORE)]),
                ("verdict not on the last line",
                 [{"user": {"login": APP}, "created_at": BEFORE,
                   "body": f"reviewer: x\nverdict: APPROVE head:{HEAD}\nlater text\n"}])):
            with self.subTest(case=case):
                self.assertEqual(self._tier(full, comments), "deferred")
        # A re-approve after a HOLD, still before the run: counts.
        self.assertEqual(self._tier(full, [verdict("HOLD", at="2026-10-25T10:00:00Z"),
                                           verdict(at=BEFORE)]), "full")
        # A later verdict on another head does not displace this head's approve.
        self.assertEqual(self._tier(full, [verdict(at="2026-10-25T10:00:00Z"),
                                           verdict("HOLD", head="b" * 40, at=BEFORE)]), "full")

    def test_the_full_lane_must_be_a_pr_number_dispatch_of_this_pull_request(self) -> None:
        full = native("success", full_suite="success")
        self.assertEqual(self._tier([full], [verdict()]), "full", "control")
        for case, macos, parents in (
                ("no Tested step: a plain dispatch", native("success", full_suite="success", tested=None), None),
                ("Tested step did not succeed", native("success", full_suite="success",
                                                       tested_conclusion="skipped"), None),
                ("tested another pull request",
                 native("success", full_suite="success",
                        tested=f"Tested refs/pull/8/merge at {MERGE} onto {BASE}"), None),
                ("malformed Tested name",
                 native("success", full_suite="success", tested=f"Tested refs/pull/7/merge at {MERGE}"), None),
                ("tested commit merges another head", full, f"{BASE} {'b' * 40}\n"),
                ("tested commit with the head first", full, f"{HEAD} {BASE}\n"),
                ("tested commit's parents unreadable", full, "!FAIL")):
            with self.subTest(case=case):
                kwargs = {} if parents is None else {"parents": parents}
                self.assertEqual(self._tier([macos], [verdict()], **kwargs), "deferred")

    def test_the_skip_safe_bootstrap(self) -> None:
        self.assertEqual(self._tier([bootstrap()], [verdict()]), "not-required")
        self.assertEqual(self._tier([bootstrap(tested=None)], [verdict()],
                                    event="pull_request"), "deferred")
        self.assertEqual(self._tier([bootstrap("failure")], [verdict()]), "deferred")
        # A dispatch whose bootstrap job carries no Tested step cannot bind the
        # approval to a merge commit, so it stays deferred.
        self.assertEqual(self._tier([bootstrap(tested=None)], [verdict()]), "deferred")

    def test_annotations_never_enter_the_decision(self) -> None:
        # A contract-lane job whose code under test printed a wider tier.
        forged = native("skipped")
        forged["annotations"] = [{"title": "shipyard-test-tier", "message": '{"tier":"full"}'}]
        self.assertEqual(self._tier([forged], [verdict()]), "deferred")
        self.assertNotIn("annotations", self.steps["Decide the tier"]["run"])

    def test_the_summary_is_bound_to_the_run_and_attempt(self) -> None:
        proc, calls, outputs = self._decide(
            [native("success"), job(name="Linux (x64) [github-hosted]", Build="success")], [verdict()])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        # Exactly #931's field set, in its order, and no label.
        self.assertEqual(list(json.loads(outputs["summary"]).items()), [
            ("schema", "shipyard-test-tier/v1"), ("check", "macos"), ("tier", "fast"),
            ("run_id", 4242), ("run_attempt", 2), ("trigger", "workflow_dispatch"),
            ("actor", APP), ("selector", "pr-fast+pr-guard+affected"),
            ("tested_ref", "refs/pull/7/merge"), ("tested_sha", MERGE), ("merged_base", BASE),
            ("full_lane_workflow", "build.yml"), ("full_suite_runs_in", "merge_group")])
        listing = next(" ".join(c) for c in calls if "/jobs" in " ".join(c))
        self.assertIn("actions/runs/4242/attempts/2/jobs", listing)
        self.assertIn("--paginate", listing)
        verdicts = next(" ".join(c) for c in calls if "/comments" in " ".join(c))
        self.assertIn("--paginate", verdicts)

    def test_trigger_actor_and_selector_report_what_ran(self) -> None:
        # Reported, not trusted: a pull_request run by its author stays deferred.
        proc, _, outputs = self._decide([native("skipped")], [verdict()],
                                        event="pull_request", actor="an-author")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        summary = json.loads(outputs["summary"])
        self.assertEqual((summary["tier"], summary["trigger"], summary["actor"], summary["selector"]),
                         ("deferred", "pull_request", "an-author", "pr-fast+pr-guard"))
        self.assertNotIn("label", summary)
        self.assertEqual((summary["tested_ref"], summary["tested_sha"], summary["merged_base"]),
                         (None, None, None), "a pull_request run tested no bound merge commit")
        for jobs, selector in (([native("success", full_suite="success")], "full"),
                               ([bootstrap()], "none")):
            with self.subTest(selector=selector):
                _, _, outputs = self._decide(jobs, [verdict()])
                self.assertEqual(json.loads(outputs["summary"])["selector"], selector)

    def test_the_run_facts_come_from_the_workflow_run_event(self) -> None:
        env = self.steps["Decide the tier"]["env"]
        self.assertEqual(env["TRIGGER_EVENT"], "${{ github.event.workflow_run.event }}")
        self.assertEqual(env["TRIGGERING_ACTOR"],
                         "${{ github.event.workflow_run.triggering_actor.login }}")
        self.assertEqual(env["RUN_CREATED_AT"], "${{ github.event.workflow_run.created_at }}")

    # --- which pull request the verdict is read from ---------------------

    def test_without_run_prs_only_this_repositorys_pull_request_at_this_head_counts(self) -> None:
        pulls = [{"number": 7, "state": "open", "head": {"sha": HEAD, "repo": {"full_name": REPO}}},
                 {"number": 8, "state": "open", "head": {"sha": HEAD, "repo": {"full_name": "someone/pulp"}}},
                 {"number": 9, "state": "open", "head": {"sha": "b" * 40, "repo": {"full_name": REPO}}},
                 {"number": 10, "state": "closed", "head": {"sha": HEAD, "repo": {"full_name": REPO}}}]
        self.assertEqual(self._tier([native("success")], [verdict()], run_prs="", pulls=pulls), "fast")

    def test_no_or_several_pull_requests_at_the_head_is_deferred(self) -> None:
        self.assertEqual(self._tier([native("success")], [verdict()], run_prs="", pulls=[]), "deferred")
        self.assertEqual(self._tier([native("success")], [verdict()], run_prs="7 8"), "deferred")

    # --- no record when it cannot be sure ---------------------------------

    def test_a_run_from_another_repository_gets_no_record(self) -> None:
        proc, calls, outputs = self._decide([native("success")], [verdict()], head_repo="someone/pulp")
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(calls, [], "no API call at all")
        self.assertNotIn("publish", outputs)

    def test_a_run_with_no_macos_job_gets_no_record(self) -> None:
        self.assertIsNone(self._tier([job(name="Linux (x64) [github-hosted]", Build="success")], [verdict()]))

    def test_an_unplaceable_job_gets_no_record_and_says_so(self) -> None:
        for case, jobs in (("two macos jobs", [native("success"), native("success")]),
                           ("another head", [native("success") | {"head_sha": "b" * 40}]),
                           ("no lane steps", [job(**{"Set up job": "success"})]),
                           ("unreadable listing", "!FAIL")):
            with self.subTest(case=case):
                proc, _, outputs = self._decide(jobs, [verdict()])
                self.assertNotEqual(proc.returncode, 0, proc.stdout)
                self.assertNotIn("publish", outputs)

    def test_unreadable_verdicts_fail_with_no_record(self) -> None:
        proc, _, outputs = self._decide([native("success")], "!FAIL")
        self.assertNotEqual(proc.returncode, 0)
        self.assertNotIn("publish", outputs)


class SecretsAndPublishTest(Harness):
    def test_missing_secrets_fail_visibly(self) -> None:
        for env in ({"APP_ID": "", "HAS_KEY": "false"}, {"APP_ID": "123", "HAS_KEY": "false"},
                    {"APP_ID": "", "HAS_KEY": "true"}):
            with self.subTest(env=env):
                proc, calls, _ = self._bash("Require the tier-record App's secrets", env, {})
                self.assertEqual(proc.returncode, 1)
                self.assertIn("::error title=no tier record::", proc.stdout)
                self.assertEqual(calls, [])
        proc, _, _ = self._bash("Require the tier-record App's secrets",
                                {"APP_ID": "123", "HAS_KEY": "true"}, {})
        self.assertEqual(proc.returncode, 0, "control: present secrets pass")

    def test_the_record_is_posted_with_the_app_token(self) -> None:
        step = self.steps["Publish shipyard-test-tier/macos"]
        self.assertEqual(step["env"]["GH_TOKEN"], "${{ steps.app-token.outputs.token }}")
        summary = '{"schema":"shipyard-test-tier/v1","check":"macos","tier":"fast"}'
        proc, calls, _ = self._bash("Publish shipyard-test-tier/macos",
                                    {"GH_TOKEN": "app", "HEAD_SHA": HEAD, "TIER": "fast",
                                     "SUMMARY": summary}, {"/check-runs": "{}"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(len(calls), 1)
        fields = dict(arg.split("=", 1) for arg in calls[0] if "=" in arg)
        self.assertEqual(fields, {"name": "shipyard-test-tier/macos", "head_sha": HEAD,
                                  "status": "completed", "conclusion": "neutral",
                                  "output[title]": "fast", "output[summary]": summary})


class WorkflowContractTest(Harness):
    def test_it_runs_from_main_on_completed_build_runs_with_no_checkout(self) -> None:
        triggers = self.workflow.get("on", self.workflow.get(True, {}))
        self.assertEqual(triggers, {"workflow_run": {"workflows": ["Build and Test"],
                                                     "types": ["completed"]}})
        self.assertNotIn("actions/checkout", json.dumps(self.workflow))

    def test_its_own_token_is_read_only(self) -> None:
        # The record is written with the App's token; GITHUB_TOKEN only reads.
        self.assertEqual(self.workflow["permissions"],
                         {"actions": "read", "issues": "read", "pull-requests": "read"})
        self.assertNotIn("permissions", self.job)
        self.assertNotIn("github.token", self.steps["Publish shipyard-test-tier/macos"]["env"]["GH_TOKEN"])

    def test_the_app_token_comes_from_the_main_only_environment(self) -> None:
        self.assertEqual(self.job["environment"], "tier-record")
        mint = self.steps["Mint the tier-record App token"]
        self.assertEqual(mint["uses"],
                         "actions/create-github-app-token@64ab729a6294000f5aa4a4bc0b73d1f32bc45510")
        self.assertEqual(mint["with"]["app-id"], "${{ secrets.TIER_RECORD_APP_ID }}")
        self.assertEqual(mint["with"]["private-key"], "${{ secrets.TIER_RECORD_APP_KEY }}")
        self.assertEqual(mint["with"]["permission-checks"], "write")
        # The secrets are checked before anything else runs.
        self.assertEqual(self.job["steps"][0]["name"], "Require the tier-record App's secrets")

    def test_nothing_else_references_the_environment_or_its_secrets(self) -> None:
        # A same-repository pull request's run gets repository secrets but not
        # this environment's, and only if no other job asks for it.
        users = set()
        for path in sorted(WORKFLOWS.glob("*.y*ml")):
            text = path.read_text(encoding="utf-8")
            workflow = yaml.safe_load(text)
            for name, body in (workflow.get("jobs") or {}).items():
                environment = body.get("environment")
                if isinstance(environment, dict):
                    environment = environment.get("name")
                if "tier-record" in str(environment or ""):
                    users.add((path.name, name))
            if "secrets.TIER_RECORD_APP_" in text:
                users.add((path.name, "<secrets>"))
        # The scan's own control: it does see the publisher.
        self.assertEqual(users, {("macos-tier-record.yml", "tier-record"),
                                 ("macos-tier-record.yml", "<secrets>")})

    def test_it_is_same_repository_pull_request_and_dispatch_runs_only(self) -> None:
        condition = " ".join(self.job["if"].split())
        self.assertIn("github.event.workflow_run.head_repository.full_name == github.repository",
                      condition)
        self.assertIn("github.event.workflow_run.event == 'pull_request'", condition)
        self.assertIn("github.event.workflow_run.event == 'workflow_dispatch'", condition)
        self.assertNotIn("merge_group", condition)

    def test_the_steps_it_reads_exist_in_build_yml(self) -> None:
        # Renaming one of them in build.yml would leave every head without a
        # record, so pin them here.
        build = yaml.safe_load(BUILD.read_text(encoding="utf-8"))
        native_steps = {step.get("name"): step for step in build["jobs"]["build"]["steps"]}
        self.assertLessEqual({"Build", "Test (non-Windows)"}, set(native_steps))
        self.assertEqual(native_steps["Build"]["if"], "matrix.lane != 'contract'",
                         "the contract lane is the lane that skips Build")
        alias_steps = {step.get("name") for step in build["jobs"]["macos"]["steps"]}
        self.assertIn(BOOTSTRAP, alias_steps)

    def test_both_macos_jobs_publish_the_tested_step_the_record_reads(self) -> None:
        # The native job and the skip-safe bootstrap job each end a `pr_number`
        # dispatch with a `Tested <ref> at <sha> onto <base>` step. Without it in
        # the bootstrap job, a skip-safe head could never leave `deferred`.
        build = yaml.safe_load(BUILD.read_text(encoding="utf-8"))
        for job_id in ("build", "macos"):
            with self.subTest(job=job_id):
                steps = build["jobs"][job_id]["steps"]
                tested = [step for step in steps if str(step.get("name", "")).startswith("Tested ")]
                self.assertEqual(len(tested), 1, f"jobs.{job_id} needs exactly one Tested step")
                name = tested[0]["name"]
                for part in ("tested_ref", "tested_sha", "merged_base"):
                    self.assertIn(part, name)
        alias = build["jobs"]["macos"]["steps"]
        names = [step.get("name") for step in alias]
        tested = next(step for step in alias if str(step.get("name", "")).startswith("Tested "))
        self.assertGreater(names.index(tested["name"]), names.index(BOOTSTRAP),
                           "the bootstrap fails closed before the Tested step can succeed")
        for output in ("tested_ref", "tested_sha", "merged_base"):
            self.assertIn(f"needs.classify.outputs.{output}", tested["name"])
        condition = tested["if"]
        for required in ("workflow_dispatch", "inputs.pr_number != ''",
                         "needs.classify.result == 'success'",
                         "needs.classify.outputs.tested_sha != ''"):
            self.assertIn(required, condition)
        self.assertNotIn("${{", tested.get("run", ""),
                         "the step must not interpolate outputs into its script")


class ChecksWriterLintTest(unittest.TestCase):
    def test_build_yml_grants_checks_read_explicitly_and_no_job_more(self) -> None:
        # Defence in depth: GitHub-actions records do not count at the guard,
        # but a job running pull request code should still hold no more.
        build = yaml.safe_load(BUILD.read_text(encoding="utf-8"))
        self.assertEqual(build["permissions"].get("checks"), "read")
        for name, body in build["jobs"].items():
            with self.subTest(job=name):
                self.assertIn(effective_checks(build, body), ("read", "none"))

    def test_only_jobs_that_run_no_pull_request_code_may_write_checks(self) -> None:
        # Every workflow, because a pull request runs many of them on the same
        # head. A job may hold `checks: write` only if it is listed here and
        # checks out nothing, or only the protected `main`.
        writers = set()
        for path in sorted(WORKFLOWS.glob("*.y*ml")):
            workflow = yaml.safe_load(path.read_text(encoding="utf-8"))
            for name, body in (workflow.get("jobs") or {}).items():
                if effective_checks(workflow, body) not in ("read", "none"):
                    writers.add((path.name, name))
                    with self.subTest(workflow=path.name, job=name):
                        for step in body.get("steps") or []:
                            if str(step.get("uses", "")).startswith("actions/checkout"):
                                self.assertEqual((step.get("with") or {}).get("ref"), "main",
                                                 "a checks writer must not check out pull request code")
        self.assertEqual(writers, CHECKS_WRITERS,
                         "a new `checks: write` job needs review: it must run no pull request code")

    def test_the_writer_lint_sees_a_planted_writer(self) -> None:
        # Positive control for the lint's permission reading.
        workflow = {"permissions": {"checks": "read"}}
        self.assertEqual(effective_checks(workflow, {"permissions": {"checks": "write"}}), "write")
        self.assertEqual(effective_checks(workflow, {"permissions": "write-all"}), "write")
        self.assertEqual(effective_checks({}, {}), "write", "no block anywhere is the repo default")
        self.assertEqual(effective_checks(workflow, {"permissions": {}}), "none")
        self.assertEqual(effective_checks(workflow, {}), "read")


if __name__ == "__main__":
    unittest.main()
