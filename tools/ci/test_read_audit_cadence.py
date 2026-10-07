#!/usr/bin/env python3
"""Tests for tools/ci/read_audit_cadence.py.

    python3 tools/ci/test_read_audit_cadence.py
"""
from __future__ import annotations

import datetime as dt
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import read_audit_cadence as rac  # noqa: E402

START = dt.date(2026, 10, 3)


def run(i, created, conclusion="success", event="schedule", branch="main", status="completed",
        completed=None, sha="aaaa"):
    """A runs-listing row. A run completes 20 minutes after it is created unless told otherwise."""
    if completed is None:
        stamp = dt.datetime.fromisoformat(created.replace("Z", "+00:00")) + dt.timedelta(minutes=20)
        completed = stamp.astimezone(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return {"id": i, "created_at": created, "updated_at": completed, "conclusion": conclusion,
            "event": event, "head_branch": branch, "status": status, "head_sha": sha}


def now(stamp):
    return dt.datetime.fromisoformat(stamp).replace(tzinfo=dt.timezone.utc)


def counted(doc):
    return [(d["date"], d["run_id"]) for d in doc["streak"]["days"]]


class LedgerTests(unittest.TestCase):
    def setUp(self):
        # These cases are about nights and order, not the flip: every success
        # counts by its conclusion unless a test hands in verdicts.
        self.since = rac.FAIL_ON_FINDINGS_SINCE
        rac.FAIL_ON_FINDINGS_SINCE = "2000-01-01T00:00:00Z"

    def tearDown(self):
        rac.FAIL_ON_FINDINGS_SINCE = self.since

    def test_a_dispatch_outside_the_window_never_counts(self):
        # The 04:07Z dispatch shape: clean, on main, completed before the window opened.
        doc = rac.ledger([run(1, "2026-10-03T04:07:00Z", event="workflow_dispatch")],
                         START, now("2026-10-03T12:00:00"))
        self.assertEqual(doc["streak"]["count"], 0)
        self.assertEqual(doc["missing"], ["2026-10-03"])
        self.assertEqual(doc["days"][0]["ignored"][0]["reason"], "completed outside the nightly window")
        # Same for a dispatch after the window and a scheduled run that finished late.
        doc = rac.ledger([run(2, "2026-10-03T10:30:00Z", event="workflow_dispatch"),
                          run(3, "2026-10-03T09:50:00Z")], START, now("2026-10-03T12:00:00"))
        self.assertEqual((doc["streak"]["count"], doc["missing"]), (0, ["2026-10-03"]))

    def test_the_scheduled_run_in_the_window_counts(self):
        doc = rac.ledger([run(1, "2026-10-03T07:41:00Z"), run(2, "2026-10-04T08:10:00Z")],
                         START, now("2026-10-04T12:00:00"))
        self.assertEqual(counted(doc), [("2026-10-03", 1), ("2026-10-04", 2)])
        self.assertEqual(doc["missing"], [])

    def test_a_same_sha_dispatch_replacing_a_cancelled_schedule_counts_once(self):
        runs = [run(1, "2026-10-03T07:41:00Z", "cancelled", sha="bbbb", completed="2026-10-03T07:45:00Z"),
                run(2, "2026-10-03T07:44:00Z", event="workflow_dispatch", sha="bbbb"),
                run(3, "2026-10-03T08:30:00Z", event="workflow_dispatch", sha="bbbb")]
        doc = rac.ledger(runs, START, now("2026-10-03T12:00:00"))
        self.assertEqual(counted(doc), [("2026-10-03", 2)])
        self.assertEqual([r["id"] for r in doc["days"][0]["ignored"]], [3])
        # A different sha is not a replacement, and neither is a dispatch with no cancelled schedule.
        runs[1]["head_sha"] = runs[2]["head_sha"] = "cccc"
        doc = rac.ledger(runs, START, now("2026-10-03T12:00:00"))
        self.assertEqual((doc["streak"]["count"], doc["missing"]), (0, ["2026-10-03"]))
        doc = rac.ledger([run(4, "2026-10-03T08:00:00Z", event="workflow_dispatch")],
                         START, now("2026-10-03T12:00:00"))
        self.assertEqual((doc["streak"]["count"], doc["missing"]), (0, ["2026-10-03"]))

    def test_two_runs_in_one_window_count_once(self):
        doc = rac.ledger([run(1, "2026-10-03T07:41:00Z"), run(2, "2026-10-03T08:41:00Z")],
                         START, now("2026-10-03T12:00:00"))
        self.assertEqual(counted(doc), [("2026-10-03", 1)])
        self.assertEqual(doc["days"][0]["ignored"][0]["reason"], "a run already counts for this night")
        # The scheduled run outranks a replacing dispatch, whichever finished first.
        runs = [run(3, "2026-10-04T07:00:00Z", "cancelled", sha="dddd"),
                run(4, "2026-10-04T07:05:00Z", event="workflow_dispatch", sha="dddd"),
                run(5, "2026-10-04T08:00:00Z")]
        doc = rac.ledger(runs, dt.date(2026, 10, 4), now("2026-10-04T12:00:00"))
        self.assertEqual(counted(doc), [("2026-10-04", 5)])

    def test_a_night_with_no_counted_run_is_a_gap_and_resets(self):
        doc = rac.ledger([run(1, "2026-10-03T07:41:00Z"), run(2, "2026-10-05T07:41:00Z")],
                         START, now("2026-10-05T12:00:00"))
        self.assertEqual(doc["missing"], ["2026-10-04"])
        self.assertEqual(counted(doc), [("2026-10-05", 2)])
        self.assertEqual([r["verdict"] for r in doc["days"]], ["clean", "gap", "clean"])

    def test_a_non_clean_night_resets(self):
        doc = rac.ledger([run(1, "2026-10-03T07:41:00Z"), run(2, "2026-10-04T07:41:00Z", "failure"),
                          run(3, "2026-10-05T07:41:00Z")], START, now("2026-10-05T12:00:00"))
        self.assertEqual(counted(doc), [("2026-10-05", 3)])
        self.assertEqual([r["verdict"] for r in doc["days"]], ["clean", "not_clean", "clean"])

    def test_tonight_is_pending_until_the_window_closes(self):
        doc = rac.ledger([run(1, "2026-10-03T07:41:00Z")], START, now("2026-10-04T09:00:00"))
        self.assertEqual([r["verdict"] for r in doc["days"]], ["clean", "pending"])
        self.assertEqual((doc["streak"]["count"], doc["missing"]), (1, []))
        doc = rac.ledger([run(1, "2026-10-03T07:41:00Z")], START, now("2026-10-04T10:00:01"))
        self.assertEqual((doc["streak"]["count"], doc["missing"]), (0, ["2026-10-04"]))

    def test_only_recent_missing_nights_hold_the_issue_open(self):
        doc = rac.ledger([run(1, "2026-10-03T07:41:00Z"), run(2, "2026-10-20T07:41:00Z")],
                         START, now("2026-10-21T09:00:00"), flag_days=7)
        self.assertEqual(len(doc["missing"]), 16)          # 10-04 .. 10-19
        self.assertEqual(doc["missing_recent"], ["2026-10-14", "2026-10-15", "2026-10-16", "2026-10-17",
                                                 "2026-10-18", "2026-10-19"])

    def test_the_report_verdict_outranks_the_conclusion(self):
        # Before the nightly failed on findings, a success could carry them.
        rac.FAIL_ON_FINDINGS_SINCE = self.since
        runs = [run(1, "2026-10-03T07:41:00Z"), run(2, "2026-10-04T07:41:00Z")]
        doc = rac.ledger(runs, START, now("2026-10-04T12:00:00"), verdicts={"1": "findings", "2": "clean"})
        self.assertEqual(counted(doc), [("2026-10-04", 2)])
        self.assertEqual(doc["days"][0]["run"]["verdict"], "not_clean")
        # Without a report, an early success is not clean; a late one is.
        doc = rac.ledger(runs, START, now("2026-10-04T12:00:00"))
        self.assertEqual(counted(doc), [("2026-10-04", 2)])
        self.assertEqual(doc["days"][0]["verdict"], "not_clean")
        doc = rac.ledger([run(4, "2026-10-04T07:41:00Z")], START, now("2026-10-04T12:00:00"),
                         verdicts={"4": "incomplete"})
        self.assertEqual(doc["streak"]["count"], 0)

    def test_runs_that_say_nothing_are_ignored(self):
        doc = rac.ledger([run(1, "2026-10-03T07:41:00Z", "cancelled"), run(2, "2026-10-03T07:50:00Z", "skipped"),
                          run(3, "2026-10-03T08:00:00Z", branch="feature"),
                          run(4, "2026-10-03T08:10:00Z", status="in_progress"),
                          run(5, "2026-10-02T07:41:00Z")], START, now("2026-10-04T09:00:00"))
        self.assertEqual((doc["streak"]["count"], doc["missing"]), (0, ["2026-10-03"]))

    def test_the_window_is_utc(self):
        # 23:50-08:00 is 07:50Z the next day.
        doc = rac.ledger([run(1, "2026-10-03T23:30:00-08:00", completed="2026-10-03T23:50:00-08:00")],
                         START, now("2026-10-04T12:00:00"))
        self.assertEqual(counted(doc), [("2026-10-04", 1)])
        self.assertEqual(doc["missing"], ["2026-10-03"])


class CliTests(unittest.TestCase):
    def test_the_cli_writes_the_ledger_and_names_missing_days(self):
        with tempfile.TemporaryDirectory() as tmp:
            runs = Path(tmp) / "runs.json"
            runs.write_text(json.dumps({"workflow_runs": [run(1, "2026-10-03T07:41:00Z"), run(2, "2026-10-05T07:41:00Z")]}))
            verdicts = Path(tmp) / "verdicts.json"
            verdicts.write_text(json.dumps({"1": "clean", "2": "clean"}))
            proc = subprocess.run([sys.executable, str(HERE / "read_audit_cadence.py"), "--runs", str(runs),
                                   "--verdicts", str(verdicts),
                                   "--start", "2026-10-03", "--now", "2026-10-05T12:00:00+00:00",
                                   "--out", f"{tmp}/l.json", "--summary", f"{tmp}/l.md"],
                                  capture_output=True, text=True, timeout=60)
            doc = json.loads((Path(tmp) / "l.json").read_text())
            md = (Path(tmp) / "l.md").read_text()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual((doc["streak"]["count"], doc["missing"]), (1, ["2026-10-04"]))
        self.assertIn("**2026-10-04**", md)
        self.assertIn("streak=1 missing=2026-10-04", proc.stdout)


class WorkflowTests(unittest.TestCase):
    def test_the_check_is_scheduled_hosted_and_backstopped(self):
        root = HERE.parents[1]
        text = (root / ".github/workflows/read-audit-cadence-check.yml").read_text()
        self.assertIn("tools/ci/read_audit_cadence.py", text)
        self.assertIn("read-audit-nightly.yml/runs", text)
        self.assertIn("schedule:", text)
        manifest = json.loads((root / ".github/schedule-backstop.json").read_text())
        self.assertIn("read-audit-cadence-check.yml", {r["file"] for r in manifest["workflows"]})


if __name__ == "__main__":
    unittest.main()
