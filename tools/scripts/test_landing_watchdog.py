#!/usr/bin/env python3
"""Tests for the outcome-based landing watchdog.

The bar these are written to: **a detector that has never been observed
failing is not known to work.** Each test below drives the classifier to a
finding using data shaped like the 2026-09-13 wedge, and each sits beside a
control that must NOT produce a finding — because a classifier that flags
everything satisfies the first half and tells nobody anything.

The fixture is a capture of the real incident: an App-opened pull request with
no `pull_request` run and two workflow runs queued with zero jobs holding the
concurrency group, plus a healthy pull request as the control.

Run: python3 tools/scripts/test_landing_watchdog.py
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import landing_watchdog as lw  # noqa: E402

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures",
                       "landing_watchdog_wedge.json")
# The config the scheduled workflow replays the fixture against.
CONFIG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                      ".shipyard", "config.toml")


def iso(when: datetime) -> str:
    return when.strftime("%Y-%m-%dT%H:%M:%SZ")


def materialize(now: datetime, old_mins: int = 180) -> str:
    """Rewrite the fixture's relative markers against a fixed clock."""
    with open(FIXTURE, "r", encoding="utf-8") as handle:
        raw = handle.read()
    return raw.replace("REPLACE_OLD", iso(now - timedelta(minutes=old_mins)))


class ReplayTheIncident(unittest.TestCase):
    def setUp(self):
        self.now = datetime(2026, 9, 13, 5, 36, tzinfo=timezone.utc)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "fixture.json")
        with open(self.path, "w", encoding="utf-8") as handle:
            handle.write(materialize(self.now))
        # The repository's own required set, exactly as the workflow's
        # negative-control step reads it: the fixture has to keep producing its
        # findings, and nothing for the healthy PR, against the live contract.
        self.contexts = lw.required_contexts(CONFIG)
        self.assertIn("Vellum trusted freeze", self.contexts, "config was not read")

    def findings(self):
        return lw.replay(self.path, self.now, self.contexts)

    def test_the_app_opened_pr_with_no_macos_check_is_reported_absent(self):
        found = [f for f in self.findings() if f["pr"] == 8277 and f["state"] == "absent"]
        self.assertEqual(len(found), 1, self.findings())
        self.assertEqual(found[0]["context"], "macos")
        self.assertTrue(
            found[0]["author_is_bot"],
            "an App-opened PR must be flagged as such: `pull_request` workflows "
            "may never have fired for it",
        )
        self.assertIn("opened by an App", found[0]["detail"])

    def test_the_zero_job_runs_are_reported_as_the_concurrency_signature(self):
        found = [f for f in self.findings() if f["state"] == "zero_jobs"]
        self.assertEqual(len(found), 2, found)
        self.assertIn("not capacity", found[0]["detail"])
        self.assertIn(34738145833, {f["run_id"] for f in found})

    def test_a_queued_macos_check_past_threshold_is_unassigned(self):
        found = [f for f in self.findings() if f["pr"] == 8280]
        self.assertEqual(len(found), 1, found)
        self.assertEqual(found[0]["state"], "unassigned")

    def test_the_healthy_pr_produces_no_finding(self):
        # THE CONTROL. Without this, a classifier that returned a finding for
        # every pull request would satisfy all three tests above.
        found = [f for f in self.findings() if f["pr"] == 8281]
        self.assertEqual(found, [], "a fully green PR must produce no finding")

    def test_replay_exits_non_zero_on_findings_and_zero_without(self):
        self.assertGreater(len(self.findings()), 0)
        empty = os.path.join(self.tmp.name, "empty.json")
        with open(empty, "w", encoding="utf-8") as handle:
            json.dump({"pull_requests": [], "required_contexts": self.contexts}, handle)
        self.assertEqual(lw.replay(empty, self.now, self.contexts), [])


class Thresholds(unittest.TestCase):
    """A young pull request must not be flagged. The thresholds are what keep
    this from becoming the alarm nobody reads."""

    def setUp(self):
        self.now = datetime(2026, 9, 13, 5, 36, tzinfo=timezone.utc)
        self.contexts = ["macos"]

    def _pr(self, age_mins: int):
        return {
            "number": 1,
            "updated_at": iso(self.now - timedelta(minutes=age_mins)),
            "head": {"sha": "deadbeef" * 5},
            "user": {"type": "User"},
        }

    def test_a_pr_pushed_five_minutes_ago_with_no_checks_is_not_a_finding(self):
        self.assertEqual(lw.classify_pr(self._pr(5), [], [], self.contexts, self.now), [])

    def test_the_same_pr_an_hour_later_is_a_finding(self):
        # Same instrument, same target, one input changed: this pair is what
        # proves the threshold is doing the work rather than the classifier
        # being permanently blind or permanently loud.
        found = lw.classify_pr(self._pr(90), [], [], self.contexts, self.now)
        self.assertEqual(len(found), 1, found)
        self.assertEqual(found[0]["state"], "absent")

    def test_a_completed_required_context_is_never_a_finding(self):
        checks = [{"name": "macos", "status": "completed", "conclusion": "failure"}]
        # A red check is a code problem, not a landing wedge. This watchdog
        # deliberately says nothing about it: it answers "can it land", and a
        # failing test is a different question with a different owner.
        self.assertEqual(
            lw.classify_pr(self._pr(600), checks, [], self.contexts, self.now), []
        )

    def test_a_run_with_jobs_is_not_the_concurrency_signature(self):
        runs = [
            {
                "id": 7,
                "status": "queued",
                "created_at": iso(self.now - timedelta(minutes=120)),
                "jobs_total_count": 4,
            }
        ]
        found = lw.classify_pr(self._pr(5), [], runs, self.contexts, self.now)
        self.assertEqual(found, [], "queued-with-jobs is capacity, not a concurrency hold")


class WorkflowNegativeControl(unittest.TestCase):
    """The scheduled workflow's first step, run the way the workflow runs it:
    the CLI replays the unmaterialized fixture against the repository config.
    It must exit 1 with all three finding states and leave the healthy PR alone,
    or the workflow fails every tick before it scans anything."""

    def test_cli_replay_fires_on_the_wedge_and_spares_the_healthy_pr(self):
        import contextlib
        import io

        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = lw.main(["--config", CONFIG, "--replay", FIXTURE, "--json"])
        self.assertEqual(code, 1, out.getvalue())
        findings = json.loads(out.getvalue())["findings"]
        self.assertEqual({f["state"] for f in findings}, {"absent", "unassigned", "zero_jobs"})
        self.assertFalse([f for f in findings if f["pr"] == 8281], findings)


class CommitStatusContexts(unittest.TestCase):
    """A required context can be a commit status rather than a check run.

    `Vellum trusted freeze` is published on a pull request's head as a status by
    a `pull_request_target` run; read as check runs alone it is absent on every
    pull request."""

    def setUp(self):
        self.now = datetime(2026, 9, 13, 5, 36, tzinfo=timezone.utc)
        self.contexts = ["macos", "Vellum trusted freeze"]
        self.pr = {
            "number": 2,
            "updated_at": iso(self.now - timedelta(minutes=600)),
            "head": {"sha": "feedface" * 5},
            "user": {"type": "User"},
        }
        self.checks = [{"name": "macos", "status": "completed", "conclusion": "success"}]

    def _status(self, state: str, age_mins: int = 600) -> list[dict]:
        return [{
            "context": "Vellum trusted freeze",
            "state": state,
            "created_at": iso(self.now - timedelta(minutes=age_mins)),
        }]

    def test_a_status_satisfies_a_required_context(self):
        found = lw.classify_pr(self.pr, self.checks, [], self.contexts, self.now,
                               self._status("success"))
        self.assertEqual(found, [], found)

    def test_a_failed_status_is_present_not_absent(self):
        found = lw.classify_pr(self.pr, self.checks, [], self.contexts, self.now,
                               self._status("failure"))
        self.assertEqual(found, [], "a red status is a code problem, not a wedge")

    def test_without_the_status_the_context_is_absent(self):
        # The control: same PR, same checks, no status. The finding must appear,
        # or the test above passes because nothing is ever flagged.
        found = lw.classify_pr(self.pr, self.checks, [], self.contexts, self.now)
        self.assertEqual([(f["context"], f["state"]) for f in found],
                         [("Vellum trusted freeze", "absent")])

    def test_a_long_pending_status_is_unassigned(self):
        found = lw.classify_pr(self.pr, self.checks, [], self.contexts, self.now,
                               self._status("pending", age_mins=90))
        self.assertEqual([(f["context"], f["state"]) for f in found],
                         [("Vellum trusted freeze", "unassigned")])

    def test_a_fresh_pending_status_is_not_a_finding(self):
        found = lw.classify_pr(self.pr, self.checks, [], self.contexts, self.now,
                               self._status("pending", age_mins=5))
        self.assertEqual(found, [])


class InstrumentSelfReport(unittest.TestCase):
    """The watchdog reporting its own failure to run."""

    def test_an_empty_required_context_list_is_refused_not_reported_clean(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = os.path.join(tmp, "config.toml")
            with open(config, "w", encoding="utf-8") as handle:
                handle.write("[governance]\n")
            os.environ["GITHUB_TOKEN"] = "x"
            code = lw.main(["--repo", "o/r", "--config", config, "--no-issue"])
        self.assertEqual(
            code, 3,
            "with no required contexts every PR looks clean; the run must fail "
            "rather than report health it did not measure",
        )

    def test_required_contexts_are_read_from_the_shipyard_config(self):
        # The positive control for the reader above: it must return non-empty
        # for a config that HAS contexts, or the check above passes for the
        # wrong reason.
        with tempfile.TemporaryDirectory() as tmp:
            config = os.path.join(tmp, "config.toml")
            with open(config, "w", encoding="utf-8") as handle:
                handle.write('[governance]\nrequired_status_checks = ["macos", "x"]\n')
            contexts = lw.required_contexts(config)
        self.assertEqual(contexts, ["macos", "x"])

    def test_the_issue_body_names_the_pr_the_context_and_forbids_re_dispatch(self):
        now = datetime(2026, 9, 13, 5, 36, tzinfo=timezone.utc)
        findings = [
            {
                "pr": 8277,
                "context": "macos",
                "state": "absent",
                "age_mins": 180,
                "detail": "no check run",
            }
        ]
        body = lw.render_issue_body(findings, 3, ["macos"], 7, now)
        self.assertIn("#8277", body)
        self.assertIn("`macos`", body)
        self.assertIn("shipyard landability", body)
        self.assertIn("Do not blind re-dispatch", body)
        self.assertIn("[default] #4", body)


if __name__ == "__main__":
    unittest.main()
