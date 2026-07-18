#!/usr/bin/env python3
"""Tests for merge_stall_watchdog.py.

The load-bearing behavior is the two-condition, two-tick predicate: only a PR
that is genuinely merge-ready (required-green + CLEAN/BEHIND + auto-merge) AND
has stayed that way across two consecutive sweeps AND for longer than the age
threshold may trip. Everything else — a young PR, a conflicted PR, a
required-check-red PR, a one-sweep blip — must stay quiet, and a merge (the PR
leaving the open set) must let the tracker close.
"""
from __future__ import annotations

import datetime as dt
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import merge_stall_watchdog as msw  # noqa: E402

NOW = dt.datetime(2026, 7, 16, 12, 0, 0, tzinfo=dt.timezone.utc)
REQUIRED = ["macos", "Enforce version & skill sync"]


def ago(minutes: float) -> str:
    return (NOW - dt.timedelta(minutes=minutes)).isoformat().replace("+00:00", "Z")


def green_checks(green_age_min: float, names=None) -> dict[str, dict]:
    """All required checks green, last one having completed green_age_min ago."""
    names = names or REQUIRED
    return {n: {"green": True, "completed_at": ago(green_age_min)} for n in names}


def pr(
    number: int,
    *,
    merge_state="CLEAN",
    auto_merge=True,
    is_draft=False,
    checks=None,
    green_age_min=60.0,
    title="a change",
) -> dict:
    return {
        "number": number,
        "title": title,
        "url": f"https://example.invalid/pr/{number}",
        "is_draft": is_draft,
        "merge_state_status": merge_state,
        "auto_merge_enabled": auto_merge,
        "checks": green_checks(green_age_min) if checks is None else checks,
    }


def snap(*prs) -> dict:
    return {
        "generated_at": NOW.isoformat(),
        "required_checks": list(REQUIRED),
        "required_checks_source": "branch-protection",
        "open_prs": list(prs),
    }


def levels(findings):
    return [f["level"] for f in findings]


class TestTrips(unittest.TestCase):
    def test_green_behind_pr_stuck_two_ticks_alarms(self):
        # The exact incident shape: a green, BEHIND, auto-merge PR that has been
        # merge-ready for an hour. First sweep: pending. Second consecutive
        # sweep: alarm.
        s = snap(pr(101, merge_state="BEHIND", green_age_min=60))

        f1, stuck1 = msw.analyze(s, prev_stuck=[], now=NOW)
        self.assertEqual(levels(f1), ["pending"])
        self.assertEqual(stuck1, [101])

        f2, stuck2 = msw.analyze(s, prev_stuck=stuck1, now=NOW)
        self.assertEqual(levels(f2), ["alarm"])
        self.assertEqual(f2[0]["number"], 101)
        self.assertEqual(f2[0]["merge_state_status"], "BEHIND")
        self.assertEqual(stuck2, [101])

    def test_clean_pr_stuck_two_ticks_alarms(self):
        s = snap(pr(102, merge_state="CLEAN", green_age_min=90))
        _, stuck1 = msw.analyze(s, prev_stuck=[], now=NOW)
        f2, _ = msw.analyze(s, prev_stuck=stuck1, now=NOW)
        self.assertEqual(levels(f2), ["alarm"])


class TestQuietCases(unittest.TestCase):
    """Every one of these MUST produce zero alarms."""

    def test_young_green_pr_does_not_trip(self):
        # Merge-ready for only 20 min: under the 45-min threshold. Not even
        # pending, no matter how many sweeps it is seen.
        s = snap(pr(201, merge_state="CLEAN", green_age_min=20))
        f1, stuck1 = msw.analyze(s, prev_stuck=[], now=NOW)
        self.assertEqual(f1, [])
        self.assertEqual(stuck1, [])
        # Even if it were somehow in the previous set, age still gates it.
        f2, _ = msw.analyze(s, prev_stuck=[201], now=NOW)
        self.assertEqual(f2, [])

    def test_dirty_pr_never_trips(self):
        # Conflicts with base. Waiting on the author, not the merger.
        s = snap(pr(202, merge_state="DIRTY", green_age_min=120))
        f1, stuck1 = msw.analyze(s, prev_stuck=[], now=NOW)
        self.assertEqual(f1, [])
        f2, _ = msw.analyze(s, prev_stuck=[202], now=NOW)
        self.assertEqual(f2, [])

    def test_blocked_pr_never_trips(self):
        # BLOCKED = a required check red/missing or a review pending. Waiting on
        # something real, not on a wedged merger.
        s = snap(pr(203, merge_state="BLOCKED", green_age_min=120))
        f2, _ = msw.analyze(s, prev_stuck=[203], now=NOW)
        self.assertEqual(f2, [])

    def test_unstable_pr_never_trips(self):
        # A non-required check still moving; required set is green but GitHub's
        # verdict is UNSTABLE, which the predicate excludes.
        s = snap(pr(204, merge_state="UNSTABLE", green_age_min=120))
        f2, _ = msw.analyze(s, prev_stuck=[204], now=NOW)
        self.assertEqual(f2, [])

    def test_required_check_red_does_not_trip(self):
        # CLEAN would not actually happen with a red required check, but guard
        # the predicate directly: one required check not green -> not stuck.
        checks = green_checks(90)
        checks["macos"] = {"green": False, "completed_at": ago(90)}
        s = snap(pr(205, merge_state="CLEAN", checks=checks, green_age_min=90))
        f2, _ = msw.analyze(s, prev_stuck=[205], now=NOW)
        self.assertEqual(f2, [])

    def test_missing_required_check_does_not_trip(self):
        # A required check absent entirely (pending / never started) is not
        # green. Only the non-required checks are present.
        checks = {"Enforce version & skill sync": {"green": True, "completed_at": ago(90)}}
        s = snap(pr(206, merge_state="CLEAN", checks=checks, green_age_min=90))
        f2, _ = msw.analyze(s, prev_stuck=[206], now=NOW)
        self.assertEqual(f2, [])

    def test_no_auto_merge_does_not_trip(self):
        # Green + CLEAN but auto-merge not enabled: waiting on a human to press
        # merge, which is not a wedged-merger stall.
        s = snap(pr(207, merge_state="CLEAN", auto_merge=False, green_age_min=120))
        f2, _ = msw.analyze(s, prev_stuck=[207], now=NOW)
        self.assertEqual(f2, [])

    def test_draft_does_not_trip(self):
        s = snap(pr(208, merge_state="CLEAN", is_draft=True, green_age_min=120))
        f2, _ = msw.analyze(s, prev_stuck=[208], now=NOW)
        self.assertEqual(f2, [])

    def test_single_tick_blip_stays_pending(self):
        # Qualifies this sweep but was NOT stuck last sweep: pending only, never
        # alarm. This is the anti-flap guard against a transient false reading.
        s = snap(pr(209, merge_state="CLEAN", green_age_min=90))
        f1, _ = msw.analyze(s, prev_stuck=[], now=NOW)
        self.assertEqual(levels(f1), ["pending"])
        self.assertNotIn("alarm", levels(f1))


class TestRecovery(unittest.TestCase):
    def test_merge_clears_the_finding(self):
        # Two sweeps stuck -> alarm. Then the PR merges: it leaves the open set,
        # so the third sweep produces zero findings and an empty stuck set,
        # which is what lets the workflow close the tracker.
        s = snap(pr(301, merge_state="BEHIND", green_age_min=60))
        _, stuck1 = msw.analyze(s, prev_stuck=[], now=NOW)
        f2, stuck2 = msw.analyze(s, prev_stuck=stuck1, now=NOW)
        self.assertEqual(levels(f2), ["alarm"])

        merged = snap()  # the stuck PR is no longer in the open set
        f3, stuck3 = msw.analyze(merged, prev_stuck=stuck2, now=NOW)
        self.assertEqual(f3, [])
        self.assertEqual(stuck3, [])

    def test_pr_becomes_dirty_clears_the_finding(self):
        # Recovery need not be a merge: if the stuck PR goes DIRTY (someone
        # pushed a conflicting base), it drops out of the stuck set and the
        # tracker can close.
        stuck = snap(pr(302, merge_state="CLEAN", green_age_min=90))
        _, stuck1 = msw.analyze(stuck, prev_stuck=[], now=NOW)
        f2, _ = msw.analyze(stuck, prev_stuck=stuck1, now=NOW)
        self.assertEqual(levels(f2), ["alarm"])

        now_dirty = snap(pr(302, merge_state="DIRTY", green_age_min=95))
        f3, stuck3 = msw.analyze(now_dirty, prev_stuck=stuck1, now=NOW)
        self.assertEqual(f3, [])
        self.assertEqual(stuck3, [])


class TestRequiredCheckResolution(unittest.TestCase):
    def test_snapshot_required_set_is_honored(self):
        # The predicate uses whatever required set the snapshot carries (read
        # from branch protection at runtime), not a hardcoded list. A PR green
        # on the documented set but NOT on a custom required set does not trip.
        s = snap(pr(401, merge_state="CLEAN", green_age_min=90))
        s["required_checks"] = ["macos", "Enforce version & skill sync", "linux"]
        f2, _ = msw.analyze(s, prev_stuck=[401], now=NOW)
        self.assertEqual(f2, [])  # "linux" is required but not present/green

    def test_default_required_set_used_when_snapshot_omits_it(self):
        s = {"open_prs": [pr(402, merge_state="CLEAN", green_age_min=90)]}
        _, stuck1 = msw.analyze(s, prev_stuck=[], now=NOW)
        f2, _ = msw.analyze(s, prev_stuck=stuck1, now=NOW)
        self.assertEqual(levels(f2), ["alarm"])


class TestMultiPrPopulation(unittest.TestCase):
    def test_alarms_and_pendings_coexist_sorted(self):
        stuck_old = pr(501, merge_state="BEHIND", green_age_min=120)
        stuck_new = pr(502, merge_state="CLEAN", green_age_min=60)
        young = pr(503, merge_state="CLEAN", green_age_min=10)
        dirty = pr(504, merge_state="DIRTY", green_age_min=120)
        s = snap(stuck_old, stuck_new, young, dirty)

        # Previous sweep only saw 501 as stuck -> 501 alarms, 502 pending,
        # 503 too young, 504 dirty.
        findings, stuck_now = msw.analyze(s, prev_stuck=[501], now=NOW)
        self.assertEqual(levels(findings), ["alarm", "pending"])
        self.assertEqual([f["number"] for f in findings], [501, 502])
        self.assertEqual(stuck_now, [501, 502])


class TestFutureStub(unittest.TestCase):
    def test_merge_queue_stub_is_inert(self):
        self.assertEqual(msw.merge_queue_stall_todo({}, NOW), [])


class TestMainCli(unittest.TestCase):
    def test_snapshot_and_prev_state_roundtrip_writes_alarm(self):
        # Drive main() end-to-end over files, as the workflow does: a stuck PR
        # whose number is in the prev-state file must produce an alarm body and
        # a state file naming it.
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "snap.json").write_text(
                json.dumps(snap(pr(601, merge_state="BEHIND", green_age_min=90)))
            )
            (d / "prev.json").write_text(json.dumps({"stuck_prs": [601]}))
            rc = msw.main(
                [
                    "--snapshot", str(d / "snap.json"),
                    "--prev-state", str(d / "prev.json"),
                    "--findings-out", str(d / "findings.json"),
                    "--state-out", str(d / "state.json"),
                    "--body-out", str(d / "body.md"),
                ]
            )
            self.assertEqual(rc, 0)
            findings = json.loads((d / "findings.json").read_text())
            self.assertEqual([f["level"] for f in findings], ["alarm"])
            state = json.loads((d / "state.json").read_text())
            self.assertEqual(state["stuck_prs"], [601])
            body = (d / "body.md").read_text()
            self.assertIn("#601", body)
            self.assertIn("merge-stall-check.yml", body)

    def test_no_findings_writes_no_body(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "snap.json").write_text(
                json.dumps(snap(pr(602, merge_state="DIRTY", green_age_min=90)))
            )
            rc = msw.main(
                [
                    "--snapshot", str(d / "snap.json"),
                    "--findings-out", str(d / "findings.json"),
                    "--state-out", str(d / "state.json"),
                    "--body-out", str(d / "body.md"),
                ]
            )
            self.assertEqual(rc, 0)
            self.assertFalse((d / "body.md").exists())
            self.assertEqual(json.loads((d / "state.json").read_text())["stuck_prs"], [])


class TestChecksFromRollup(unittest.TestCase):
    """The GraphQL-rollup flattener that feeds analyze() in the live path."""

    def test_checkrun_success_is_green(self):
        commit = {
            "statusCheckRollup": {
                "contexts": {
                    "nodes": [
                        {
                            "__typename": "CheckRun",
                            "name": "macos",
                            "status": "COMPLETED",
                            "conclusion": "SUCCESS",
                            "completedAt": ago(50),
                        }
                    ]
                }
            }
        }
        checks = msw._checks_from_rollup(commit)
        self.assertTrue(checks["macos"]["green"])

    def test_checkrun_failure_is_not_green(self):
        commit = {
            "statusCheckRollup": {
                "contexts": {
                    "nodes": [
                        {
                            "__typename": "CheckRun",
                            "name": "macos",
                            "status": "COMPLETED",
                            "conclusion": "FAILURE",
                            "completedAt": ago(50),
                        }
                    ]
                }
            }
        }
        self.assertFalse(msw._checks_from_rollup(commit)["macos"]["green"])

    def test_in_progress_checkrun_is_not_green(self):
        commit = {
            "statusCheckRollup": {
                "contexts": {
                    "nodes": [
                        {
                            "__typename": "CheckRun",
                            "name": "macos",
                            "status": "IN_PROGRESS",
                            "conclusion": None,
                            "completedAt": None,
                        }
                    ]
                }
            }
        }
        self.assertFalse(msw._checks_from_rollup(commit)["macos"]["green"])


if __name__ == "__main__":
    unittest.main()
