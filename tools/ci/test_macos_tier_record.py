#!/usr/bin/env python3
"""The macos tier record: decided from the job's steps, bound to its run.

Runs the workflow step's own bash against a fake `gh` that answers the jobs
listing with raw API JSON, so the real jq that reads the steps is what runs.
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
WORKFLOW = ROOT / ".github/workflows/macos-tier-record.yml"
BUILD = ROOT / ".github/workflows/build.yml"
REPO = "Generous-Corp/pulp"
HEAD = "a" * 40
BOOTSTRAP = "macOS PR/dispatch bootstrap (required when native leg is absent)"

# Answers `gh api <path>` from a {path-substring: body} fixture, in order, and
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


def job(name="macos", head=HEAD, **steps: str) -> dict:
    return {"name": name, "head_sha": head,
            "steps": [{"name": step, "conclusion": conclusion} for step, conclusion in steps.items()]}


def native(build: str, full_suite: str = "skipped", **extra) -> dict:
    return job(**{"Build": build, "Test fast deterministic tier (pull request head)": "success",
                  "Test (non-Windows)": full_suite}, **extra)


class MacosTierRecordTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
        cls.job = cls.workflow["jobs"]["tier-record"]
        cls.step = cls.job["steps"][0]

    def _run(self, jobs: list[dict] | str, head_repo: str = REPO,
             pages: int = 1) -> tuple[subprocess.CompletedProcess, list[list[str]]]:
        if isinstance(jobs, str):
            listing = jobs
        else:
            # --paginate prints one JSON document per page.
            per_page = max(1, -(-len(jobs) // pages))
            listing = "".join(json.dumps({"total_count": len(jobs),
                                          "jobs": jobs[i:i + per_page]}) + "\n"
                              for i in range(0, max(len(jobs), 1), per_page))
        fixture = {"/check-runs": "{}", "/attempts/2/jobs": listing}
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = Path(tmp) / "bin"
            bin_dir.mkdir()
            gh = bin_dir / "gh"
            gh.write_text(FAKE_GH, encoding="utf-8")
            gh.chmod(0o755)
            (Path(tmp) / "fixture.json").write_text(json.dumps(fixture), encoding="utf-8")
            log = Path(tmp) / "log"
            log.write_text("", encoding="utf-8")
            env = {**os.environ, "PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}",
                   "FAKE_GH_FIXTURE": str(Path(tmp) / "fixture.json"), "FAKE_GH_LOG": str(log),
                   "GITHUB_REPOSITORY": REPO, "RUN_ID": "4242", "RUN_ATTEMPT": "2",
                   "HEAD_SHA": HEAD, "HEAD_REPO": head_repo, "GH_TOKEN": "x"}
            proc = subprocess.run(["bash", "-c", self.step["run"]], env=env, capture_output=True,
                                  text=True, encoding="utf-8", check=False)
            return proc, [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]

    @staticmethod
    def _record(calls: list[list[str]]) -> dict | None:
        posts = [call for call in calls if any(arg.endswith("/check-runs") for arg in call)]
        if not posts:
            return None
        assert len(posts) == 1, posts
        fields = dict(arg.split("=", 1) for arg in posts[0] if "=" in arg)
        assert fields["name"] == "shipyard-test-tier/macos", fields
        assert fields["head_sha"] == HEAD, fields
        assert fields["status"] == "completed" and fields["conclusion"] == "neutral", fields
        summary = json.loads(fields["output[summary]"])
        assert fields["output[title]"] == summary["tier"], fields
        return summary

    def _tier(self, jobs) -> str | None:
        proc, calls = self._run(jobs)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        record = self._record(calls)
        return record and record["tier"]

    # --- the workflow's shape -------------------------------------------------

    def test_it_runs_from_main_on_completed_build_runs_with_no_checkout(self) -> None:
        triggers = self.workflow.get("on", self.workflow.get(True, {}))
        self.assertEqual(triggers, {"workflow_run": {"workflows": ["Build and Test"],
                                                     "types": ["completed"]}})
        self.assertEqual(self.workflow["permissions"], {"actions": "read", "checks": "write"})
        self.assertNotIn("actions/checkout", json.dumps(self.workflow))
        self.assertNotIn("permissions", self.job)

    def test_no_job_that_runs_pull_request_code_can_write_checks(self) -> None:
        build = yaml.safe_load(BUILD.read_text(encoding="utf-8"))
        self.assertNotIn("checks", build.get("permissions", {}))
        for name, body in build["jobs"].items():
            with self.subTest(job=name):
                self.assertNotEqual((body.get("permissions") or {}).get("checks"), "write")

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

    def test_it_is_same_repository_pull_request_and_dispatch_runs_only(self) -> None:
        condition = " ".join(self.job["if"].split())
        self.assertIn("github.event.workflow_run.head_repository.full_name == github.repository",
                      condition)
        self.assertIn("github.event.workflow_run.event == 'pull_request'", condition)
        self.assertIn("github.event.workflow_run.event == 'workflow_dispatch'", condition)
        self.assertNotIn("merge_group", condition)

    # --- the tier, from the steps the runner recorded --------------------------

    def test_a_skipped_build_is_the_contract_lane(self) -> None:
        self.assertEqual(self._tier([native("skipped")]), "deferred")

    def test_a_build_that_ran_is_the_full_lane(self) -> None:
        self.assertEqual(self._tier([native("success")]), "fast")
        # A failed build ran too; the check's own red is what blocks it.
        self.assertEqual(self._tier([native("failure")]), "fast")

    def test_a_full_suite_that_ran_is_full(self) -> None:
        self.assertEqual(self._tier([native("success", full_suite="success")]), "full")

    def test_the_bootstraps_green_is_not_required(self) -> None:
        self.assertEqual(self._tier([job(**{BOOTSTRAP: "success"})]), "not-required")
        self.assertEqual(self._tier([job(**{BOOTSTRAP: "failure"})]), "deferred")

    def test_annotations_never_enter_the_decision(self) -> None:
        # A contract-lane job whose code under test printed a wider tier.
        forged = native("skipped")
        forged["annotations"] = [{"title": "shipyard-test-tier", "message": '{"tier":"full"}'}]
        self.assertEqual(self._tier([forged]), "deferred")
        self.assertNotIn("annotations", self.step["run"])

    def test_the_record_is_bound_to_the_run_and_attempt(self) -> None:
        proc, calls = self._run([native("success"),
                                 job(name="Linux (x64) [github-hosted]", Build="success")], pages=2)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(self._record(calls), {
            "schema": "shipyard-test-tier/v1", "check": "macos", "tier": "fast",
            "run_id": 4242, "run_attempt": 2, "label": "macos-ci",
            "full_suite_runs_in": "merge_group"})
        listing = next(" ".join(c) for c in calls if "/jobs" in " ".join(c))
        self.assertIn("actions/runs/4242/attempts/2/jobs", listing)
        self.assertIn("--paginate", listing)

    # --- no record when it cannot be sure -------------------------------------

    def test_a_run_from_another_repository_gets_no_record(self) -> None:
        proc, calls = self._run([native("success")], head_repo="someone/pulp")
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(calls, [], "no API call at all")

    def test_a_run_with_no_macos_job_gets_no_record(self) -> None:
        self.assertIsNone(self._tier([job(name="Linux (x64) [github-hosted]", Build="success")]))

    def test_an_unplaceable_job_gets_no_record_and_says_so(self) -> None:
        for label, jobs in (("two macos jobs", [native("success"), native("success")]),
                            ("another head", [native("success") | {"head_sha": "b" * 40}]),
                            ("no lane steps", [job(**{"Set up job": "success"})]),
                            ("unreadable listing", "!FAIL")):
            with self.subTest(case=label):
                proc, calls = self._run(jobs)
                self.assertNotEqual(proc.returncode, 0, proc.stdout)
                self.assertIsNone(self._record(calls))


if __name__ == "__main__":
    unittest.main()
