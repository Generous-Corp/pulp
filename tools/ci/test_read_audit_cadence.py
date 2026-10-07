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


def run(i, created, conclusion="success", event="schedule", branch="main", status="completed", sha="aaaa"):
    return {"id": i, "created_at": created, "conclusion": conclusion, "event": event,
            "head_branch": branch, "status": status, "head_sha": sha}


def now(stamp):
    return dt.datetime.fromisoformat(stamp).replace(tzinfo=dt.timezone.utc)


def counted(doc):
    return [(d["date"], d["run_id"]) for d in doc["streak"]["days"]]


class LedgerTests(unittest.TestCase):
    def setUp(self):
        # These cases are about days and order, not the flip: every success
        # counts by its conclusion unless a test hands in verdicts.
        self.since = rac.FAIL_ON_FINDINGS_SINCE
        rac.FAIL_ON_FINDINGS_SINCE = "2000-01-01T00:00:00Z"

    def tearDown(self):
        rac.FAIL_ON_FINDINGS_SINCE = self.since

    def test_the_scheduled_run_counts_for_the_day_it_was_created(self):
        # GitHub fires the 07:41Z cron hours late; the day is the run's own date.
        doc = rac.ledger([run(1, "2026-10-03T16:20:53Z"), run(2, "2026-10-04T13:25:01Z")],
                         START, now("2026-10-04T23:00:00"))
        self.assertEqual(counted(doc), [("2026-10-03", 1), ("2026-10-04", 2)])
        self.assertEqual(doc["missing"], [])

    def test_an_out_of_band_dispatch_on_a_different_sha_does_not_count(self):
        runs = [run(1, "2026-10-03T14:00:00Z", "cancelled", sha="bbbb"),
                run(2, "2026-10-03T04:07:13Z", event="workflow_dispatch", sha="cccc"),
                run(3, "2026-10-03T15:00:00Z", event="workflow_dispatch", sha="cccc")]
        doc = rac.ledger(runs, START, now("2026-10-04T12:00:00"))
        self.assertEqual((doc["streak"]["count"], doc["missing"]), (0, ["2026-10-03"]))
        self.assertEqual({r["id"] for r in doc["days"][0]["ignored"]}, {2, 3})
        # A dispatch on a day whose schedule ran is not a replacement either,
        # and a day with only dispatches is a gap.
        doc = rac.ledger([run(4, "2026-10-03T04:07:13Z", event="workflow_dispatch")],
                         START, now("2026-10-04T12:00:00"))
        self.assertEqual((doc["streak"]["count"], doc["missing"]), (0, ["2026-10-03"]))

    def test_a_same_sha_dispatch_replacing_a_cancelled_schedule_counts_once(self):
        runs = [run(1, "2026-10-03T14:25:00Z", "cancelled", sha="bbbb"),
                run(2, "2026-10-03T14:26:00Z", event="workflow_dispatch", sha="bbbb"),
                run(3, "2026-10-03T18:00:00Z", event="workflow_dispatch", sha="bbbb")]
        doc = rac.ledger(runs, START, now("2026-10-04T12:00:00"))
        self.assertEqual(counted(doc), [("2026-10-03", 2)])
        self.assertEqual(doc["days"][0]["run"]["reason"], "replaces cancelled scheduled run 1")
        self.assertEqual([r["id"] for r in doc["days"][0]["ignored"]], [3])
        # A dispatch from before the cancelled schedule did not replace it.
        doc = rac.ledger([run(1, "2026-10-03T14:25:00Z", "cancelled", sha="bbbb"),
                          run(4, "2026-10-03T09:00:00Z", event="workflow_dispatch", sha="bbbb")],
                         START, now("2026-10-04T12:00:00"))
        self.assertEqual((doc["streak"]["count"], doc["missing"]), (0, ["2026-10-03"]))

    def test_a_schedule_and_a_dispatch_on_one_day_count_once(self):
        doc = rac.ledger([run(1, "2026-10-03T08:56:00Z", "failure", event="workflow_dispatch"),
                          run(2, "2026-10-03T14:25:00Z"),
                          run(3, "2026-10-03T20:00:00Z", event="workflow_dispatch")],
                         START, now("2026-10-04T12:00:00"))
        self.assertEqual(counted(doc), [("2026-10-03", 2)])
        self.assertEqual([r["verdict"] for r in doc["days"]], ["clean", "pending"])
        self.assertEqual({r["id"] for r in doc["days"][0]["ignored"]}, {1, 3})
        # Two scheduled runs on one day: the first created counts.
        doc = rac.ledger([run(4, "2026-10-03T14:25:00Z"), run(5, "2026-10-03T20:25:00Z", "failure")],
                         START, now("2026-10-04T12:00:00"))
        self.assertEqual(counted(doc), [("2026-10-03", 4)])

    def test_a_day_with_neither_is_a_gap_and_resets(self):
        doc = rac.ledger([run(1, "2026-10-03T14:00:00Z"), run(2, "2026-10-05T14:00:00Z")],
                         START, now("2026-10-05T23:00:00"))
        self.assertEqual(doc["missing"], ["2026-10-04"])
        self.assertEqual(counted(doc), [("2026-10-05", 2)])
        self.assertEqual([r["verdict"] for r in doc["days"]], ["clean", "gap", "clean"])

    def test_a_non_clean_day_resets(self):
        doc = rac.ledger([run(1, "2026-10-03T14:00:00Z"), run(2, "2026-10-04T14:00:00Z", "failure"),
                          run(3, "2026-10-05T14:00:00Z")], START, now("2026-10-05T23:00:00"))
        self.assertEqual(counted(doc), [("2026-10-05", 3)])
        self.assertEqual([r["verdict"] for r in doc["days"]], ["clean", "not_clean", "clean"])

    def test_today_and_a_running_day_are_pending(self):
        doc = rac.ledger([run(1, "2026-10-03T14:00:00Z")], START, now("2026-10-04T23:00:00"))
        self.assertEqual(([r["verdict"] for r in doc["days"]], doc["missing"]), (["clean", "pending"], []))
        doc = rac.ledger([run(1, "2026-10-03T14:00:00Z"), run(2, "2026-10-04T23:50:00Z", None,
                                                               status="in_progress")],
                         START, now("2026-10-05T01:00:00"))
        self.assertEqual([r["verdict"] for r in doc["days"]], ["clean", "pending", "pending"])
        self.assertEqual((doc["streak"]["count"], doc["missing"]), (1, []))

    def test_only_recent_missing_days_hold_the_issue_open(self):
        doc = rac.ledger([run(1, "2026-10-03T14:00:00Z"), run(2, "2026-10-20T14:00:00Z")],
                         START, now("2026-10-21T09:00:00"), flag_days=7)
        self.assertEqual(len(doc["missing"]), 16)          # 10-04 .. 10-19
        self.assertEqual(doc["missing_recent"], ["2026-10-14", "2026-10-15", "2026-10-16", "2026-10-17",
                                                 "2026-10-18", "2026-10-19"])

    def test_the_report_verdict_outranks_the_conclusion(self):
        # Before the nightly failed on findings, a success could carry them.
        rac.FAIL_ON_FINDINGS_SINCE = self.since
        runs = [run(1, "2026-10-03T14:00:00Z"), run(2, "2026-10-04T14:00:00Z")]
        doc = rac.ledger(runs, START, now("2026-10-04T23:00:00"), verdicts={"1": "findings", "2": "clean"})
        self.assertEqual(counted(doc), [("2026-10-04", 2)])
        self.assertEqual(doc["days"][0]["run"]["verdict"], "not_clean")
        # Without a report, an early success is not clean; a late one is.
        doc = rac.ledger(runs, START, now("2026-10-04T23:00:00"))
        self.assertEqual(counted(doc), [("2026-10-04", 2)])
        self.assertEqual(doc["days"][0]["verdict"], "not_clean")
        doc = rac.ledger([run(4, "2026-10-04T14:00:00Z")], START, now("2026-10-04T23:00:00"),
                         verdicts={"4": "incomplete"})
        self.assertEqual(doc["streak"]["count"], 0)

    def test_runs_that_say_nothing_are_ignored(self):
        doc = rac.ledger([run(1, "2026-10-03T14:00:00Z", "skipped"),
                          run(3, "2026-10-03T15:00:00Z", branch="feature"),
                          run(5, "2026-10-02T14:00:00Z")], START, now("2026-10-04T09:00:00"))
        self.assertEqual((doc["streak"]["count"], doc["missing"]), (0, ["2026-10-03"]))

    def test_the_day_is_the_utc_date(self):
        doc = rac.ledger([run(1, "2026-10-03T23:30:00-02:00")], START, now("2026-10-05T09:00:00"))
        self.assertEqual(counted(doc), [("2026-10-04", 1)])
        self.assertEqual(doc["missing"], ["2026-10-03"])


class CliTests(unittest.TestCase):
    def test_the_cli_writes_the_ledger_and_names_missing_days(self):
        with tempfile.TemporaryDirectory() as tmp:
            runs = Path(tmp) / "runs.json"
            runs.write_text(json.dumps({"workflow_runs": [run(1, "2026-10-03T14:00:00Z"), run(2, "2026-10-05T10:00:00Z")]}))
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
